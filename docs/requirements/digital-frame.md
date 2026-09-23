# Digital frame — requirements

What the M5Paper Color has to do to be a general-purpose digital photo frame.

**Status of this document.** These requirements are *read off the shipping
firmware's source*, not measured and not invented. M5Stack's user demo is checked
out at `refs/M5PaperColor-UserDemo/` (MIT, `pmokbel` fork), and every requirement
below cites the file and line it came from. That makes it a different class of
document from `docs/research/reference-source-review.md`, whose figures are all
predictions labelled `H1`–`H6`: nothing here is a prediction about timing or image
quality, it is a description of behaviour that already exists in code. What it is
*not* is a description of behaviour anyone has observed on this unit — where the
code and M5Stack's own [usage
page](https://docs.m5stack.com/en/guide/display_device/papercolor/usage) disagree,
that is flagged and the code is taken as authoritative.

Throughout, `UD/` abbreviates `refs/M5PaperColor-UserDemo/main/`.

## Scope

**In scope.** Local operation in full: the setup access point, the web UI and its
HTTP API, image upload and storage on microSD or internal flash, the slideshow,
the buttons, orientation, persisted settings, the low-power RTC-wake cycle, and
the status peripherals (RGB LED, boot and button sounds, USB mass storage, mDNS,
the setup QR code).

**Out of scope — Dropped.** The Ezdata cloud mode (`mode_2` in the shipping
firmware) — remote image push through M5Stack's cloud service,
`UD/apps/ezdata_photo_push/` and `UD/hal/ezdata/`.

It was first dropped at the API surface alone, so that the ported 3,662-line
`index.html` would not have to be edited: `GET /api/modes` advertised `mode_1`
only and `POST /api/mode/switch` refused anything else. That left two live wires
— a hardcoded landing card that was still clickable and routed to a panel
polling a stub forever, and a runtime `<script>` injection from
`cdn.jsdelivr.net`, the only outbound dependency in an otherwise offline UI. So
**ticket `22` (2026-09-03) cut it out of the page as well**, along with the
`/api/mode/mode_2/config` routes and the `mode_2` identifier itself. There is no
cloud mode and no external URL anywhere in the repository.

`current_mode` and its `cur_mode` NVS key remain, for storage compatibility with
the stock firmware: a device left in Ezdata mode by it normalises the value to
`""` on load and rewrites the key, rather than failing to parse its own NVS.

**Added, and not from the shipping firmware.** **FR-10, Google Photos as a second
photograph source** — the owner's requirement of 2026-09-12, for a user who cannot
operate a PC. Every other requirement here is cited to `refs/M5PaperColor-UserDemo/`;
that one is not, and it says so at its head. It does not remove or degrade the SMB
source, and part of what the owner asks for — the whole Google Photos library —
**is not achievable by any route** and is recorded there as such.

**Deferred.** Refresh-time optimisation. The panel's `PLL` register (`0x30`, the
FRS field) stays at the shipping `0x08` until Phase 0 has measured what changing
it costs. See "Relationship to Phase 0" at the end.

---

## FR-1 Power and boot

| ID | Requirement | Source |
| --- | --- | --- |
| FR-1.1 | On the very first boot, show a guide image, set the RTC to 2026-01-01 00:00:00, set NVS key `papercolor/first_boot_done`, and power the device off. The user then presses the power button to start normally. | `UD/apps/app_manager/app_manager.cpp:694-727` |
| FR-1.2 | Play a boot sound at startup. Suppressed when the `boot_sound` setting is off, and always suppressed on an RTC-wake boot. | `UD/main.cpp:28-33` |
| FR-1.3 | Raise the EPD rail through the PM1 (GPIO0) **before** initialising the panel. The panel is electrically dead until this happens. | `docs/board-pinmap.md`, `src/board/m5papercolor/board_pm1.h:33` |
| FR-1.4 | Power off through the PM1's shutdown command, not by halting the CPU. | `UD/apps/app_manager/app_manager.cpp:537` |
| FR-1.5 | Distinguish a cold boot from an RTC-wake boot at startup, before any application state is built. | `UD/hal/hal.h:118-120` |

## FR-2 Networking

| ID | Requirement | Source |
| --- | --- | --- |
| FR-2.1 | Start in AP+STA mode at boot. The access point is named `PaperColor-XXXXXX` from the low three bytes of the MAC, is open (no password), and uses channel 6. | `UD/apps/app_manager/app_manager.cpp:73-102` |
| FR-2.2 | Serve the web UI at `192.168.4.1`, with a DNS redirector and a DHCP captive-portal URL so that joining the AP opens the UI automatically. | `UD/apps/app_server/app_server.cpp:1418`, `UD/hal/utils/dns_server/dns_server.h` |
| FR-2.3 | Advertise mDNS as `<device_name>.local` at boot on every active interface, not only once a station joins the AP. | `UD/apps/app_server/app_server.cpp:1440-` |
| FR-2.4 | Support joining an existing network as a station: scan, save credentials, disconnect, and retry a failed connection every 10 s. The web UI must be reachable over the LAN as well as over the AP. | `UD/apps/app_manager/app_manager.cpp:56, 651-673` |
| FR-2.5 | Optionally shut the AP down after 10 minutes with no clients while the station link is up. **Disabled by default** (`WIFI_AP_AUTO_OFF_ENABLE = 0`); keep it a compile-time option and keep the default. | `UD/apps/app_manager/app_manager.h:11-21` |

Station mode carries no cloud dependency in our scope — it exists so the frame is
usable from a phone already on the house network, which is also what makes FR-2.3
worth having.

> **Deliberate deviation, 2026-09-05: the API is authenticated, and FR-2.1's open
> access point is on its way out.** Ticket `10` said an open AP with an unauthenticated
> upload endpoint was "the shipping firmware's security posture, inherited knowingly"
> and that it would be worth a note here if it ever became a concern. The owner
> raised it, so here is the note.
>
> What changed, and why it is a deviation rather than a bug fix: the shipping firmware
> really does behave this way, and this project's rule is that the firmware is the
> requirement. It is being departed from because the requirement was written for a
> device on one surface and this one is on two — FR-2.1's open AP *and* FR-2.4's LAN,
> at the same time — and because FR-5 puts private photographs behind it.
>
> - **Every route except `GET /` requires a token** (ticket `29`, implemented and
>   LAN-verified). `/` stays open because FR-2.2's captive portal works by answering
>   requests addressed to somebody else, and because an unpaired browser needs
>   somewhere to be told how to pair.
> - **FR-2.1's `WIFI_AUTH_OPEN` becomes WPA2-PSK** once the pairing QR is known to
>   scan (ticket `30`, not built). The AP name and channel are unchanged.
> - **FR-6.3's 5 s hold is what delivers both**, which is the shipping firmware's own
>   gesture doing the shipping firmware's own job. The behaviour a user performs is
>   unchanged; what appears on the panel carries more.
>
> Unchanged: FR-2.2, FR-2.3, FR-2.4 and FR-2.5, including FR-2.5's default of off.
> The trust boundary is physical access to the frame, and nothing more is claimed —
> see ticket `29` for why the larger designs were rejected.

> **Deliberate deviation, 2026-09-19: FR-2.1's access point is not raised at boot, and FR-2.5's
> auto-off is on and inverted.** The owner's reasoning: the AP is for first setup and for pushing
> photographs onto a frame with no home Wi-Fi, both of which happen with somebody standing at the
> frame. Ticket `67`.
>
> - **The radio starts in station mode.** The AP netif, its `192.168.4.1` and its DHCP server still
>   exist; there is no beacon. Confirmed off the air by a second board's scan.
> - **The connect card raises it, and nothing else does** — the 5 s hold of FR-6.3, or a first boot on
>   a device that has never paired. It closes itself after 30 minutes with no client, held open while
>   a client is associated, capped at four windows.
> - **FR-2.5's condition "while the station link is up" is dropped**, because a frame with no home
>   Wi-Fi is the case the AP exists for and it has to close there too. Its `WIFI_AP_AUTO_OFF_ENABLE`
>   block is deleted rather than kept beside this: it changed the radio's mode without going through
>   the one owner of that state.
> - **Why, beyond power:** ticket `66` put the WPA2 passphrase at 8 characters, 40 bits, on the
>   owner's instruction. A handshake can only be captured while there is a beacon, so a radio that
>   is up when asked rather than permanently is the other half of that trade. **It also returns ~5.8 KB
>   of internal RAM**, measured on both boards, which is this project's scarce resource. The power
>   saving is expected and **not measured** — this bench has no current meter.
> - **The recovery ladder is one step longer and unchanged in kind**: the AP is still how you reach a
>   frame whose credentials never associate, and it is still gated on physical access — a button now,
>   rather than a radio that was already on. `-DFRAME_AP_ALWAYS_ON` restores FR-2.1's permanent AP.

> **Deliberate deviation, 2026-09-19: FR-6.3's screen carries words and a typed code, because a
> computer cannot read a QR.** The owner's report of 2026-09-18: a PC could not connect at all.
> Ticket `66`.
>
> - **FR-6.3's gesture is unchanged** — a 5 s hold on TOP — and so is what the shipping firmware does
>   with it. What appears on the panel carries more: the SSID, the access point's password, the mDNS
>   name, an address and an 8-character pairing code, in words, beside the two QR codes.
> - **A second route requires no token: `POST /api/auth/pair`** (FR-3). It hands out the *same* token
>   FR-2.1's deviation note above describes, to whoever can read the panel, so the trust boundary is
>   still physical access and nothing more is claimed. See ticket `66` for why this is not the PIN
>   ticket `29` rejected.
> - **The screen is also drawn once at boot until the device has paired with something.** A user whose
>   only computer is a PC has no way to guess that an unlabelled side button held for five seconds is
>   what produces a password, and the panel is the only surface this device has before anyone is paired.
>   Cleared on the existing rule — whatever renders next, or three minutes.
> - **FR-2.1's WPA2 password is 8 characters**, the owner's instruction of 2026-09-19 — it is typed
>   by hand and 8 is WPA2's own floor. Crockford base32 rather than hex, so those eight characters
>   carry 40 bits instead of 32 and contain no glyph pair a reader confuses. **40 bits is not large**;
>   it rests on the same trust boundary as everything else here, physical access to the frame. A
>   device provisioned before that date is re-keyed once and a phone that had joined must join again.
> - Unchanged: FR-2.1's SSID and channel, FR-2.2's captive portal and its token-bearing redirect,
>   FR-2.3, FR-2.4, and FR-6.1/6.2/6.4.

> **Deliberate deviation, 2026-09-17: the product name is generic, so FR-2.1's SSID prefix and
> FR-8's `device_name` default are not the shipping firmware's.** The owner's instruction: this
> project is no longer the M5Paper Color's, and a name that is one board's model number is wrong on
> the other. The new name is **PaperFrame**.
>
> - **FR-2.1's AP becomes `PaperFrame-XXXXXX`.** The MAC suffix, the channel and (per the note
>   above) WPA2 are unchanged, so a unit is still identified by its MAC. **A phone that had joined
>   the old SSID does not know the new one** and has to pair again through the panel's QR.
> - **FR-8's `device_name` default becomes `paperframe-<six hex>`**, the same three MAC bytes the
>   SSID carries, lowercased because the setter normalises to [a-z0-9-] — so `paperframe-a1b2c3`
>   answers at `paperframe-a1b2c3.local` (FR-2.3). A bare product name would make every frame
>   answer to one `.local`, which is what the two on this bench did as `papercolor`: one shadowed
>   the other with no symptom. `board_wifi_unit_id()` is the one derivation both names use. A device
>   that has been configured keeps its stored name; only a factory-fresh or NVS-erased one takes the
>   new default.
> - **The NVS namespace stays `papercolor`** — FR-1.1's `papercolor/first_boot_done` is unchanged.
>   Renaming it would make every configured device lose its credentials at once, and nothing outside
>   the flash chip can see it.
> - **The Web UI's title, header logo and placeholders follow** (`assets/index.html`). The visible
>   `Copyright ©2026 M5Stack` footer became `UI based on M5Stack's PaperColor firmware (MIT)`; the
>   file's SPDX notice is a licence obligation and stays regardless.
>
> Unchanged: everything the name does not appear in. The one thing to watch is that documents,
> tickets and plans written before this date name the old SSID and hostname, and they are accounts of
> what was true then rather than errors.

> **Deliberate deviation, 2026-09-18: the UI has no mode selection, and FR-9's mode landing page is
> gone from the page while its API stays.** The owner's instruction, and
> The tabbed-page plan is the account. The shipping firmware opens on a MODE page and
> offers two modes; ticket `22` deleted the second one, so since then that page has been a one-item
> menu standing between every page load and the photographs.
>
> - **The page opens on the photographs.** Three tabs replace the logo, the status bar and the fixed
>   bottom bar: **Album**, **Upload** (the two halves of what was one page) and **Settings**.
> - **`POST /api/mode/switch`, `GET /api/device/ready` and `can_enter_mode` are unchanged**, and
>   `init()` satisfies them itself. This is the deviation: the *requirement* is a screen where the
>   user picks a mode, and the choice is now made on their behalf because there is only one. Restoring
>   a second mode means restoring a chooser, and the API it would need is still there.
> - **FR-2.4's LAN reachability and FR-2.2's captive portal are untouched** — the portal still serves
>   `/`, which is now the album.
> - **Settings gains a System panel** (addresses, the ambient sensor, storage, the clock, firmware,
>   and a diagnostics row), which is where FR-2's SSID and IP are now readable: the tab bar has room
>   for a connection dot and the battery and nothing else. `GET /api/system/info` is its one route.
>   The factory reset lives at the bottom of that same panel, still behind its confirmation and still
>   answering `501` per FR-8.2's note.

## FR-3 Web UI and HTTP API

The UI is a single embedded page. Port `UD/apps/app_server/index.html` and keep
the API contract byte-compatible, so the page needs no rework.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/` | The UI itself |
| GET | `/api/modes` | Available modes — **`mode_1` only** in our scope |
| POST | `/api/mode/switch` | Select a mode |
| GET/POST | `/api/mode/mode_1/config` | Orientation, auto-slideshow, interval, low-power |
| GET | `/api/wifi/scan` | Nearby networks (SSID, RSSI, secured) |
| POST | `/api/wifi/config` | Save station credentials |
| GET | `/api/wifi/status` | Link state, IP, SSID, last error |
| POST | `/api/wifi/disconnect` | Drop the station link, keep the AP |
| GET | `/api/device/ready` | Readiness probe for the UI |
| GET | `/api/photos/list` | Stored images |
| POST | `/api/photos/upload` | Upload; "upload and display" or "upload only" |
| POST | `/api/photos/display` | Display a stored image now |
| DELETE | `/api/photos/delete` | Remove an image |
| GET | `/data/*` | Serve a stored file (thumbnails, the UI's previews) |
| GET | `/api/storage` | Which medium is active, and free space |
| GET | `/api/battery` | Battery level |
| POST | `/api/system/reset` | Factory reset |

Route table: `UD/apps/app_server/app_server.cpp:1361-1383`. State and config
payload shapes: `UD/apps/app_server/app_server.h:17-44`.

**Two deliberate departures from that table, both 2026-09-07 and both in
`docs/measurements.md`.**

`GET /thumb/*` is an **addition**: it serves a re-encoded 192×336 JPEG of a stored image. The
shipping firmware has no such route because its grid draws thumbnails from `/data/*`, which
means downloading the original — a median 162 KB here to fill a small square, 1.9 MB for a
twelve-photograph page. `/data/*` keeps its meaning and the page still uses it for View.

`GET /api/photos/list` no longer returns `size` **unless the request carries
`with_size=1`**. Filling it costs one `stat()` per listed photograph, 22-27 ms each on this
FATFS, and nothing in the ported `index.html` reads the field — `photoCache` keeps
`{name, url}` and its only `.size` references are `blob.size` on the upload path. The
parameter exists rather than the field being deleted so that a client written against the
original shape can still get it. The response also carries `thumb_url` alongside `url`.

### Where colour reduction actually happens

Worth stating plainly, because it decides how much work FR-5 has to do — and because
reading the browser code alone gives the wrong answer.

**All colour reduction happens on the device.** The browser composites the layers,
resizes to the panel's 400 × 600, and uploads an ordinary full-colour PNG:
`uploadOnly()` and `uploadWithDisplay()` both call `exportToCanvas()` and
`toBlob(…, "image/png")` with no quantisation in between
(`UD/apps/app_server/index.html:3264-3277, 3298-3313`).

That `index.html` *does* contain two quantisers, `quantizeToEpdPalette()` (dithered, with
`EPD_QUALITY_DITHER = 200`) and `quantizeToEpdPaletteOriginal()` (nearest). They feed
`exportPreviewInkCanvas()` and the live preview only (`:2438-2452, 3248-3261`) — the
on-screen "what this will look like on ink" rendering, which even remaps white to
`#eef1f4` so the preview reads as a screen image. None of that reaches the device.
**This paragraph describes upstream's page, and this project's has diverged**: both those
names and the white remap are gone from `assets/index.html`, whose preview matches against
the selected palette and, since 2026-09-22, draws in that palette's own colours as
epdoptimize's demo does — see `docs/render-pipeline.md`. Upstream's numbers are what FR-5 was written against, which
is why they stay here.

**But it bakes FR-5.3's matte in, and this project deliberately does not.** Upstream's
`exportToCanvas()` composites onto a panel-sized canvas after a contain fit
(`layer.baseFitScale = Math.min(cw/iw, ch/ih, 1)`), so any source that is not the panel's
aspect ratio is uploaded with white bars already in the file. That is harmless for FR-5.3
itself — the device would have drawn the same matte — but it is not harmless for the
`auto_adjust` flow, which this project added and upstream does not have: `render_image()`
takes the region it classifies and measures from the *file's* dimensions
(`src/app/app_display.c:337-338`), so a pre-matted upload gives it the whole canvas. Measured on
the host, that turns a photograph into `flatIllustration` and switches range compression and
white preservation off; `docs/measurements.md` has the table and the fixture trap.
`assets/index.html`'s `exportUploadCanvas()` therefore uploads the drawn rectangle when the
composition is a single untouched layer, and the full canvas otherwise — the preview still
shows the matte, because the panel will have one.

**Verified against the hardware, not just the source.** The four photos left in the
shipping firmware's `/data` by the factory (recovered from a full flash dump) are
400 × 600 PNGs, colour type 2, carrying **45,237 and 93,532 unique colours**. A
six-colour image cannot have 93,532 colours.

**The mode travels as a filename prefix.** The upload carries an `algorithm` form
field, `nearest` or `dither` (`UD/apps/app_server/index.html:3279, 3314`), and the device renames the
file accordingly: `generate_next_photo_name()` picks the prefix character `'N'` for
nearest and `'d'` for everything else, producing `imageN###.png` / `imaged###.png`
(`app_server.cpp:276-278`). The recovered files are `imaged001.png`–`imaged004.png`,
which is the dither branch.

At display time the slideshow reads that prefix back: a name starting `imageN`
selects `epd_fastest` (`local_photo_slideshow.cpp:468-470`), whose dither function is
the no-op `_dither_row_none`; anything else renders under `epd_quality`, i.e.
`_dither_row_rgb_pair` at strength 140
(`refs/M5GFX/src/lgfx/v1/panel/Panel_ED2208.cpp:433-439`).

**Consequences for this project.** There is no path where the device receives an
already-reduced image, so quantisation is not a fallback for microSD and USB files —
it is the main path for every image the frame ever shows. Both modes must exist on
the device and both must be good. This makes the quantiser (ticket `05`) the single
largest determinant of how the product looks.

**This project still uses M5GFX's two algorithms for those two modes**, against
`EPD_RENDER_DEFAULT`. An alternative was ported in full from `paperlesspaper/epdoptimize`
(`src/core/epd_epdopt.c`) and measured at 24.0 s a photograph against 1.62 s, so it is host-only;
only that library's *palette* was adopted. (Both figures are from the 160 MHz build of
2026-09-05; the row path is 1 084 ms on the frame now. The ratio is what decided it.) The
requirement here is the interface — two modes, chosen by the filename prefix, both on the
device — and it is met either way.

**Beyond FR-3.3 and FR-8: which palette the quantiser matches against is a persisted setting
with a Web UI control**, added 2026-09-05. The shipping firmware has no such setting, so this
is an addition rather than a reinterpretation: a tenth NVS key (`palette`), a `palette` field
and a `palettes` list on `GET/POST /api/mode/mode_1/config`, and a `<select>` in
`assets/index.html` built from that list. It exists because no palette suits every picture —
the calibrated sets keep shadow detail and can push dark browns towards green, the
uncalibrated one inverts that trade. `docs/measurements.md` has the numbers behind it.
**The default is `boeber` ("Calibrated - brighter primaries") since 2026-09-06 by owner
instruction**, which is not `EPD_RENDER_DEFAULT`'s `aitjcize` — the constant is the renderer's
fallback and the non-frame envs' choice, the setting is what `env:frame` starts from.

**And a second addition, `auto_adjust`, which is the one that touches FR-3.3 rather than sitting
beside it.** Turned on, the frame classifies each photograph and takes its tone curve, range
compression and *dithering mode* from the class — epdoptimize's own auto flow, ported in
`src/core/epd_classify.c` and `src/core/epd_auto.c`. The last of those three is the reinterpretation: an
`imageN` upload no longer necessarily renders nearest, because the classifier decides. It is an
eleventh NVS key, and it **defaulted to off until 2026-09-06** on the grounds that overriding a
requirement cited to the shipping firmware has to be the user's choice rather than this build's.
**The owner instructed on 2026-09-06 that it default to ON**, so that reasoning no longer holds
and this build ships the reinterpretation: FR-3.3's "Nearest for an `imageN` upload" is met only
when the user turns `auto_adjust` back off. Recorded here as a known, chosen departure.

Its cost was measured on hardware 2026-09-05 and does not threaten anything the requirements ask
for: 1 917 ms a photograph, taking the non-refresh work from 8 % to 21 % of a 15 015 ms refresh.
**And 1 917 is the expensive end rather than the typical one** — measured on the shipping
application 2026-09-06, the two photographs `env:frame` actually rendered cost 726 and 1 184 ms,
because their suggestion carried a neutral saturation and the tone stage took its cheap
three-LUT path. Both figures are real; which one a photograph pays depends on its `lumaStdDev`.
Whether it looks better was settled by eye rather than by instrument: the owner looked at the
result on the panel on 2026-09-06 and chose it as the default.

**And a third addition, `dither_diffuse`, added 2026-09-06 — a twelfth NVS key, off at first and
**default ON from 2026-09-06** by the same owner instruction.** Turned on, the *dithered* half of FR-3.3 is Floyd-Steinberg error diffusion
(`src/core/epd_diffuse.c`, epdoptimize's quantiser) instead of M5GFX's row-wise pair search. Unlike
`auto_adjust` it does **not** reinterpret FR-3.3: nearest stays nearest, from the filename rule or
from the auto plan, because upstream's `quantizationOnly` and FR-3.3's "Nearest" are the same
request. So FR-3.3's two modes remain two modes; only how the dithered one dithers changes.

It is also the one addition whose default is not about cost, because it **costs less** than the path
it replaces: 602.7 ms of diffusion plus a 217.6 ms exact nearest pack against the row path's
1076.3 ms, and 737 ms on the shipping application with `auto_adjust` on. Its default rests on the
owner's own look at the glass on 2026-09-06, and it stays a setting **separate from
`auto_adjust`** so a later comparison can still move one variable at a time.
`docs/measurements.md` has the figures.

One thing about how it composes with the palette setting, because getting it wrong is invisible
until the picture is on the glass: `epd_render_t.tone` *is* display-mode range compression, and so
is `epd_adjust_range()`. `epd_auto_row_tone()` is the rule that exactly one of them runs — the
row path's copy stays on when the plan asks for no range compression (flat artwork, pixel art)
and goes off otherwise. That is a deliberate deviation from upstream, which has no equivalent of
the row path's compression at all, taken because ticket 19 measured the compression rather than
the palette as the win.

| ID | Requirement | Source |
| --- | --- | --- |
| FR-3.1 | Uploads come in two flavours: store and display immediately, or store only. | usage page |
| FR-3.2 | Several images can be loaded at once, and scaled and repositioned freely before upload. This is client-side work inside `index.html`; the device receives the finished bitmap. | usage page, `index.html` |
| FR-3.3 | Offer two colour-reduction modes: **Nearest** (sharper, loses gradation) and **Dither** (better detail, visible noise). The choice is uploaded as an `algorithm` form field and encoded into the stored filename; **the reduction itself happens on the device** — see below. | usage page, `UD/apps/app_server/index.html:3279`, `app_server.cpp:276-278` |
| FR-3.4 | The HTTP server needs ≥13 concurrent sockets, which requires `CONFIG_LWIP_MAX_SOCKETS=16`. Below that `httpd_start()` fails and neither the captive portal nor the LAN UI ever binds. | `UD/../sdkconfig.defaults`, `app_server.cpp:1427` |

## FR-4 Storage

| ID | Requirement | Source |
| --- | --- | --- |
| FR-4.1 | Prefer the microSD card. Fall back to the internal FAT volume if no card is present or if the card fails to initialise. | `UD/apps/local_photo_slideshow/local_photo_slideshow.cpp:39-53` |
| FR-4.2 | Detect insertion and removal while running and switch media live. On a switch, reset the displayed-image index. A card that failed once is not retried until it is physically reseated. **IMPLEMENTED AND VERIFIED LIVE 2026-09-08** (ticket `08`), after the same day measured it doing none of this: `board_storage_ensure()` had always been able to and was called from nowhere. `frame_main.c` now polls the detect line every 10 s, debounced over two agreeing readings, and on a change switches medium, reloads the mirror's manifest and catalogue, and resets the index. Verified in both directions on hardware with no reboot — out: `media=flash` and the factory photographs; in: `card init OK`, 115 photographs, 100 mirrored, 1,032 catalogued. | same, `:55-92` |
| FR-4.3 | Mount at `/data`, on either medium. | `sdkconfig.defaults`, `TINYUSB_MSC_MOUNT_PATH` |
| FR-4.4 | Expose `/data` over USB as a mass-storage device, so a PC can drop files on it directly. Filesystem ownership has to be handed between the USB stack and the application; every application-side access goes through a prepare-and-lock sequence. | `UD/hal/storage/hal_storage.h:59-69` |
| FR-4.5 | The internal volume is a 6 MB FAT partition on wear-levelled flash, at offset `0xA00000`, with the application occupying `0x10000`–`0xA00000` of a 16 MB flash. | `refs/M5PaperColor-UserDemo/partitions.csv` |
| FR-4.6 | Recognise `.jpg`, `.jpeg`, `.png` and `.bmp`, case-insensitively. Scan `/data` only — no recursion. Sort by filename ascending. Cap the list at 500 images. | `UD/apps/local_photo_slideshow/local_photo_slideshow.cpp:525-557`, `local_photo_slideshow.h:137` |
| FR-4.7 | A file the decoder cannot handle must fail cleanly and leave the frame running, not hang or display garbage. **Known limitations, measured:** progressive JPEG and unusual chroma sampling are rejected by TJpgDec; BMP must be uncompressed 24- or 32-bit. See `.scratch/digital-frame/issues/06`. | measured |

The card is on SPI2_HOST alongside the panel (`docs/board-pinmap.md`), so FR-4 and
FR-5 contend for one bus. Card access must be excluded for the duration of a
refresh.

## FR-5 Display and slideshow

| ID | Requirement | Source |
| --- | --- | --- |
| FR-5.1 | The panel is 400 × 600, six colours: black, white, yellow, red, blue, green. Index 4 (orange) exists in the Spectra 6 palette but **is not valid on the 4.0″ part** and must never be emitted. | `src/panel/epd_cmds.h:55-71` |
| FR-5.2 | Hold an orientation setting with two values (0 and 1) and apply it to rendering. Changing it re-renders the current image. | `UD/hal/hal.h:62`, `local_photo_slideshow.cpp:250-277` |
| FR-5.3 | Scale each image to fit inside the screen preserving aspect ratio, and centre it. Clear to white first, so an image that does not fill the screen is matted rather than showing the previous frame. | `local_photo_slideshow.cpp:460-464` |
| FR-5.4 | Advance automatically every `interval_minutes` when `auto_slideshow` is on. An interval of 0 means manual only. Wrap around at the end of the list. | same, `:334-356` |
| FR-5.5 | Coalesce manual navigation: after a button press, wait a 1 s settle window and refresh once, so holding down "next" costs one refresh rather than one per press. If the settled index equals what is already on screen, do not refresh at all. | same, `:311-332, 426-441`, `local_photo_slideshow.h:138` |
| FR-5.6 | Persist the displayed index in the RX8130's battery-backed RAM (two bytes at `0x20`) so it survives a power cut, and restore it at startup. | same, `:25, 156-158, 437-438` |
| FR-5.7 | An empty image directory is a normal state, not an error: keep running and display nothing. | same, `:159-162` |
| FR-5.8 | Re-scan the directory before every navigation, and clamp the index if the list shrank underneath it. | same, `:400-423` |

> **FR-5.2 AND FR-6.2 ARE DEPARTED FROM DELIBERATELY: the orientation setting has FOUR values, not
> two.** Ticket `69`, the owner's request of 2026-09-20 — "画面の回転方向を2方向から4方向へ" — asked
> for all four quarter turns, and the reason they gave when asked is the landscape hand: they want
> either way round, not an upside-down frame. Built the same day.
>
> The requirements above are left as they read, because they are a faithful record of what the shipping
> firmware does and that is what this document is for. What changed here:
>
> - `rotation` is a count of quarter turns from the panel's native orientation, 0..3
>   (`EPD_CANVAS_ROTATION_MAX`). **The stored meaning of 0 and 1 is unchanged**, so a device configured
>   by an older build needs no migration and gets none.
> - **FR-6.2's click cycles 0→1→2→3→0** instead of toggling. Put to the owner with the cost stated —
>   returning to a known orientation is four clicks rather than one, on a frame whose user is an elderly
>   parent — and accepted.
> - The API gains `rotation: 0..3` as the authoritative field. FR-8's `orientation` string stays, still
>   two-valued and still derived from the panel's own dimensions, because a word cannot name a half
>   turn; a body carrying both lets `rotation` win.
>
> This is recorded the way ticket `42`'s departure from FR-7 is recorded below, rather than leaving the
> requirement reading as though the firmware still met it.

## FR-6 Buttons

| ID | Requirement | Source |
| --- | --- | --- |
| FR-6.1 | **DOWN** (GPIO9) advances to the next image; **UP** (GPIO10) goes back one. | `local_photo_slideshow.cpp:392-393`, remapped to position — see below |
| FR-6.2 | A click on **TOP** (GPIO1) toggles the orientation between 0 and 1, saves it, and re-renders the current image. **Departed from since 2026-09-20: it cycles 0→1→2→3→0** — see the note under FR-5. | same, `:391, 250-277` |
| FR-6.3 | Holding **TOP** for 5 s re-enables the access point and displays the Wi-Fi setup QR code. The LED blinks white while the press is being counted and turns green on the 5 s mark. | `app_manager.cpp:581-621` |
| FR-6.4 | Every button press plays a short confirmation tone. | `local_photo_slideshow.cpp:388-390` |

> **Settled on the unit, 2026-09-03: the case has no A/B/C markings at all.** The
> owner read the layout off the device and it is by position:
>
> | Position | GPIO | Action |
> | --- | --- | --- |
> | TOP | 1 | orientation on a click, access point on a 5 s hold |
> | UP | 10 | previous image |
> | DOWN | 9 | next image |
>
> This is not the shipping firmware's assignment. That firmware puts the orientation
> toggle and the AP hold on GPIO10 — the middle button — while its own usage page
> describes that action as being on "the top button". With no letters on the case,
> the letters were never the thing to match: the usage page's *position* is what a
> user reads, and the actions follow it. UP and DOWN then mean what they say.
>
> The firmware remains authoritative for *behaviour* — what a click and a 5 s hold
> do — which is unchanged. Only which pin carries which action moved.

## FR-7 Low-power mode

| ID | Requirement | Source |
| --- | --- | --- |
| FR-7.1 | When `low_power_mode` and `auto_slideshow` are both on and the interval is positive, schedule an RTC wake `interval_minutes` ahead and power off after each refresh. | `app_manager.cpp:163-181, 239-253` |
| FR-7.2 | Power off only after 60 s of idle. Button activity, a client connected to the AP, and a refresh in progress all reset or block the timer. | same, `:200-237` |
| FR-7.3 | On an RTC-wake boot, take a one-shot path: display exactly one image — the one after the index stored in RTC RAM — and power off again without starting the web server or the normal application loop. | same, `:255-306`, `local_photo_slideshow.cpp:223-248` |
| FR-7.4 | If wake scheduling fails, do **not** power off. A device that cannot schedule its own wake and shuts down anyway is a device that never comes back. | `app_manager.cpp:241-244` |

**None of FR-7 is implemented, and it is not going to be on this firmware.** Ticket `15` owns FR-7
proper and is blocked on two things that are not close: a PM1 `SYS_CMD` power-off is forbidden by
`-DBOARD_NO_POWER_OFF` because VBUS does not boot this board, and PM1 GPIO2 (the RTC wake line) is
UserDemo-only and unverified here.

**`low_power_mode` NO LONGER MEANS THIS TABLE, and that is a deliberate departure** — taken
2026-09-10 under ticket `42`, with the owner's approval of the plan that proposed it. It is now
the master switch for an ACTIVE-HOURS SCHEDULE: outside a configured local-hour window the frame
stops advancing the slideshow and stops opening periodic mirror windows, and nothing powers off.
`httpd`, mDNS and the access point stay up throughout, so the failure mode of getting the window
wrong is a stale picture rather than a device nobody can switch back on. `app_schedule.h` carries
the predicate and its fail-open rules; `app_clock.h` carries the clock it needs.

Two reasons the reuse won over a fourth NVS key. The toggle was **plumbed end to end and consumed
nowhere** — NVS, the settings struct and accessors, the API and a live control in the Web UI, with
`app_settings_low_power_mode()` having zero callers outside `app_settings.c` (established
2026-09-08, ticket `42`) — so a user could flip it, watch it persist across a reboot, and have it
change nothing; that is the same shape as the "four guards that were not guarding" in
`docs/defect-log.md`. And the alternative left that switch in the interface still doing
nothing. **The Web UI's description of this control was rewritten at the same time**: it used to
describe FR-7's power-off and wake, which was never this firmware's behaviour.

What FR-7 still has that the schedule does not: the power-off itself, the RTC wake, and the
one-shot wake path of FR-7.3. A future ticket `15` that builds them needs its own key, because this
one is taken.

## FR-8 Settings

Nine values, persisted individually in NVS (`UD/hal/hal.h:19-29, 59-69`):

| Key | Type | Default |
| --- | --- | --- |
| `wifi_ssid` | string(64) | empty |
| `wifi_password` | string(64) | empty |
| `rotation` | uint8, 0 or 1 | 1 |
| `auto_slideshow` | bool | false |
| `interval_minutes` | int | 60 |
| `current_mode` | string(16) | empty (no mode selected) |
| `boot_sound` | bool | true |
| `device_name` | string(64) | `papercolor` |
| `low_power_mode` | bool | false |

Plus five that are **not** FR-8's, added by this build and listed here so the NVS namespace has one
index: `palette`, `auto_adjust`, `dither_diffuse`, `slideshow_random`, `smb_on_demand`,
`smb_resize`, `auto_rotate`, and — tickets `41` and `42` — `tz_offset` (int16 minutes east of UTC,
default 540) with `active_start` and `active_end` (uint8 local hours, default 7 and 23). The
`slideshow_seed` key is state rather than a setting and is deliberately absent from the API.

| ID | Requirement | Source |
| --- | --- | --- |
| FR-8.1 | Guard settings access with a mutex; the web server and the application task both write them. | `UD/hal/hal.h:99-101` |
| FR-8.2 | `POST /api/system/reset` restores every value above to its default, shows the first-boot guide image, and powers off. | `app_manager.cpp:505-541` |
| FR-8.3 | Normalise and validate `device_name` and `current_mode` on the way in rather than trusting the request body. | `UD/hal/hal.h:42-46` |

## FR-9 Status and sensors

| ID | Requirement | Source |
| --- | --- | --- |
| FR-9.1 | Indicate seven states on the RGB LED: operation failed, image read error, refresh started, refresh complete, success, waiting for Wi-Fi, startup succeeded. Events are posted as an event-group bitmask by whatever code notices them, and a dedicated task owns the LED. | `UD/hal/hal.h:71-77, 152-162` |
| FR-9.2 | The LED is two WS2812B on PM1 LDO power — the rail has to be on before anything lights. | `docs/board-pinmap.md` |
| FR-9.3 | Report battery level over `GET /api/battery`. | `app_server.cpp:1372` |
| FR-9.4 | Read the SHT40 for ambient temperature and humidity and keep it available. | `UD/hal/hal.h:88-89, 110` |

## FR-10 Google Photos as a second photograph source

**This is the first requirement in this document that does not come from the shipping firmware.**
Every FR above is cited to `refs/M5PaperColor-UserDemo/`. This one comes from the owner,
2026-09-12, and the citation is the conversation plus the measurements in
`.scratch/digital-frame/gphotos_prereg.md`. It is recorded here rather than in a ticket because it
changes what the product is for, and because ticket `57` — which recommended doing none of it in
this repository — was written without it.

**The requirement, in the owner's terms.** A person who cannot operate a PC
must be able to see **their own** Google Photos photographs on the frame. Adding photographs has to
work from a phone and nothing else. This is stated as mandatory, not as an idea.

**The three constraints that follow, and each one changes an earlier conclusion:**

| Constraint | What it overturns |
| --- | --- |
| **Wi-Fi only at the site** — no always-on PC, NAS or Pi | Ticket `57` §4's shape 3 recommended a companion machine that pulls from Google and drops files on the SMB share. There is nowhere at the site to run it, so **the frame itself must speak HTTPS to the public internet**. `57` §2's TLS row stops being the cheapest way to say no and becomes a requirement to satisfy |
| **SMB stays; both sources may coexist later** | This is a **second source**, not a replacement. The memo's "locate the function fetching from SMB and replace it" is refused by the requirement itself, and ticket `37`'s catalogue seam is where a second source hangs |
| **Tens to hundreds of photographs** | The 2026-09-12 arms used a four-photograph album, which cannot test any claim about scale. At hundreds, whether the initial HTML carries the whole album or one screenful is the difference between a working design and a different, much larger one |

### FR-10.1 The source is a public shared-album link, not an account

The frame holds a `https://photos.app.goo.gl/...` link, entered once through the web UI it already
serves to a phone. **No OAuth, no Google account on the device, no token to expire.** Measured
2026-09-12: the short link answers `302` with a real `Location:` header, and the share page returns
the photographs' base URLs **in HTML that needs no JavaScript**.

A family member creates the shared album once. After that everyone, including the person who cannot
use a PC, adds photographs from the Google Photos app on their phone.

**The album must be LINK-SHARED, and that is not the same as sharing it with people.** Measured
2026-09-12: an album shared to Google accounts carries no `key=` in its URL and returns **`404` to
any request without a signed-in session** — the frame's every request. Only an album for which
someone has explicitly created a link answers an anonymous client. A family that shares an album
with each other and expects the frame to see it gets a frame showing nothing, and FR-10.4 is why
nobody at the site would be able to tell why.

**The privacy trade must be put to the family in words, not discovered.** A link-shared album is a
bearer capability: anyone holding the URL reads every photograph in it, with no account and no
audit trail.

**The photograph host is not fixed.** The same measurement found one album serving tiles from
`photos.fife.usercontent.google.com/pw/` and another from `lh3.googleusercontent.com/pw/`. A parser
anchored on one host finds nothing on the other, which is the hole in the source memo's §4. Avatar
paths (`/a/`, `/ogw/`) are on those hosts too and are not photographs.

### FR-10.2 The whole library is NOT achievable and must not be promised

The owner's ideal is the person's entire library. **No path to that exists.** Google removed
`photoslibrary.readonly` on 2025-04-01 (ticket `57` §1, checked against developers.google.com), so
no third-party application can read a user's library; and a shared link exposes only what is in that
album. This is a property of Google's product, not of this frame, and no amount of work here
changes it.

**The operational substitute** is a shared album that fills itself — Google Photos has offered
both partner sharing and face-based auto-add rules for shared albums. **Unverified here**: neither
was checked against Google's current documentation, and until one is, it is a lead and not a
design. Whoever takes it should check it the way `57` §1 was checked, against the source rather
than from memory.

### FR-10.3 Both sources coexist; neither is privileged

SMB is not removed or degraded. Ticket `37` already separates *what there is to show* from *what is
local*, and the slideshow only reads the local side — so a second source supplies a catalogue and a
fetch-one-by-index. **What does not carry over is identity**: ticket `45` established that the only
thing identifying a catalogue entry across a refresh is its share path, and all four of its options
are keyed on one. A Google source has no share path, so `45` needs a fifth option written for an
opaque per-photograph id before either source is built on it.

### FR-10.4 A failure must be visible without a console or a PC at the site

Nobody at the frame's location can read a log. A scrape that stops working — which its own source
memo expects, since Google promises nothing about the markup — must surface somewhere a remote
family member can see. Ticket `55`'s persistent-fault LED has two conditions and this is not one of
them, and `55` §4's own rule says not to put on the LED what the API can report, so this needs an
API field first.

**What the scrape looks for is data, not code, since 2026-09-13** (the scrape-rules plan):
a signed rule file the frame fetches from a URL or takes through `POST /api/gphotos/config`. A change of
host, anchor, photo URL template or User-Agent, or of the sequence that follows the key, needs no
rebuild and nobody at the site. **A change of structure** — a page that needs JavaScript, a cookie,
another endpoint — **still needs firmware**, and delivering that without USB is ticket `61`.
`GET /api/gphotos/status`'s `rules` is where a rule file's fate is read.

### Non-functional: it is someone else's bandwidth, at the parent's house

Measured 2026-09-12: a CDN-resized 400x600 photograph is a **median 144,993 B**, not the 30-80 KB
its source memo claims. At the shipped `interval_minutes` of 60 that is ~3.5 MB a day of
photographs plus ~1.1 MB for each full index re-scrape — and the scrape must be full, because every
URL sits in the last 7 % of the document, so early abort saves nothing. **~4.6 MB a day per frame**,
on a connection this project does not control and may be metered. On-demand fetching also already
fetches photographs it never shows (ticket `45`: 14 of 34 at one censoring), and that waste moves
from the LAN to Google's CDN.

### The two measurements that gate this, in order

1. ~~Scale, on a 60+ photograph album.~~ **DONE 2026-09-12 and it passes: 167 of 167.** A
   link-shared album of 167 photographs returns all 167 base URLs, with pixel dimensions, to an
   anonymous JavaScript-free GET. **`batchexecute` is not needed at this scale** — the expensive
   branch is off the path. The document costs ~1.1 MB of fixed shell plus **~794 B per
   photograph**, so the refresh scales predictably. **Untested above a few hundred**: re-run the
   arm if an album ever gets much larger.
2. ~~One TLS session's cost in internal RAM.~~ **MEASURED ON HARDWARE 2026-09-12, 20/20 sessions
   (`env:gphotosprobe`): ~45-47 KB of internal RAM per open session, ~12 KB off `dma_largest`,
   fetch task stack 3,716 B used (budget 8 KB).** The predicted
   refusal did not happen. But against the frame's idle `int_free` of ~72 KB that is two thirds of
   the headroom, and its existing troughs already reach **1,995 B** (an upload's thumbnail sidecar)
   and **5,419 B** (an on-demand SMB fetch) — **a 47 KB session concurrent with either does not
   fit.** One issuer (`WE2`, Google Trust Services) covers both hosts, so one chain suffices.
   **The constraint here is memory and nothing else.** The same runs measured a 1.11-1.14 s
   handshake and 268 KB/s, and neither is a requirement: one panel refresh is 15,014.6 ms and the
   interval is 60 minutes, so a second of handshake is 7 % of one refresh, 24 times a day. Those
   figures live in ticket `60` with their conditions and decide nothing. Likewise a browser
   User-Agent is worth ~120 KB of document per index refresh — **0.12 MB a day, an implementation
   note, not a requirement.**

3. ~~The COMBINED case, which is what actually decides this.~~ **RAN ON HARDWARE 2026-09-13 AND
   IT DOES NOT WORK AT THE DEFAULTS.** On an idle frame nine fetches all succeeded but took
   `dma_largest` to 4,864-6,400 B and once **544 B** — below the ~2 KB where lwIP silently drops
   frames. Under 931 KB uploads driven back to back, **ten consecutive fetches failed with
   `ESP_ERR_HTTP_CONNECT` over four minutes while 20/20 uploads returned 200**, and the internal-RAM
   watermark reached **23 bytes**. (A claim first recorded here — that merely linking the TLS client
   cost 24 KB of static internal RAM — was wrong: it is +315 B. Ticket `60` §3.)

   **Nothing broke** — uploads served, renders continued, the API answered afterwards. What failed,
   silently, for four minutes, was the Google fetch: **exactly the case FR-10.4 requires a surface
   for and nothing implements.**

   **This refused the defaults, not the feature — and the knob arm found the setting that works the
   same day: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`.** It moves mbedTLS's heap to PSRAM: one session's
   internal-RAM cost falls from ~45-47 KB to ~10-14 KB, and under the same upload load **32 of 32
   fetches succeeded** with `dma_largest` never under 11,776 B. The two knobs named here first are
   not the answer — a 4 KB input buffer breaks the read, because nothing negotiates a smaller record
   size with Google, and `DYNAMIC_BUFFER` makes `dma_largest` worse. **Adopted into `sdkconfig.frame`
   the same day, and built:** the album's LIST is mirrored and its photographs are fetched one at a
   time before they are shown, like the SMB share's — the owner's ruling of that evening —
   verified on hardware, with `GET /api/gphotos/status` as FR-10.4's surface. The knob also moves the
   Wi-Fi supplicant's crypto allocations; an iPad joined the WPA2 AP and changed the album under it. **FR-10.3
   is met by construction**: the slideshow selects from one catalogue that holds both sources
   (`src/app/app_catalog.c`); both on demand at once has not been run on hardware. **A newly entered
   link that cannot be read, or holds no photograph, is not kept**: the last link that worked is put
   back and the page says why (the owner's rule, 2026-09-13). **Up to four albums are shown together,
read once a day or on the page's Sync now** (the owner's change of the same day). Ticket `60`
§3c-§3f.

Ticket `60` owns both and the build behind them.

## Non-functional requirements

| ID | Requirement |
| --- | --- |
| NFR-1 | No dependency on M5GFX or M5Unified. Rendering goes through this project's own `src/panel/el040ef1/epd_el040ef1.c`. The two libraries stay in `refs/` as references for pin maps and register sequences only. |
| NFR-2 | Builds under PlatformIO with `framework = espidf`: `~/.platformio/penv/Scripts/pio.exe run -e m5papercolor`. Not native ESP-IDF, not Arduino. |
| NFR-3 | Colour quantisation, dithering, fit-and-centre arithmetic and frame packing are ESP-IDF-free C, tested on the host under `env:native`. This is the existing `src/core/epd_format.c` discipline, widened. |
| NFR-4 | Every colour index written to the panel is one of the six valid ones. Index 4 is a defect, not a colour. |
| NFR-5 | A full refresh **measured 15.57 s** on this unit at 26.6 °C (median of 10, IQR 0.007 ms), not the datasheet's 12 s typical — `.scratch/digital-frame/issues/17`. Since 2026-09-03 the driver's default sequencing drops the stock firmware's three 200 ms sleeps and it is **15.01 s** (`epd-refresh-optimization/issues/09`; the frame reports `last_ms=15014`). Either way it blocks the SPI bus and the panel for that whole time. The UI must stay responsive across it, and no image operation may be started while one is running. |
| NFR-6 | Minimum slideshow interval is 1 minute, matching the shipping firmware. Whether a shorter interval is safe for the panel is unmeasured — do not lower it on the strength of the datasheet alone. |
| NFR-7 | Ported code and `index.html` are MIT-licensed by M5Stack. Keep the attribution and the licence header. |

## Open questions

1. ~~**Physical button labels.**~~ **Closed 2026-09-03.** There are no case markings.
   By position: GPIO1 = TOP, GPIO10 = UP, GPIO9 = DOWN. See the note under FR-6.
2. **PM1 GPIO2**, the RTC wake line — still UserDemo-only, and load-bearing for FR-7.
   GPIO1 and GPIO4 are **closed as of 2026-09-04**: drive GPIO4 high, read GPIO1 pulled
   up, low = card present. Observed cardless (GPIO1 high when armed) and then with a 2 GB
   card fitted (low when armed, and only when armed — only bit 1 of `GPIO_IN` moves).
   The firmware still treats detect as advisory and lets the mount attempt decide.
   `.scratch/digital-frame/issues/08`.
3. ~~**PNG decoding.**~~ **Closed.** `src/app/epd_image.c` decodes all three: PNG through
   `espressif/libpng` (the real libpng, not `pngle` — these files come off a network
   and out of a browser), JPEG through `espressif/esp_jpeg`, and BMP hand-written as
   predicted. The frame renders `imaged001.png` and `test-photo.jpg` on hardware.
4. **Minimum safe refresh interval.** Deferred to Phase 0; NFR-6 is the interim
   answer.
5. ~~**Panel temperature reads a plausible-looking wrong number.**~~ **Closed
   2026-09-03.** It was the line, as suspected: the panel's only data pin is `SI0` =
   MOSI, and the read was issued on MISO, which belongs to the microSD and floated to
   `0x00` and then `0xFF` — decoding to the plausible 0 °C and −1 °C. The read now runs
   on a half-duplex 3-wire handle and returns real temperatures (34–38 °C across the
   sweeps of 2026-09-03). Two tails found on 2026-09-04, both in `issues/18`: the read
   left the panel driving SI0, which stopped the microSD answering anything until the
   panel was reset (`epd_read_temperature()` now ends in `epd_reset()`), and the **first**
   read after boot still returns 0 °C while later ones are real — a first-transaction
   artefact, harmless to the harness, but not to anything wanting a temperature before its
   first refresh. `.scratch/digital-frame/issues/18`.

Closed on hardware 2026-09-01 (`.scratch/digital-frame/issues/17`):

- ~~RX8130 7-bit address~~ — it is **`0x32`**.
- ~~Which palette index is which colour~~ — all six render as `src/panel/epd_cmds.h` declares,
  photographed. The handoff document's `4 = blue, 5 = green` is wrong.
- ~~Whether the driver works~~ — it does, end to end.

Closed on hardware 2026-09-02 (`.scratch/digital-frame/issues/03`, `08`):

- ~~Whether the internal FAT volume is usable~~ — it is, and it **already contains the
  stock firmware's photos**. FR-4.5's cross-compatibility is observed, not just intended.
- ~~Whether excluding card access across a refresh works~~ — a guarded read issued 1.5 s
  into a refresh waits 14.1 s, against 15 ms with no refresh running, and returns correct
  content. On flash. **Closed on a fitted card 2026-09-04**: 13.61 s of wait, and the
  *unguarded* read waits exactly as long, which is what proves the driver rather than the
  mutex is doing it.

## Relationship to Phase 0

Phase 0 — the refresh-timing work in `.scratch/epd-refresh-optimization/` and
`docs/phase0-*.md` — is paused, not abandoned, and none of its artefacts change.
Two things keep the two efforts compatible:

- The panel driver keeps taking the FRS value as a parameter, so a faster setting
  found later applies to the frame application with no change above the driver.
- Separating dithering from the SPI transfer (NFR-3) is exactly what Phase 0's
  ticket `07` needs in order to measure the two independently. The shipping
  firmware welds them together inside one transaction
  (`refs/M5GFX/src/lgfx/v1/panel/Panel_ED2208.cpp:425-458`), which is why that
  measurement is hard against M5GFX and easy against this design.
