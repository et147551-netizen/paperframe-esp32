// The pure string half of request identity: percent-coding, and the rule that says a name from a
// request may only ever be one component of /data.
//
// Ticket 79. This was three static functions inside app_server.c, which is 181 KB of ESP-IDF and
// therefore unreachable from `pio test -e native` -- so **the actual path-traversal guard was the
// one piece of request handling with no test**, while `photo_list_is_image()` beside it had
// thirty. Nothing about the behaviour changed in the move; the tests came after it.
//
// The ORDER these are called in is the property that matters and it is not visible from here:
// a name is DECODED and then CHECKED. Checking first passes `%2e%2e%2f` straight through, which
// is the oldest traversal trick there is. app_server.c's request_file_name() is the one place
// that does both, and test/test_req_name pins the pair rather than only the parts.

#ifndef REQ_NAME_H
#define REQ_NAME_H

#include <stdbool.h>
#include <stddef.h>

// Percent-decodes `in` into `out`, which must hold `size` bytes including the terminator.
// `+` becomes a space, as it does in a query string.
//
// Returns false rather than truncating when the result would not fit, and false on a malformed
// escape (`%` at the end, or a non-hex digit after it). **Truncating would be the bug**: a name
// cut short is a different name, and the caller's next step is to decide whether it is safe.
bool req_url_decode(const char *in, char *out, size_t size);

// Percent-encodes `in` into `out` keeping RFC 3986's unreserved set (`A-Za-z0-9-_.~`) verbatim.
// Returns false if the result would not fit -- and the caller then has a photograph it cannot
// build a URL for, which is a listing that silently omits it rather than a truncated link.
bool req_url_encode(const char *in, char *out, size_t size);

// A name that came from a request may only ever be a single component of /data: non-empty, no
// `..` anywhere in it, no `/` and no `\`.
//
// It is deliberately a WHITELIST OF STRUCTURE rather than a list of bad sequences: there is no
// stripping, no canonicalising and no second pass, so there is nothing for a `....//` to survive.
// A leading dot is allowed, which is FR-4.6's ported rule -- `photo_list_is_image()` keeps
// dotfiles out of the album and the slideshow, and ticket 38 rejects them on the *mirror* side
// only. So `GET /data/.smbidx` is a 200 for an authenticated client, deliberately.
bool req_name_is_safe(const char *name);

#endif // REQ_NAME_H
