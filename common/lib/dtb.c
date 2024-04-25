#include <common/lib/misc.h>
#include <stdbool.h>

#if defined (UEFI)

#include <efi.h>

void *get_dtb(void) {
    const EFI_GUID dtb_guid = EFI_DTB_TABLE_GUID;
    static void *dtb = NULL;
    static bool checked_dtb = false;

    // Only try to get the DTB once
    if (checked_dtb) {
        return dtb;
    }

    // Look for the DTB in the configuration tables
    for (size_t i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *cur_table = &gST->ConfigurationTable[i];

        if (memcmp(&cur_table->VendorGuid, &dtb_guid, sizeof(EFI_GUID)) == 0) {
            dtb = cur_table->VendorTable;
            break;
        }
    }

    checked_dtb = true;
    return dtb;
}



#endif