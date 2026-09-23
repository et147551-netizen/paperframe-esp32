// Read-only SMB2 client: one share, one directory, one file at a time.
//
// Ticket .scratch/digital-frame/issues/24, for the NAS mirror in ticket 25. No writes,
// no share enumeration, no discovery, no SMB1 -- every modern server refuses SMB1 by
// default and both WSL Android implementations converged on dropping it.
//
// libsmb2 is LGPLv2.1, unlike the rest of this project. See src/idf_component.yml.
//
// Reads stall, and no configuration known here stops it. That is measured, and it is why
// this module is built on libsmb2's ASYNC API with deadlines of its own rather than on
// the synchronous calls that look like the obvious choice.
//
//   * A 16 KB or 32 KB read stalls on its first PDU, deterministically, which is why
//     BOARD_SMB_READ_CHUNK is 4096. A 4 KB read still stalls mid-file roughly once every
//     ~175 PDUs -- about one file in four or five -- and raising
//     CONFIG_LWIP_TCP_WND_DEFAULT from 5760 to 16384 did NOT measurably change that rate.
//     The 10/10 run that once said otherwise was a single run and does not reproduce;
//     ticket 23's correction of 2026-09-04 has the seven runs, including two of its own
//     unmodified probe stalling in rounds 1 and 2. During a stall nothing arrives at all:
//     20,921 select() samples, zero readable, SO_ERROR 0, socket ESTABLISHED. So no
//     client-side change recovers the transfer -- what a caller needs is a deadline.
//
//   * smb2_set_timeout() fires for connect (measured at 11.11 s for a dead host) and
//     cannot fire for a stalled mid-file read. This is not a quirk, it is structural.
//     smb2_timeout_pdus() scans exactly two lists, smb2->outqueue and smb2->waitqueue
//     (managed_components/sahlberg__libsmb2/lib/pdu.c:548), and lib/socket.c:410 takes a
//     PDU off waitqueue the moment its response header decodes, parking it in the single
//     slot smb2->pdu while the payload is read. lib/init.c:326-344 confirms the slot is a
//     third place: destroy_context calls the callback for everything on the two queues
//     and frees smb2->pdu without calling its own. A command stalled between header and
//     payload is therefore in none of the places the timeout sweep looks, and no value of
//     smb2_set_timeout() would ever have rescued one.
//
// Hence: every call here runs its own poll()/smb2_service() loop against a wall-clock
// deadline, which covers every internal state including mid-payload, and abandons by
// destroying the context from a frame that is not inside libsmb2. board_smb_read_file()
// then reconnects and resumes from its saved offset, so a stall costs a retry -- a 59 ms
// reconnect and a 1.9 s re-read, against a 15.6 s panel refresh -- instead of the whole
// device. Nothing in this module can block indefinitely.

#ifndef BOARD_SMB_H
#define BOARD_SMB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_smb_classify.h"

#define BOARD_SMB_HOST_SIZE 64
#define BOARD_SMB_SHARE_SIZE 64
#define BOARD_SMB_PATH_SIZE 128
#define BOARD_SMB_USER_SIZE 64
#define BOARD_SMB_PASS_SIZE 64
#define BOARD_SMB_NAME_SIZE 256

// 4096, from the measurement above. Not a tuning knob to raise casually.
#define BOARD_SMB_READ_CHUNK 4096

// How many 4 KB preads are outstanding at once. The chunk size is fixed by the stall above,
// so the only lever on the round trip is to stop waiting for each reply before asking for
// the next. libsmb2 supports it directly -- smb2_write_to_socket() drains the whole outqueue
// per service call and replies are matched by message id -- and it costs a read_data plus a
// pdu per slot rather than the ~34.8 KB of internal RAM that raising the TCP receive window
// costs (ticket 27).
//
// IT IS WORTH 1.33x AND NO MORE, and ticket 33 has the reason: the round trip was never
// mostly network. 82 % of a 4 KB read was SMB3 signature verification -- libsmb2 verifies
// every received PDU (lib/socket.c:567-582) with AES-CMAC while re-expanding the AES key for
// every 16-byte block (lib/aes.c:444-456), measured at 4.895 us per byte. So pipelining
// recovers the queueing and network legs and then flattens. src/smb_aes_hw.c is where the
// rest went.
//
// AND IT IS 1 IN THE SHIPPING BUILD, because more than one reply in flight is only correct
// at a TCP receive window this application does not have. At `WND 16384 / mbox 16` --
// env:smbprobe's -- depths 1 to 16 all read the same 155,803 bytes to the same sha256, ten
// times each, across three runs. At `WND 5760 / mbox 6` -- what sdkconfig.frame carries --
// depth 4 raises `smb2_service: Wrong signature in received PDU` within three groups, three
// times in one read, and the read fails. The window is the only variable between those two
// results. Ticket 33 has the logs; the mechanism inside libsmb2's receive path is not
// established, and nothing here should assume it is a corruption rather than a desync.
//
// Deeper is not free in RAM either: the sweep's internal-RAM low-water mark fell as depth
// rose, which is arriving payloads queued as lwIP pbufs, and env:frame idles at
// int_free ~72 KB. Raising the window costs another ~34.8 KB on top (ticket 27).
//
// So the depth is a lever that exists, is measured, and stays at 1 until somebody raises the
// window deliberately and re-runs the sweep. Set at runtime, not by a build flag, so that
// sweep is one flash on one association.
//
// One consequence to know before trusting the group code: AT DEPTH 1 THE INTERESTING PARTS OF
// IT NEVER RUN. A single-slot group has a trivial contiguous prefix, so the out-of-order and
// hole-recovery logic in read_group_locked() is exercised only by env:smbprobe's sweep and not
// by the shipping application. It is kept because it is the instrument for an open question,
// not because it is on a tested path.
#define BOARD_SMB_READ_DEPTH 1
#define BOARD_SMB_READ_DEPTH_MAX 16

void board_smb_set_read_depth(int depth);
int board_smb_read_depth(void);

// Which SMB dialect to ask for; 0 means whatever the server picks, which is the library's
// default and what the frame ships. It exists because the dialect chooses the SIGNATURE
// ALGORITHM and this server requires signing: at 3.x libsmb2 verifies every received PDU
// with software AES-128-CMAC that re-expands the key for each 16-byte block
// (lib/smb2-signing.c:170-192, lib/aes.c:444-456), and at 2.1 it uses HMAC-SHA256 instead.
// Ticket 33 measured that verification as 21 of the 29 ms a 4 KB read costs, so which
// branch runs is not a detail. Pass an enum smb2_negotiate_version value.
void board_smb_set_dialect(int version);

// The @@SMBCHUNK trace, on at boot when -DBOARD_SMB_TRACE_CHUNKS is set and a no-op
// otherwise. It has to be switchable at runtime because it is one printf per GROUP: at
// depth 1 that is one per 4 KB and at depth 16 one per 64 KB, so leaving it on across a
// depth sweep would credit the deeper arms with the serial traffic it saved. The
// decomposition arm turns it on; anything measuring a rate turns it off.
void board_smb_set_trace(bool on);

// The deadlines. Each is a wall-clock ceiling on one libsmb2 command, and each carries
// the measurement it comes from rather than a round number someone liked.
//
//   CONNECT  a dead host is now detected here rather than by libsmb2's own connect
//            timeout, which this module's service loop bypasses: 12.02 s measured
//            2026-09-04, against the 11.11 s of the synchronous path. A deadline in this
//            phase reports ERR_CONNECT, not ERR_TIMEOUT -- see abandon_locked().
//   LIST     a 129-entry listing took 765 ms (ticket 23); this is 20x that, because
//            smb2_opendir materialises the whole directory before it returns.
//   OPEN     a create round trip is one PDU at a measured ~45 ms.
//   CHUNK    ~66x the measured 45 ms median round trip for a 4 KB read. This is the one
//            that turns a stall into a retry, so it is the one that matters: too tight
//            and a merely slow share is treated as dead, too loose and the mirror spends
//            the time anyway.
//   CLOSE    a close on a session that is about to be dropped is not worth waiting for.
//   FILE     one budget for a whole board_smb_read_file() call including its retries.
//            The 4 MB per-file ceiling is ~48 s at the measured 84 KB/s.
#define BOARD_SMB_CONNECT_DEADLINE_MS 12000
#define BOARD_SMB_LIST_DEADLINE_MS 15000
#define BOARD_SMB_OPEN_DEADLINE_MS 5000
#define BOARD_SMB_CHUNK_DEADLINE_MS 3000
#define BOARD_SMB_CLOSE_DEADLINE_MS 2000
#define BOARD_SMB_FILE_DEADLINE_MS 120000

typedef struct {
    char host[BOARD_SMB_HOST_SIZE];
    // No port field: libsmb2 3.0.1 exposes no setter for it, so a configurable port
    // would be a setting that silently does nothing. It connects on 445.
    char share[BOARD_SMB_SHARE_SIZE];
    char path[BOARD_SMB_PATH_SIZE]; // directory inside the share, "" for its root
    char user[BOARD_SMB_USER_SIZE];
    char password[BOARD_SMB_PASS_SIZE];
    char domain[BOARD_SMB_USER_SIZE];
    bool enabled;
} board_smb_config_t;

typedef struct {
    char name[BOARD_SMB_NAME_SIZE];
    uint64_t size;
    uint64_t mtime;
} board_smb_entry_t;

// Connects and tree-connects. On failure nothing is cached: the context is destroyed and
// the next call starts clean. ComittoNxA cached a null session on failure and stayed
// broken until restart -- 「失敗をキャッシュへ書くと次回以降も壊れたままになる」.
board_smb_err_t board_smb_connect(const board_smb_config_t *cfg);

// Decides whether an entry is worth one of the caller's `max` slots. `name` is a bare
// file name. NULL accepts everything.
typedef bool (*board_smb_accept_fn)(const char *name);

// Regular files in cfg->path, sorted by nothing -- the caller sorts. A genuinely empty
// directory returns BOARD_SMB_OK with *count == 0; conflating that with a failure is the
// bug ComittoNxA shipped in 1c6e844, and treating a failure as empty is its equal and
// opposite. Directories and the . / .. entries are skipped.
//
// `accept` runs BEFORE the cap, so `max` is a budget of entries the caller wants rather
// than of whatever the directory happens to hold. It matters: the real share on this
// bench has 1,070 files at its root, and a folder whose first `max` entries are archives
// and sidecars used to yield zero photographs (ticket 27).
//
// `truncated` -- both it and `accept` may be NULL -- is set when an accepted entry
// arrives with `out` already full. **A truncated listing is not evidence that anything is
// absent from the share**, so a caller that reconciles deletions against one has to stop
// doing that; app_smb_sync deleted photographs the share still held until it did.
board_smb_err_t board_smb_list(board_smb_entry_t *out, size_t max, size_t *count,
                               board_smb_accept_fn accept, bool *truncated);

// What a listing is looking for. FILES is what board_smb_list() has always done -- regular
// files, "." and ".." skipped -- and DIRS is the same walk keeping the directories instead.
//
// DIRS exists so the mirror can find out what folders a share has (ticket 37). It is NOT
// cheaper than FILES and the distinction matters: smb2_opendir materialises every entry
// whatever we then keep, at a measured 142.5 bytes each, so asking for the 8 directories of a
// 1,080-entry root still costs 153,784 B and still cannot be done on env:frame. Discovery is
// affordable only where the parent folder is itself small.
typedef enum {
    BOARD_SMB_LIST_FILES = 0,
    BOARD_SMB_LIST_DIRS,
} board_smb_list_what_t;

// The general form. `path` is a directory inside the share, or NULL for the connected
// configuration's own path -- which is what board_smb_list() passes, so the two cannot drift.
// Everything else means what it does there.
//
// Taking a path rather than reconnecting is the point: a catalogue that walks six folders would
// otherwise pay six sessions, and board_smb_connect() tears the session down and builds a new
// one every time it is called.
board_smb_err_t board_smb_list_path(const char *path, board_smb_list_what_t what,
                                    board_smb_entry_t *out, size_t max, size_t *count,
                                    board_smb_accept_fn accept, bool *truncated);

// What the last board_smb_list() cost. Instrument for ticket 37's Phase 0: whether a share
// with ~1,070 entries can be listed inside env:frame's headroom at all, which decides whether
// the frame can hold an index of a whole share or only a moving window onto one.
//
// `dirents` IS THE DIRECTORY'S OWN COUNT, NOT WHAT SURVIVED THE CAP, and the distinction is
// the whole point of the struct: smb2_opendir materialises the entire directory before the
// first smb2_readdir() returns, so the RAM is a function of `dirents` while every figure
// recorded here so far was labelled with the *kept* count. That conflation made one 243-entry
// measurement read as a 200-entry one and the per-entry slope look ~25x steeper than it is
// (docs/agents/measurements.md, the listing-transient section).
//
// The internal-RAM samples bracket the materialisation: `open` is taken the instant
// smb2_opendir returns, which is the peak, and int_min before/after brackets the trough the
// call reached. Raw numbers, no verdict -- ticket 03 cost a re-run to a derived boolean.
typedef struct {
    uint32_t dirents;  // every entry smb2_readdir returned, "." ".." and directories included
    uint32_t files;    // of those, regular files other than "." and ".."
    uint32_t accepted; // of those, what `accept` kept (== *count unless the cap truncated)
    uint32_t open_ms;  // smb2_opendir alone, which is where the round trips and the RAM are
    uint32_t walk_ms;  // the readdir walk plus closedir; issues no request
    uint32_t int_free_before, int_free_open, int_free_after;
    uint32_t int_min_before, int_min_after;
    uint32_t dma_largest_before, dma_largest_after;
} board_smb_list_stats_t;

// Zeroed at the start of every board_smb_list(); meaningful after one returns BOARD_SMB_OK.
void board_smb_get_list_stats(board_smb_list_stats_t *out);

// Reads one file from cfg->path into `buf`. `name` is a bare file name, not a path.
// *len is what was read; a file larger than `max` returns BOARD_SMB_ERR_NOTFOUND rather
// than a truncated image the caller cannot tell from a whole one. A file of exactly `max`
// bytes fits and succeeds -- the boundary is measured, not assumed (2026-09-04).
//
// A read that stalls is retried up to BOARD_SMB_READ_ATTEMPTS times, reconnecting and
// resuming from the offset already read. When even that runs out, the call returns
// BOARD_SMB_ERR_TIMEOUT within BOARD_SMB_FILE_DEADLINE_MS -- it does not hang, and the
// caller's correct response is "this file failed", not "this file is slow".
board_smb_err_t board_smb_read_file(const char *name, uint8_t *buf, size_t max,
                                    size_t *len);

// The same read, but `path` is the FULL path inside the share rather than a leaf inside
// cfg->path. The on-demand fetch needs it (ticket 37 Phase 3): a photograph chosen from the
// catalogue can be in any folder of smb_path's list, while cfg->path is only element 0. Passing
// "" or NULL means the share root.
//
// The pair is the same shape as board_smb_list() over board_smb_list_path(), and for the same
// reason: the folder-relative form is what every existing caller wants and prepending the
// configured path in two places is how the two drift apart.
board_smb_err_t board_smb_read_file_path(const char *path, uint8_t *buf, size_t max,
                                         size_t *len);

void board_smb_disconnect(void);

bool board_smb_is_connected(void);

// The session's socket, for diagnostics only -- watching a read that has stalled from
// another task. It deliberately does NOT take the module's lock: a read holds that lock for
// its whole duration, so a watcher that took it would block until the very thing it is
// meant to observe finished. It is therefore a snapshot, valid while a session is up, and
// -1 when there is none. Nothing in the mirror should route work through it.
int board_smb_fd(void);

// The libsmb2 context, as an opaque pointer, for diagnostics only -- and, like
// board_smb_fd(), without taking the module's lock, so it is a snapshot and NULL when
// there is no session.
//
// It exists for one question that cannot be asked any other way: whether a stalled read is
// stalled mid-payload, which is what the comment at the top of this file claims from
// reading libsmb2's source and has not yet confirmed on hardware. env:smbprobe casts this
// back and reads three fields of the private struct. Nothing else may use it, and nothing
// at all may write through it.
void *board_smb_context(void);

#endif // BOARD_SMB_H
