#include "app_catalog.h"

#include "app_gphotos_sync.h"
#include "app_settings.h"
#include "app_smb_sync.h"
#include "smb_catalog.h"

static size_t smb_part(void)
{
#ifdef GPHOTOS_BENCH_ONLY_SLOT
    // Bench flag (app_gphotos_sync.c): the catalogue is one album slot and nothing else.
    return 0;
#endif
    return (app_settings_smb_enabled() && app_settings_smb_on_demand())
               ? app_smb_sync_catalog_count()
               : 0;
}

size_t app_catalog_count(void)
{
    const size_t n = smb_part() + app_gphotos_sync_count();
    // The slideshow's permutation holds SMB_CATALOG_MAX entries. Beyond it the tail of the album's
    // list is unreachable rather than the whole epoch failing over to filename order.
    return n > SMB_CATALOG_MAX ? SMB_CATALOG_MAX : n;
}

bool app_catalog_local(size_t i, char *local, size_t local_size)
{
    const size_t smb = smb_part();
    return i < smb ? app_smb_sync_catalog_local(i, local, local_size)
                   : app_gphotos_sync_local(i - smb, local, local_size);
}

void app_catalog_want(const uint16_t *index, size_t n)
{
    const size_t smb = smb_part();
    uint16_t to_smb[SMB_SYNC_WANT_MAX];
    uint16_t to_google[SMB_SYNC_WANT_MAX];
    size_t ns = 0, ng = 0;
    for (size_t i = 0; index && i < n; i++) {
        if (index[i] < smb) {
            if (ns < SMB_SYNC_WANT_MAX) {
                to_smb[ns++] = index[i];
            }
        } else if (ng < SMB_SYNC_WANT_MAX) {
            to_google[ng++] = (uint16_t)(index[i] - smb);
        }
    }
    // Both are told every time, an empty list included: a want list is replaced wholesale, and a
    // source left holding the previous advance's ask would fetch a photograph nobody wants now.
    app_smb_sync_want(ns ? to_smb : NULL, ns);
    app_gphotos_sync_want(ng ? to_google : NULL, ng);
}
