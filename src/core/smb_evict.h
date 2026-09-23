// "May this cached photograph be deleted?" -- the eviction walk's protected set.
//
// Extracted from app_smb_sync.c's evict_to_cache_limit() on 2026-09-21. **The walk itself stays
// where it was and should**: it deletes files, mutates the manifest it is iterating, takes the
// module lock and logs. What was pure and buried inside it is this one question, and it is the half
// that carries the rule -- never evict what the panel is showing, and never evict something the
// selector has already asked for.
//
// Worth its own home for the reason docs/agents/smb-mirror.md gives eviction generally: the rules
// "look like tidiness and are not". Two of the three ways to get this wrong are silent and one is
// worse than silent:
//
//   * dropping the CURRENT photograph deletes the file that is on the glass, so the next render of
//     it -- a settings change, a rotation -- finds nothing and the frame goes blank;
//   * dropping the PENDING one throws away the fetch that a window just paid for, and the next
//     advance asks for it again, which is an unbounded loop of fetch-and-evict on a full cache;
//   * dropping a WANTED one strands the want: app_catalog has asked for index N, the mirror
//     fetched it, this evicts it, and the slideshow shows the matte instead.
//
// None of those is observable except on hardware, at 15 to 31 s a look, and the cache has to be at
// its ceiling first. test/test_evict is the alternative.
//
// **An empty name is not a match.** app_slideshow_state_t's current_name and pending_name are fixed
// buffers that are empty when there is nothing showing or nothing queued, and an empty string must
// not protect an entry whose name is also somehow empty -- the original wrote that as
// `ss.current_name[0] && strcmp(...) == 0` and the guard is kept here rather than being tidied into
// the comparison.

#ifndef SMB_EVICT_H
#define SMB_EVICT_H

#include <stdbool.h>
#include <stddef.h>

#include "smb_manifest.h"

// True when `local` may be deleted: it is not the photograph on the glass, not the one queued, and
// not in the protected set.
//
// `local` is a manifest local name. `current_name` and `pending_name` may be NULL or empty, which
// both mean "nothing there". `protected_names` may be NULL when `protected_count` is zero, and its
// row width is SMB_MANIFEST_NAME_SIZE so the caller can pass its stack array unchanged.
//
// False for a NULL or empty `local`: an entry with no name is not one to delete on a guess.
bool smb_evict_may_drop(const char *local, const char *current_name, const char *pending_name,
                        const char protected_names[][SMB_MANIFEST_NAME_SIZE],
                        size_t protected_count);

#endif  // SMB_EVICT_H
