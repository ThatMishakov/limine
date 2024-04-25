
#if defined(__riscv)

#include <lib/acpi.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/dtb.h>
#include <sys/cpu.h>
#include <mm/pmm.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/smoldtb.h>

// ACPI RISC-V Hart Capabilities Table
struct rhct {
    struct sdt header;
    uint32_t flags;
    uint64_t time_base_frequency;
    uint32_t nodes_len;
    uint32_t nodes_offset;
    uint8_t nodes[];
} __attribute__((packed));

#define RHCT_ISA_STRING 0
#define RHCT_CMO        1
#define RHCT_MMU        2
#define RHCT_HART_INFO  65535

struct rhct_header {
    uint16_t type;      // node type
    uint16_t size;      // node size (bytes)
    uint16_t revision;  // node revision
} __attribute__((packed));

// One `struct rhct_hart_info` structure exists per hart in the system.
// The `offsets` array points to other entries in the RHCT associated with the
// hart.
struct rhct_hart_info {
    struct rhct_header header;
    uint16_t offsets_len;
    uint32_t acpi_processor_uid;
    uint32_t offsets[];
} __attribute__((packed));

struct rhct_isa_string {
    struct rhct_header header;
    uint16_t isa_string_len;
    const char isa_string[];
} __attribute__((packed));

#define RISCV_MMU_TYPE_SV39 0
#define RISCV_MMU_TYPE_SV48 1
#define RISCV_MMU_TYPE_SV57 2

struct rhct_mmu {
    struct rhct_header header;
    uint8_t reserved0;
    uint8_t mmu_type;
} __attribute__((packed));

size_t bsp_hartid;
struct riscv_hart *hart_list;
static struct riscv_hart *bsp_hart;

static struct riscv_hart *riscv_get_hart(size_t hartid) {
    for (struct riscv_hart *hart = hart_list; hart != NULL; hart = hart->next) {
        if (hart->hartid == hartid) {
            return hart;
        }
    }
    panic(false, "no `struct riscv_hart` for hartid %u", hartid);
}

static inline struct rhct_hart_info *rhct_get_hart_info(struct rhct *rhct, uint32_t acpi_uid) {
    uint32_t offset = rhct->nodes_offset;
    for (uint32_t i = 0; i < rhct->nodes_len; i++) {
        struct rhct_hart_info *node = (void *)((uintptr_t)rhct + offset);
        if (node->header.type == RHCT_HART_INFO && node->acpi_processor_uid == acpi_uid) {
            return node;
        }
        offset += node->header.size;
    }
    return NULL;
}

void dtb_error(const char *why) {
    panic(false, "dtb: %s", why);
}

void init_riscv(void) {
    struct madt *madt = acpi_get_table("APIC", 0);
    struct rhct *rhct = acpi_get_table("RHCT", 0);
    if (madt != NULL && rhct != NULL) {
        for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
            (uintptr_t)madt_ptr < (uintptr_t)madt + madt->header.length; madt_ptr += *(madt_ptr + 1)) {
            if (*madt_ptr != 0x18) {
                continue;
            }
            struct madt_riscv_intc *intc = (struct madt_riscv_intc *)madt_ptr;

            // Ignore harts we can't do anything with.
            if (!(intc->flags & MADT_RISCV_INTC_ENABLED ||
                    intc->flags & MADT_RISCV_INTC_ONLINE_CAPABLE)) {
                continue;
            }

            uint32_t acpi_uid = intc->acpi_processor_uid;
            size_t hartid = intc->hartid;

            struct rhct_hart_info *hart_info = rhct_get_hart_info(rhct, acpi_uid);
            if (hart_info == NULL) {
                panic(false, "riscv: missing rhct node for hartid %u", hartid);
            }

            const char *isa_string = NULL;
            uint8_t mmu_type = 0;
            uint8_t flags = 0;

            for (uint32_t i = 0; i < hart_info->offsets_len; i++) {
                const struct rhct_header *node = (void *)((uintptr_t)rhct + hart_info->offsets[i]);
                switch (node->type) {
                    case RHCT_ISA_STRING:
                        isa_string = ((struct rhct_isa_string *)node)->isa_string;
                        break;
                    case RHCT_MMU:
                        mmu_type = ((struct rhct_mmu *)node)->mmu_type;
                        flags |= RISCV_HART_HAS_MMU;
                        break;
                }
            }

            if (isa_string == NULL) {
                print("riscv: missing isa string for hartid %u, skipping.\n", hartid);
                continue;
            }

            if (strncmp("rv64", isa_string, 4) && strncmp("rv32", isa_string, 4)) {
                print("riscv: skipping hartid %u with invalid isa string: %s", hartid, isa_string);
            }

            struct riscv_hart *hart = ext_mem_alloc(sizeof(struct riscv_hart));
            if (hart == NULL) {
                panic(false, "out of memory");
            }

            hart->hartid = hartid;
            hart->acpi_uid = acpi_uid;
            hart->isa_string = isa_string;
            hart->mmu_type = mmu_type;
            hart->flags = flags;

            hart->next = hart_list;
            hart_list = hart;

            if (hart->hartid == bsp_hartid) {
                bsp_hart = hart;
            }
        }
    } else {
        void *dtb = get_dtb();
        if (dtb == NULL) {
            panic(false, "riscv: no DTB and ACPI tables found");
        }
    
        // TODO: Move this to lib/dtb.c
        dtb_ops ops = {
            .malloc = ext_mem_alloc,
            .free = NULL,
            .on_error = dtb_error,
        };
        dtb_init((uintptr_t)dtb, ops);

        dtb_node *cpus = dtb_find("cpus");
        if (cpus == NULL) {
            panic(false, "riscv: no cpus node found in DTB");
        }

        dtb_node *cpu = dtb_find_child(cpus, "cpu");
        while (cpu != NULL) {
            dtb_node_stat stat;
            dtb_stat_node(cpu, &stat);

            if (strncmp(stat.name, "cpu", 3)) {
                cpu = dtb_get_sibling(cpu);
                continue;
            }

            const char * isa_string = NULL;
            dtb_prop *isa = dtb_find_prop(cpu, "riscv,isa");
            if (isa != NULL) {
                isa_string = dtb_read_string(isa, 0);
            }

            uint32_t hartid = 0;
            dtb_prop *hartid_prop = dtb_find_prop(cpu, "reg");
            if (hartid_prop != NULL) {
                size_t val = 0;
                if (dtb_read_prop_values(hartid_prop, 1, &val) != 1) {
                    panic(false, "riscv: invalid reg property in cpu node");
                }
                hartid = val;
            } else {
                panic(false, "riscv: no reg property found in cpu node");
            }

            int mmu_type = 0;
            dtb_prop *mmu = dtb_find_prop(cpu, "mmu-type");
            if (mmu != NULL) {
                int index = 0;
                const char * mmu_string = dtb_read_string(mmu, index);
                while (mmu_string != NULL) {
                    if (!strcmp(mmu_string, "riscv,sv39") && mmu_type < 0) {
                        mmu_type = 0;
                    } else if (!strcmp(mmu_string, "riscv,sv48") && mmu_type < 1) {
                        mmu_type = 1;
                    } else if (!strcmp(mmu_string, "riscv,sv57") && mmu_type < 2) {
                        mmu_type = 2;
                    }

                    index++;
                    mmu_string = dtb_read_string(mmu, index);
                }
            }


            struct riscv_hart *hart = ext_mem_alloc(sizeof(struct riscv_hart));
            if (hart == NULL) {
                panic(false, "out of memory");
            }

            hart->hartid = hartid;
            hart->acpi_uid = 0;
            hart->isa_string = isa_string;
            hart->mmu_type = mmu_type;
            hart->flags = mmu_type;

            hart->next = hart_list;
            hart_list = hart;

            if (hart->hartid == bsp_hartid) {
                bsp_hart = hart;
            }

            cpu = dtb_get_sibling(cpu);
        }
    }

    if (bsp_hart == NULL) {
        panic(false, "riscv: missing `struct riscv_hart` for BSP");
    }

    if (strncasecmp(bsp_hart->isa_string, "rv64i", 5)) {
        panic(false, "unsupported cpu: %s", bsp_hart->isa_string);
    }

    for (struct riscv_hart *hart = hart_list; hart != NULL; hart = hart->next) {
        if (hart != bsp_hart && strcmp(bsp_hart->isa_string, hart->isa_string)) {
            hart->flags |= RISCV_HART_COPROC;
        }
    }
}

struct isa_extension {
    const char *name;
    size_t name_len;
    uint32_t ver_maj;
    uint32_t ver_min;
};

// Parse the next sequence of digit characters into an integer.
static bool parse_number(const char **s, size_t *_n) {
    size_t n = 0;
    bool parsed = false;
    while (isdigit(**s)) {
        n *= 10;
        n += *(*s)++ - '0';
        parsed = true;
    }
    *_n = n;
    return parsed;
}

// Parse the next extension from an ISA string.
static bool parse_extension(const char **s, struct isa_extension *ext) {
    if (**s == '\0') {
        return false;
    }

    const char *name = *s;
    size_t name_len = 1;
    if (**s == 's' || **s == 'S' || **s == 'x' || **s == 'X' || **s == 'z' || **s == 'Z') {
        while (isalpha((*s)[name_len])) {
            name_len++;
        }
    }
    *s += name_len;

    size_t maj = 0, min = 0;
    if (parse_number(s, &maj)) {
        if (**s == 'p') {
            *s += 1;
            parse_number(s, &min);
        }
    }

    while (**s == '_') {
        *s += 1;
    }

    if (ext) {
        ext->name = name;
        ext->name_len = name_len;
        ext->ver_maj = maj;
        ext->ver_min = min;
    }
    return true;
}

static bool extension_matches(const struct isa_extension *ext, const char *name) {
    for (size_t i = 0; i < ext->name_len; i++) {
        const char c1 = tolower(ext->name[i]);
        const char c2 = tolower(*name++);
        if (c2 == '\0' || c1 != c2) {
            return false;
        }
    }
    // Make sure `name` is not longer.
    return *name == '\0';
}

bool riscv_check_isa_extension_for(size_t hartid, const char *name, size_t *maj, size_t *min) {
    // Skip the `rv{32,64}` prefix so it's not parsed as extensions.
    const char *isa_string = riscv_get_hart(hartid)->isa_string + 4;

    struct isa_extension ext;
    while (parse_extension(&isa_string, &ext)) {
        if (!extension_matches(&ext, name)) {
            continue;
        }
        if (maj) {
            *maj = ext.ver_maj;
        }
        if (min) {
            *min = ext.ver_min;
        }
        return true;
    }

    return false;
}

#endif