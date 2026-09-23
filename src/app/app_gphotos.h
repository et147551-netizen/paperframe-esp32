#pragma once

// Ticket 60's COMBINED arm: one HTTPS session inside env:frame, while httpd, the mirror, the
// slideshow and a panel refresh are all live.
//
// WHY THIS EXISTS SEPARATELY FROM env:gphotosprobe. That build measured the TLS increment in
// isolation and got a clean answer -- ~45-47 KB of internal RAM per open session, ~12 KB off
// dma_largest, over 20 sessions on 2026-09-12. It neither refuses FR-10 nor clears it: against
// env:frame's idle int_free of ~72 KB a 47 KB session is two thirds of the headroom, and this
// frame's existing troughs already reach 1,995 B (an upload's thumbnail sidecar) and 5,419 B
// (an on-demand SMB fetch). A session concurrent with either does not fit. **How often they
// coincide, and what happens when they do, is the question this module exists to answer** -- and
// it cannot be answered by a station-only build.
//
// IT IS NOT IN THE SHIPPING BUILD. Everything here compiles to nothing without
// -DFRAME_GPHOTOS_PROBE, which is a bench flag and carries the URL with it.
//
// NO PERMANENT TASK. docs/board-and-storage.md records that adding two small tasks was
// once enough to make httpd_start() return ESP_ERR_HTTPD_TASK -- a frame that boots with a panel,
// a slideshow and no web UI, on one easily-missed log line. So a fetch runs on a TRANSIENT task
// created for it and deleted at the end, and boot is not perturbed at all. **If that creation ever fails, that is a
// finding and not an error to retry**: it means 8 KB of internal RAM was not available at the
// moment a real implementation would have needed it, and the module says so.
//
// PRIVACY (owner's instruction, 2026-09-12): a share key is a bearer capability to a whole
// album and a photo URL fetches a photograph to anyone holding it. Neither is ever printed.

#include <stdbool.h>

#include "sdkconfig.h"

// EVERY CAPTURE CARRIES ITS OWN mbedTLS CONFIGURATION, and this is where that line is defined so
// that both probes print the same one. Ticket 60's knob arm CHANGES these settings, and no figure
// taken before such a change may be quoted after it -- so a log that does not say which
// configuration produced it is not attributable at all. env:gphotosprobe prints it beside the TCP
// window pair; app_gphotos.c prints it inside env:frame, which printed no mbedTLS condition
// anywhere before this.
#if defined(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC)
#define GPHOTOS_MBEDTLS_ALLOC "external"
#elif defined(CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC)
#define GPHOTOS_MBEDTLS_ALLOC "internal"
#elif defined(CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC)
#define GPHOTOS_MBEDTLS_ALLOC "default"
#else
#define GPHOTOS_MBEDTLS_ALLOC "custom-or-iram"
#endif

#ifdef CONFIG_MBEDTLS_DYNAMIC_BUFFER
#define GPHOTOS_MBEDTLS_DYNBUF "on"
#else
#define GPHOTOS_MBEDTLS_DYNBUF "off"
#endif

#ifdef CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA
#define GPHOTOS_MBEDTLS_FREECFG "on"
#else
#define GPHOTOS_MBEDTLS_FREECFG "off"
#endif

#define GPHOTOS_MBEDTLS_COND_FMT \
    "# mbedtls: in=%d out=%d alloc=" GPHOTOS_MBEDTLS_ALLOC " dynbuf=" GPHOTOS_MBEDTLS_DYNBUF \
    " free_cfg=" GPHOTOS_MBEDTLS_FREECFG "\n"
#define GPHOTOS_MBEDTLS_COND_ARGS \
    CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN, CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN

// Called from the heartbeat loop, once per iteration. Spawns a fetch when the configured
// interval has elapsed and no fetch is already in flight. Compiles to nothing without the flag.
void app_gphotos_tick(void);

// True while a session is open, for anything that wants to know. The heap attribution does not
// use this -- it goes through app_heapwatch_set_activity(HEAPWATCH_F_HTTPS) at the point the
// state changes, for the same locking reason app_smb_sync and app_display do.
bool app_gphotos_busy(void);
