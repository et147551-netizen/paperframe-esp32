// The catalogue the slideshow selects from: every photograph that can be fetched on demand, from
// every source that fetches on demand.
//
// Ticket 60, FR-10.3. Until 2026-09-13 the slideshow's catalogue epoch called the SMB module
// directly, so the only on-demand source there could be was an SMB share. The Google Photos album
// became a second one when the owner asked for its photographs to be fetched one at a time
// like the share's rather than mirrored ("SMBと同じく都度"), and both have to be selectable
// without either being privileged.
//
// Indices are CONCATENATED: [0, smb) is the SMB share's catalogue and [smb, smb + google) is the
// album's list. The SMB part counts only while `smb_enabled` and `smb_on_demand` are both on -- a
// share that is switched off would otherwise go on being selected, and every selection of it would
// miss. A change in either count moves the boundary, and a count change already regenerates the
// slideshow's permutation, so nothing holds an index across it.
//
// The contract is app_smb_sync_catalog_*'s, unchanged: no filesystem I/O, and `want` names only
// entries the caller has found absent from /data.
//
// Called from the main task. Nothing here may put a large buffer on the stack.

#ifndef APP_CATALOG_H
#define APP_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t app_catalog_count(void);
bool app_catalog_local(size_t i, char *local, size_t local_size);
void app_catalog_want(const uint16_t *index, size_t n);

#endif // APP_CATALOG_H
