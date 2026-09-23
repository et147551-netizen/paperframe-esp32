#include "smb_evict.h"

#include <string.h>

// An empty candidate protects itself. The walk should never hand one over -- a manifest entry has a
// name -- but "delete the file called nothing" is the wrong way to find out that it did.
static bool names_match(const char *a, const char *b)
{
    return a && b && a[0] && b[0] && strcmp(a, b) == 0;
}

bool smb_evict_may_drop(const char *local, const char *current_name, const char *pending_name,
                        const char protected_names[][SMB_MANIFEST_NAME_SIZE],
                        size_t protected_count)
{
    if (!local || !local[0]) {
        return false;
    }
    if (names_match(local, current_name) || names_match(local, pending_name)) {
        return false;
    }
    if (protected_names) {
        for (size_t i = 0; i < protected_count; i++) {
            if (names_match(local, protected_names[i])) {
                return false;
            }
        }
    }
    return true;
}
