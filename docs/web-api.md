# The web server, the API and the photo grid

Moved out of the project index verbatim on 2026-09-09 so it is read when it bears on
the task rather than loaded every session. Nothing here was rewritten.

## Networking and the API

**The API is authenticated and the token is a cookie.** Every route except `GET /` and
`POST /api/auth/pair` returns `401` without it.

> **`POST /api/auth/pair` is the second unauthenticated route, since 2026-09-19 (ticket `66`).** It is
> how a browser with **no camera** gets a token at all: a PC could not pair, because the token was only
> ever handed out by the QR on the glass. It takes `{"code":"XXXX-XXXX"}` — eight Crockford base32
> symbols printed on the panel's connect card — and answers `200` with the same `Set-Cookie` the `?t=`
> path sets, plus the token in the body for a client that will not keep cookies.
>
> **It is a delivery channel for the existing token, not a second auth scheme**, which is what answers
> ticket `29`'s rejection of a PIN: no session table, nothing expires for an already-paired client, and
> the rate limiting is a five-attempt counter on a window that only a 5 s button hold opens
> (`FRAME_PAIRING_CODE_MS`, ten minutes). **`HOST_GUARD(req)` and not `GUARD(req)`** — the Host check
> stays, because this reply carries a cookie; a right code under a foreign `Host` is `403` and does not
> pair, measured. **It answers on the station link as well as the AP**, which is the mirror image of the
> SMB routes' `request_from_ap()` refusal and is the whole point: a PC on the house LAN is the case it
> exists for. `403` when no window is open is decided **before** the body is parsed, so a closed window
> says nothing about a guess; a malformed body inside a window is `400` and **costs no attempt**.
> `set_token_cookie()` is shared with `send_index()` so the two cannot drift apart on `SameSite`.
>
> **The unpaired page is `#pair-page` now, not a red banner** (`assets/index.html`). `apiCall()`'s `401`
> branch is still the one choke point — every caller passes through it and almost all of them swallow
> the exception — but it switches the page instead of laying a band over an empty album, and
> `renderRoute()` refuses to navigate away from it. **A cookie rather than a header because the photo grid loads thumbnails as
plain `<img src>` tags**, which cannot carry one — `?t=<token>` on `/` is what turns a
pairing URL into that cookie. > **FIXED 2026-09-11, and the paragraph below is kept because its warning still governs the fix.**
> The owner decided the security question this had been parked on: **the AP is closed to WPA2
> first, and then the portal redirect carries the token** (ticket `30` step 2). `h_static_serve()`
> answers a foreign `Host` with a `302` to `http://192.168.4.1/?t=<token>` when **three** things hold
> — the Host is not ours, `board_wifi_ap_secured()` says the radio is encrypted *now*, and
> `request_certainly_from_ap()` says the request definitely arrived over the AP. That last one is a
> new function rather than the existing `request_from_ap()`, because the existing one fails **open**
> (an unnameable socket counts as the AP, which is the right bias for a refusal and the wrong one for
> a grant). `-DFRAME_AP_OPEN` restores an open AP **and** withholds the token together, because the
> gate reads the radio and not the flag. Verified from here only in the negative: a foreign Host over
> Ethernet still gets `200` and no token, and `403` on the API. **The positive path needs a phone**
> and is ticket `30`'s remaining acceptance.

`Host:`-mismatched requests get `403` on the API and `200` on
`/`, **and that combination is what BREAKS the captive portal rather than what saves it**
(ticket `30`, measured on a phone 2026-09-09). The portal serves the page as
`connectivitycheck.gstatic.com`, so every relative API call the page then makes carries that
Host and is refused 403 — the page arrives and is inert, showing `index.html`'s own
"Cannot reach server" toast. Reproducible from the bench with `curl -H "Host: …"` and no phone.
The `/*` exclusion in `host_ok()`'s comment is necessary and **not sufficient**: it walks the
probe and the wildcard and not the page's own fetch. **The pairing-URL QR works** — it is served
at `192.168.4.1`, which is the whole difference. Do not "fix" this by redirecting the probe to
`?t=<token>` without reading the ticket: that hands the API token to anything joining an open AP,
and it wants deciding alongside step 2's password. The token is minted by `src/app/app_auth.c` **after
`board_wifi_init()`**, because `esp_fill_random()` is only a true RNG once RF is up; on this
bench it comes from `-DFRAME_API_TOKEN` in git-ignored `platformio_local.ini`, the same
fallback-not-override pattern as `FRAME_WIFI_SSID`. The boot log prints four characters and a
length, never the token. That value is wrapped in **both** quote kinds
(`-DFRAME_API_TOKEN='"…"'`), so pulling it out without printing it needs `tr -d "\"'\r"`; strip
one kind and every request comes back `401`, which reads exactly like a wrong token:

```bash
T=$(grep -o 'FRAME_API_TOKEN=.*' platformio_local.ini | head -1 | cut -d= -f2- | tr -d "\"'\r")
curl.exe -s -m 10 "http://192.168.1.20/api/smb/status?t=$T"
```

**The guard is one `GUARD(req)` line per handler and `esp_http_server` has no middleware
hook, so a new route that forgets it is silently open** — sweep all of them, not a sample.

**`GET /api/smb/status`'s `unowned_files` is not a count of orphans**, and reading it as one is
the trap it was built with (2026-09-10, ticket `27`). It counts **photographs on `/data` that the
manifest does not own** — the factory images, every Web-UI upload and every orphan alike, because
a mirrored name and an uploaded one are the same kind of string. It is **a baseline plus the
leak**: the reading is the number *moving* with no upload behind it, and `unowned_exact` false
makes it a lower bound rather than a number. Bench baseline **10**. It updates once per sync
window, not per request, so a fresh boot reports the count from the first window and not before.

**The `?t=` fallback needs a query buffer sized for the query, not for the token.** It was
`APP_AUTH_TOKEN_HEX_SIZE + 8`, which fits `t=<32 hex>` and nothing else, and
`httpd_req_get_url_query_str()` reports a query that does not fit as `RESULT_TRUNC` rather than
`ESP_OK` — so **any second parameter turned a valid token into a 401**, including
`/api/photos/list?t=…&per_page=500`, the paged photo grid a pairing-QR client with no cookie
has to use. Fixed 2026-09-06; the lesson is that a 401 here can mean "your URL was too long".
**And it can also mean "your shell ate the token"** — the console prints `# 401 from <ip> <uri>`
*including the query*, so read that line for an empty `t=` before blaming auth. The quoting trap
that did it twice on 2026-09-09 is in `docs/hardware-runs.md`.

**A handler must not remove the link its own reply travels over, and 500 ms of slack is not
enough — it has to be a floor.** `h_wifi_config()` used to call `board_wifi_connect()`, which
disconnected the station synchronously, and then send its JSON: `curl` got `http=000` and a user
changing networks through the Web UI always saw a failed request (ticket `10`). Since 2026-09-10
`board_wifi_connect()` only queues the credentials and `retry_task()` applies them one to two
ticks later; twelve consecutive POSTs then answered with a full body in a median **0.13 s**, and
the station is back with an IP **1.6 s** later. The first attempt spent the flag on the *next*
tick, which is 0-500 ms depending on where the request landed, and **two of nine POSTs sent their
headers and then lost the body** — so when checking a route that drops its own transport,
`http=200` is not the check, `size_download` is. **A request that lands in an on-demand mirroring
window still gets nothing at all** (`httpd_down` up to ~12.7 s), which is ticket `26` and looks
identical from `curl`; the `smbsync` line on the console is what tells them apart.

**`send_error()` maps codes through a fixed ladder and everything not in it becomes a 500** — 415
and 503 had been going out as 500s at four call sites until a new 413 did the same and gave it away
(ticket `47`). **A new status code has to be added in two places**, the ladder and the call site.

**An error response carries an optional `code`, and a `code` MAY NOT BE REWORDED** — ticket `74`,
2026-09-21. The page shows `message` verbatim, so a UI translated into Japanese still answered a
refused upload in English; `code` is what its dictionary is keyed on (`err.<code>` in
`assets/index.html`, and `errorText()` is the one place per path that reads it). Four things about it:

- **It is opt-in per call site and only 23 of the 76 literal messages have one.** The criterion is a
  message somebody meets in normal use of the page and can act on — the progressive-JPEG refusal, the
  three AP-scope `403`s, the pairing-code refusals. The rest are terse developer strings ("bad json",
  "write fail") and keep plain `send_error()`. **A client that does not recognise a code falls back to
  `message`**, which is what makes this per-route rather than a flag day.
- **Renaming a code silently drops a translation in every language.** `message` stays free to change;
  the code is the contract. Ticket `74` §3c rejected matching on message TEXT for exactly this
  reason, and a renamed code is the same failure wearing a different hat.
- **Three codes carry numbers in a `data` object** rather than baked into the sentence, because a
  translation puts them elsewhere: `upload.over_ceiling` (`bytes`, `limit`) and `pair.wrong`
  (`left`). `send_error_full()` takes it and owns it; `send_error_code()` is the no-data case.
- **The thumbnail handler's 413/415/503 have no code on purpose.** They answer `<img src>` tags, so
  nothing ever reads their JSON. Same for `/api/storage/rescan`'s 503, which the page never calls.

A `curl` that wants to see one: `POST` a progressive JPEG and read `code` beside `message`. That is
also the field to read when checking which firmware is on a board, since `fw_version` lies after an
incremental build.

**Two routes refuse a JPEG the display path cannot decode, and they sniff rather than read the
name** (2026-09-12, ticket `56`'s third item). `UPLOAD_MAX_SIZE` is 12 MB and
`EPD_IMAGE_MAX_FILE_BYTES` is 6 MB, so an over-ceiling photograph used to be stored, listed, given
no sidecar and then to fail every draw for ever while `POST /api/photos/display` answered
`{"status":"ok"}`. `file_is_over_jpeg_ceiling()` now guards both: `h_photos_upload()` after the
multipart parse and **before** the name is generated, and `h_photos_display()` inside the
`stat()`'s own lock section. Three things to keep in mind before touching either:

- **The ceiling is JPEG-only on purpose** and the test is `epd_image_sniff()` on the first eight
  bytes, because that is how the decoder dispatches and because a non-interlaced PNG streams rows
  and has no file limit on the display path. An 8.33 MB PNG uploads with `200` and draws; the
  extension cannot be the test, since a nameless blob's defaults to `jpg`.
- **`req->content_len` cannot be the test either**, so the refusal does not shorten the upload: the
  bytes are on the card before the format is known, and a 6.52 MB refusal still costs its 46 s.
- **The same file is still a broken tile**: `/thumb/` and the upload's sidecar apply the 6 MB
  ceiling to *every* format because that path slurps the file whole, so the 8.33 MB PNG that draws
  has no thumbnail. Anticipated in `epd_image.h:66-68`, observed 2026-09-12, and open as ticket `58`.

**An upload that arrives while a mirroring window has httpd stopped is destroyed after exactly
130,826 bytes** — `curl` exit 56, no status code, nothing in the log addressed to the client, 7 of 10
attempts — while an upload **already in flight** always finishes, because `httpd_stop()` waits for the
running handler and its post-response sidecar. That wait is why `httpd_down` reached 34,372 ms with
uploads interleaved against 20,768 for the same window alone. Ticket `59`, measured 2026-09-12, four
options costed. Nothing leaks: `.upload.tmp` is 404 afterwards.

**Two of those options shipped 2026-09-16 and the firmware half does less than its own comment
predicted.** `h_photos_upload()` stamps `mark_activity()` on entry and `bs_fill()` stamps it for every
chunk that arrives, so the mirror's quiet gate can see a body being received — but **a request the
server has not dispatched yet is invisible to any stamp**, and that interval is where the collision is
made: five windows in one arm were let through with `idle=` 37.8-42.5 s while a client sat connected
and unread. **The handler-visible span of a 931 KB upload is only 7.7-12.7 s** against a client wall of
76-83 s, so the stamps decide anything only for a body the handler sees for over 30 s — a slow client,
or a file near the ceiling on a poor link. Ticket `59` carries the five arms, and `server: upload begin
<n> bytes` plus `smbsync: window due: idle=… run=… manual=…` are the lines that made them readable;
**an upload arm without those two is unreadable, and three arms were read wrongly before they existed.**

**The Web UI half is `uploadWithRecovery()`** (`assets/index.html`, beside `apiCall()`): on a
network-level failure it polls `GET /api/smb/status` for up to 40 s, re-sends once if the frame was
down and came back, and otherwise names the likely cause. `apiCall()` tags a `fetch` rejection with
`networkLevel`, which is how a lost connection is told from a real status code — **a `413` must keep
its own message and must not be retried.** And a window costs a client its wait even when nothing
fails: three `curl` uploads back to back measured 7.92 s, 7.95 s and **27.38 s**, all with the same
7.7 s handler span.

**Probing an API path that does not exist returns the gzipped `index.html`, not a 404**,
because the `/*` wildcard is the last route — one such probe dumped 29.5 KB of binary into a
terminal. Probe with `-o /dev/null -w "%{http_code}"`, and **count the real route table
rather than trusting a comment about it** — the comment above `max_uri_handlers` has gone
stale twice. There are **32** as of 2026-09-20: ticket `60`'s `GET /api/gphotos/status`,
`GET`/`POST /api/gphotos/config` and `POST /api/gphotos/sync` were four of them,
`GET /api/system/info` came next, ticket `66`'s `POST /api/auth/pair` after that, and ticket `68`'s
`POST /api/panel/maintenance` after that. **33 since 2026-09-22**: ticket `09`'s
`POST /api/storage/usb`, registered just before the `/*` wildcard and answering on hardware the same
day (the wildcard registers after it, so a page that loads means every route did).
**That fills `max_uri_handlers` = 33**, so the next route has to raise it again.
`POST /api/storage/usb` answers `202 {"status":"restarting"}` and then restarts the frame as a USB
drive (`src/board/board_usb_msc.h`); `501` on the reTerminal E1002, whose USB-C cannot be a drive,
and `GET /api/system/info`'s `storage.usb_msc` is what the page hides its button off. `/api/system/*`
is now `info` and `reset`. The boot line says the number it registered
(`server: http server on :80, 32 routes`, confirmed on hardware 2026-09-20), which is the count to
trust.

**34 since 2026-09-22, run on both boards the same day**: ticket `61`'s `POST /api/system/ota`, and
`max_uri_handlers` was raised to 34 with it. The body is the signed `firmware.bin` itself, not
multipart (`curl --data-binary @.pio/build/frame/firmware.bin`), streamed 2 KB at a time into the
spare OTA slot. It answers `202 {"status":"restarting"}` and restarts only after the image's signature
and board identity have both passed. `413` means the image is larger than the slot, `503` means the
write could not start (for example, the running image is still on trial), and `400` means a refusal
whose `message` says which check failed. `GET /api/system/info`'s `ota` block reports the running slot,
its `state` (`pending` = on trial, otherwise `valid`, a USB flash included) and `last_result`.
**A refusal before the body is read reaches `curl` as exit 56 with the status and no body**: the
server answers and closes on the unread upload. `last_result` holds the reason.
`src/app/app_ota.h` has the rules.

**35 since 2026-09-23**: ticket `61`'s pull, `POST /api/system/ota/check`, no body. It answers
`202 {"status":"checking"}` at once and the outcome lands in the `ota` block: `checking`,
`last_check_s` (uptime when the last pull ended, `-1` before one), `last_http`, and **`last_code`,
the word the page translates** — `up_to_date`, `installing`, `no_release`, `http_error`, `net_error`,
`other_board`, `rolled_back`, `refused`, `failed` or `none`. Treat those like an error `code`: the
page keys its strings on them, so they may not be reworded. `503` with a `message` is a refusal before
anything started (on trial, a pull or upload already running, a Google Photos run holding the
network). `url_set` false hides the page's button. A push is refused with `503` while a pull runs.
**Whether the frame checks BY ITSELF is the `ota_auto` setting** (NVS, default on), read and written
on `/api/mode/mode_1/config` like `standby_deep`; off stops only the automatic check, and this route
still works.

**And the table being full is a design pressure on new features, not only a number to bump.** Ticket
`68` wanted a start/stop verb *and* a status readout; the status went onto the existing
`GET /api/system/info` as `panel.maintenance` instead of becoming a 33rd route, which is also where the
page's own poll reads it. Prefer that when the new state belongs beside something already served.

**`POST /api/panel/maintenance`** takes `{"action":"start"|"stop"|"white"|"clear"}` and **returns immediately** — the
server serves one request at a time, so a handler that drove ten 15 s refreshes inline would take the
whole UI down for two and a half minutes, which from a browser is indistinguishable from a crash. It
answers `{"status":"queued"}` and the application task's tick does the work. Its "already running" case
is **`503` and deliberately not `409`**, because 409 is not in `send_error()`'s ladder and would go out
as a 500 — the same defect that ladder's comment already records shipping twice. Measured on hardware
2026-09-20: `200`, `503`, `400` for an unknown action and `401` with no token, all four as intended.

**`"white"` parks the panel on white now**, added the same day: one refresh through the same
`app_display_request_blank()` the window edge uses, and it **changes nothing persisted** — not
`standby_white`, not the schedule. It exists because GooDisplay's precaution is to store the panel
showing a fully white image and there was previously no way to ask for one short of running the
ten-flat course or emptying the album. It is refused **503** while a course runs, because the display
has one pending slot and it would discard whichever flat was waiting. Verified on hardware: `200`
`{"status":"queued"}` with the panel going blank (`current=""`, `renders` +1), and `503` when posted
during a course.

**`"clear"` is `white`'s escalation** (2026-09-20): seven refreshes, white/black alternating, three
black passes each bracketed by white, ending white — ~1.8 min here and ~3.6 min on the E1002. Same
`503` mutual exclusion as the course, and it needs no leading white because its own first step is one.
**It is a mechanism and not a measured remedy**: one white render is measured to remove a plainly
legible ghost completely, this is for the case beyond that, and that case has never been produced on
this bench because the owner declined to manufacture a ghost to test it.

**`clear` takes an optional `cycles`, 1..5, default 1** — the owner asked for a *continuous* mode and
this is what it became. A duration was rejected because it is not board-independent: 8 h of continuous
clearing is 1,920 refreshes on the M5Paper Color and 932 on the E1002, so the same "two hours" costs
twice as much on one board as the other, and either number is two orders of magnitude past the ~16
automatic advances a day this frame does. **N cycles is `6N + 1` steps and not `7N`**, because the
whites at the seams are shared — 7, 13, 19, 25, 31 — which also keeps every count starting *and* ending
on white. Out of range is a **`400`** and not a clamp, and it is told apart from the busy `503` on
purpose: one is the client's mistake and the other is a state it can wait out, so a retry loop does not
spin on a body that will never be accepted. Verified on hardware 2026-09-20: omitted → 7, `2` → 13,
`5` → 31, `0` and `6` → `400`.

**`standby_deep` on `/api/mode/mode_1/config` makes the window's white park a whole clear cycle**, and
is **off by default because of the refresh count**: nightly it is 6×7 + the course night's 11 = 53/week
≈ 7.6/day against a ~16/day baseline, so **+47 %** where the shipping behaviour costs +15 % — within
sight of the +63 % that got a nightly course rejected in ticket `68` §5. **On the course's own night the
course wins and the deep clear is skipped**, because only one sequence can hold the panel and a clear
started there would make the course refuse, losing the weekly diagnostic silently once a week. Both
arms measured on hardware the day it shipped.

**Two things about the response's `steps` field, both found on hardware the day it shipped.** It is the
**queued** sequence's length for `start` and `clear`, not the status's — the start verbs hand a flag to
the application task and return, so for up to 200 ms `app_maint_get_status()` still describes whatever
ran last, and the first version answered `"steps": 10` to a seven-step clear. And for `"white"` the
field is **meaningless** and reports the last sequence's count, because one refresh is not a sequence;
`GET /api/system/info`'s `panel.maintenance.steps` plus the new `clearing` flag are what a client
should draw a progress readout from.

**`/api/gphotos/config` is not shaped like `/api/smb/config`, in two deliberate ways.** GET never
returns an album link — only `albums[i].has_album` and `albums[i].album_host`, four entries, one per
slot — because a link is a bearer capability to the whole album. And POST is **not** refused from the
frame's own access point: the site this exists for has Wi-Fi and nothing else, so a phone on the AP
is one of the ways a link gets in. POST takes `{"enabled": bool, "albums": [slot0, …]}`, both
optional; per slot `null` or a missing entry keeps the stored link, `""` removes it, a string sets
it. It accepts only `https://photos.app.goo.gl/…` and `https://photos.google.com/share/…` with
`key=`, so the device never fetches an arbitrary URL; a share URL without `key=` is an account-shared
album, which would 404 on every run, and is refused with the fix in the message. Saving with
`enabled` on asks for a run at once, and so does **`POST /api/gphotos/sync`** (the page's Sync now;
`400` when off or no slot holds a link). Without either, albums are read **once a day** — 24 h after
the last run ends, held outside active hours — and once 90 s after boot.
`GET /api/gphotos/status`'s `albums[i]` (`configured`, `last_result`, `http_status`,
`last_ok_age_s`, `items`, `over_cap`, `album_bytes`) describe each slot's last read; top-level
`album_items` is their sum and `last_result`/`http_status` the first configured slot's that is not
`ok`. `fetched`, `failed`, `deleted` and `evicted` count since boot; `owned` is photographs kept on
`/data` and `want_pending` is a photograph the slideshow has asked for (on demand since 2026-09-13).
`albums[i].reverted` / `reverted_reason` / `reverted_http` say that a NEWLY saved link in that slot
could not be read or held no photograph and its last good link was put back (the owner's rule,
ticket `60` §3e); a save, Sync now, or that slot's next whole read clears them. **A save only queues
the album read for the next 10-second tick**, so a status read straight after a save still describes
the previous run.

**`/api/gphotos/config` also carries the scrape rules** (the scrape-rules plan),
which is how a markup change reaches a frame nobody can flash. POST takes `rules_url` (an `https://`
link under 256 characters; `""` returns to the build's `-DGPHOTOS_RULES_URL`) and `rules` (a signed
rule file's text, at most 4096 bytes); GET returns only `rules_url_host` and `rules_url_is_default`,
because a secret Gist's raw URL is a capability like an album link. **A `200` from that POST says
nothing about the rule file**: it is only queued, and its signature and JSON are checked on the
gphotos task at the next album run — which only happens with Google Photos on. Read
`GET /api/gphotos/status`'s `rules` instead: `seq` and `source` for the set in force (`builtin` is
seq 0), `trial_seq` for one on trial until a run in which some album arrives whole, `rejected_seq`,
and `last_check` (`not newer`, `on trial`, `adopted`, `rejected`, `refused`, `fetch failed`) with
`last_reason` and `last_http`. Files are made with `tools/gphotos_rules_sign.py`. The body limit rose
to fit a rule file, and **`read_json_body()`'s cJSON copies that string into internal RAM for the
length of the request** — `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` puts anything that small there.
Not measured.

**The product name in the page and on the air is `PaperFrame` since 2026-09-17, and the NVS
namespace is still `papercolor`.** Renamed: the `<title>`, the header logo (`PAPER` + five coloured
letters, now `FRAME`), both `device_name` placeholders and every JS fallback in
`assets/index.html`; `DEFAULT_DEVICE_NAME`, and so the mDNS hostname, which now carries the unit's own six MAC
characters (`paperframe-a1b2c3`) from the same `board_wifi_unit_id()` the SSID uses; the softAP's prefix in
`board_wifi_ap_ssid()`; and the mDNS instance name. **Not** renamed: `NVS_NAMESPACE` in
`app_settings.c` and `app_auth.c`, because that would lose every configured device's Wi-Fi
credentials, API token and share password at once. The visible `Copyright ©2026 M5Stack` footer is
now `UI based on M5Stack's PaperColor firmware (MIT)`; the file's SPDX header is a licence
obligation and stays. The requirements doc's FR-2 deviation note of that date is the authority, and
**a stored `device_name` survives the rename**, so a device configured before it keeps answering at
its old `.local` name.

**`GET /api/mode/mode_1/config` is where the page learns the panel's size, since 2026-09-17.**
`add_mode_config()` serves `panel_width`, `panel_height` — the **physical** size in the panel's
native orientation, 400x600 here and 800x480 on the E1002 — and `panel_native`, the word for that
orientation. Before this the page carried `400x600` / `600x400` as constants in five places, which is
one board's answer; `getCanvasSize()` in `assets/index.html` is now the single place that turns these
into the logical size, and the preview's device mock-up is derived from them too. `panel_native` is
derivable from the two numbers and is served anyway, because this mapping has been got the wrong way
round once already.

**And `orientation` is derived from the panel at both ends, not from a constant.** It is the wire
name for `rotation`, whose 0 means *the panel's native orientation* (`epd_canvas.h:31`) — so
`rotation == 1 ? "landscape" : "portrait"` was right on the EL040EF1 and inverted on the ED2208-GCA.
`orientation_name()` and `orientation_rotation()` in `app_server.c` are the read and the write of one
derivation, deliberately paired: a GET and a POST that disagree about this is a setting that flips
itself. **The stored NVS value's meaning is unchanged** — "native" or "turned", on either panel — so
there is no migration, the same argument as the 2026-09-06 correction. Ticket `62` item 4.

**Since 2026-09-20 the authoritative field is `rotation: 0..3` and `orientation` is the derived one**
(ticket `69`). Four quarter turns need four names and the word has two, so the number carries the
setting and the word is output — which is what those two failures argue for. Three things to know:

- **`rotation` wins when a body carries both and they disagree**, and that is written down in
  `h_mode_cfg_set()` rather than left to fall out of cJSON's iteration order, which would make the
  answer depend on how a client serialised its object.
- **`orientation` is still answered for every rotation, and still only names two of the four.** It is
  the odd bit now (`rotation & 1`), not `== 1`, which used to call rotation 3 by the native
  orientation's name. A `POST` of the word alone reaches only the two un-flipped directions, which is
  correct: a word cannot name a half turn.
- **`rotation_max` is served beside it** on `/api/mode/mode_1/config`, and `panel.rotation` on
  `/api/system/info`. The page uses the number for the four-item control and for the preview's
  logical size, and shows the angle beside the word in System info — a frame hung the other way round
  reads identically otherwise.

Measured on hardware 2026-09-20: `rotation: 4` is a `400` "rotation out of range", and
`{"rotation":2,"orientation":"landscape"}` answers `rotation 2, orientation portrait` — the number won.

## The page is three tabs, and `GET /api/system/info` exists because of it

**2026-09-18, the tabbed-page plan.** The page opens on **Album**; **Upload** is the
preview-and-upload half of what was one two-column page; **Settings** keeps its own index and its
category routes. The route is still the hash and it is now one mechanism for both levels —
`#album`, `#upload`, `#settings`, `#settings/<category>` — so a reload and a phone's back gesture
behave as they did.

**Three things a reader of the old page will look for and not find.** The MODE page is gone: there
has been one mode since ticket `22`, so `init()` does its `POST /api/mode/switch` itself. That POST
is **once per boot, not once per page load** — `can_enter_mode` reads `s_requested_mode`, a RAM-only
static (`app_server.c:79`), so the first client of each boot pays it and the rest see it already
true. The status bar is gone into the tab bar, which carries only the connection dot and the
battery; **the SSID and the IP moved to Settings → System.** And the fixed bottom bar is gone, so
`goToLanding()`, `showModeSwitch()`, `switchMode()` and the mode modal went with it.

**`#wifi-page` and the functions that served it were deleted on 2026-09-22**, with the CSS only
they used (the mode selector, the bottom-sheet modal, the connection-status line). They had been
unreachable since before the tabs — the modal that was their only entry had no caller at all — and
Wi-Fi has been configured through Settings → Wi-Fi since ticket `10`. `loadSetupConfig()` survives
as the one read that seeds `globalConfig.device_name`/`boot_sound`, which `currentDeviceName()`
falls back to. The album's Prev and Next wrap round at either end since the same day, and
`loadStorage()`'s status line is the page's one storage warning: besides "running low" and "nearly
full" it now warns for an SD card whose `GET /api/storage` `fat` is `fat16`/`fat12` (the 512-entry
root fills at ~100 photographs) and for `card_unreadable` (a card with no filesystem this build reads,
so the frame is on internal flash). Both are 2026-09-23; `board_storage.c` carries why.

**`GET /api/system/info` is one read, of things that already existed, and it does two things
deliberately.** It reports **no photograph count** (that needs `scan_photos()` under `s_scan_mutex`,
a directory read on a one-worker server, and the Album tab already has the number) and **no mirror
state** (`/api/smb/status` and `/api/gphotos/status` own that; the panel reads all three when it is
opened). Everything is raw — bytes, milliseconds, dBm, a UNIX epoch, a timezone offset in minutes —
and formatted in the page. The panel does **not** poll: the server takes one request at a time and
this handler holds it for the sensor's conversion.

  **"One request at a time" is verified against the library, not assumed** (2026-09-21, after ticket
  `76` found a claim about a *different* library that this repository had been repeating and that was
  false). `esp_http_server` creates exactly **one** task — `httpd_thread`
  (`esp_http_server/src/httpd_main.c:331`, started once at :533) — which loops on `httpd_server()`,
  whose `select()` (:294) hands ready sockets to the handler **inline, in that same task**. So two
  handlers cannot overlap, and every enumeration in this project that says "the two HTTP sites cannot
  collide with each other" rests on that and is sound. What the one task does *not* protect is a
  handler racing another task — which is what tickets `73` and `76` are.

  **And it is a STORAGE route, which grepping the handler denies.** Its body takes no lock; the one
  innocuous line asking for free bytes does — `board_storage_usage()` (`app_server.c:2536`) takes
  `board_storage_lock()`. That matters because a render blocks storage routes, and **`/api/battery`
  is the contrast: it reads the PM1 over I²C, touches no filesystem, and is held up by nothing but the
  single-worker queue.**

  **How long `/api/system/info` waits behind a render depends on the MEDIUM, and the header everyone
  cites for this is unconditional and therefore incomplete.** `board_spi.h:48-49` says
  "`board_storage_lock()` takes this one internally, so a filesystem access holds both and a refresh
  holds only this" — but `board_storage.c:93-96` is
  `s_lock_took_bus = (s_ctx.media == BOARD_STORAGE_MEDIA_SD)`, and the comment there gives the reason:
  the internal FAT volume is on the flash controller, never touches SPI2, and taking the panel bus for
  it would make the UI wait out a refresh for nothing, which is NFR-5 failing. So a render has **two
  phases that are not the same for a caller**:

  | Phase | Holds | A storage route waits |
  | --- | --- | --- |
  | **Decode** (open + draw, 15.6 s measured, `app_display.c:380-396`) | storage mutex; **plus** the SPI bus on SD | **on both media** |
  | **Refresh** (the DRF, 15.0 s on the M5 / ~31 s on the E1002) | the SPI bus alone | **on SD only** |

  **BOTH halves are measured, and the flash one is the older of the two.** Ticket `03`'s 2026-09-03
  section is both the change and its verification: after the three-way conditional went in, **nine
  consecutive requests during a refresh answered in 70-90 ms** (`issues/03-spi-bus-sharing.md:219-222`,
  which also states the split outright — "this ticket's `read_wait_ms=14125` is now the SD behaviour,
  and the flash behaviour is ~15 ms with a refresh in flight"). The SD half is ticket `46`,
  2026-09-09: `/api/photos/list` blocking `14.9-16.8 s` **with a card fitted**, against 0.10-0.26 s
  idle — 60-160x — with ~3 s of the total still unattributed.

  So: **on SD a figure from inside a refresh window is unreachable over HTTP and needs firmware; on
  internal flash it is reachable in tens of milliseconds.** Never state it unqualified — and note that
  which board is on which medium is **a property of today** (`docs/hardware-runs.md`: since
  2026-09-22 each board has a card, and moving one needs hands).

  **If you find yourself deriving this from `board_storage.c` again, stop and open ticket `03`.** The
  comment directly above that line names it. Two sessions on 2026-09-20 re-derived this mechanism four
  times between them — blaming the single worker, then a render holding the storage lock, then an
  unconditional storage-to-bus coupling, then the medium — and every one of those was refutable from a
  file already on disk, including a measurement that made the last fixture unnecessary.

- **`clock.since_sync_s` is `-1`, not `UINT32_MAX`, for "no sync this boot"** — the convention
  `last_run_age_s` already uses on both mirror routes. `synced` true *with* no age is a real state
  and not a contradiction: a software reset keeps the SoC's clock and loses the flag, so `app_clock`
  trusts the clock and leaves the age unset until this boot's first sync lands
  (`app_clock.c:219-237`). Passing the sentinel through as a number made the page draw
  "1193046 h ago", seen on hardware 33 s into a boot.
- **`sensors.temp_c` and `humidity_pct` are absent together when the read failed**, never 0. This
  handler is `board_sht40_read()`'s first caller in the application, and being the first to call it
  *repeatedly* is what exposed a wait that was right less than half the time — see
  `docs/defect-log.md` 2026-09-18.
- **`network.ap.secured` asks the radio, not the build flag**, the same question the portal redirect
  asks before it will part with the token.
- **`network.ap.up` and `window_s` are ticket `67`, and `up` is the one to read first.** The access
  point is **down between windows** since 2026-09-19 — raised only by the connect card, closed after
  30 minutes with no client — so `secured` true with `up` false means "the password is stored and the
  next window will carry it", not "there is something on the air to join". `window_s` is the seconds
  left, 0 when down, and it **stops counting down while a client is associated**, so a steady number
  means somebody is using it. **Everything about the captive portal below only applies inside a
  window**: `h_static_serve()`'s token-bearing `302` needs `request_certainly_from_ap()`, and with no
  AP there are no AP clients, so the redirect cannot fire. The heartbeat carries the same thing as
  `ap=<n>s` or `ap=down`.
- **`diag` is `int_free` / `int_min` / `dma_largest` / `psram_free` with no threshold attached.**
  Internal RAM is the scarce resource here and below ~2 KB of `dma_largest` lwIP drops arriving
  frames with a healthy console; until this route the only way to read that was a serial cable. A
  limit drawn in the firmware would be a second thing that can be wrong.

**`DELETE /api/photos/delete` takes the name in the QUERY STRING, not in a JSON body**, and a
measurement script has been getting that wrong. `h_photos_delete()` calls
`httpd_req_get_url_query_str()` and then `httpd_query_key_value(query, "name", ...)`, answering
`400 name required` to anything else — so the correct form is
`-X DELETE ".../api/photos/delete?t=<tok>&name=<file>"`. Found 2026-09-12 when 20 of 20 deletes
returned `400`; the same twenty succeeded immediately in the query form.

**The reason it matters beyond one script:** `.scratch/digital-frame/overlap_arm.py` sent the JSON
form, and its pre-registration claims "every uploaded photograph is deleted at the end: the card is
left as it was found, which is what makes these numbers comparable with ticket `48`'s". With a
`400` on every delete that claim was false, and each run left its 931 KB fixtures behind. The helper
is fixed; **a cleanup step whose return code nobody reads is not a cleanup step**, which is why
`gphotos_overlap_arm.py` counts its deletions and prints `*** THE CARD IS NOT AS IT WAS FOUND ***`
when any fails.

**`esp_http_server` serves ONE request at a time, and that settles more arguments than it
looks like it should** (measured 2026-09-07). A handler that blocks blocks *everything*:
`GET /api/battery`, which reads the PM1 over I²C and takes no storage lock at all, goes from
0.17 s to **4.5 s** behind a 527 KB transfer throttled to 60 KB/s, and
`uxTaskGetSystemState` shows exactly one `httpd` task. Three consequences:

- **A per-handler mutex against `max_open_sockets` is never needed.** Eight concurrent
  `/thumb/` requests came back in a 1.5 s ladder — 1.67, 3.14 … 12.19 s — so the "8 × the
  handler's peak RAM" arithmetic describes something that cannot happen.
- **A 128 KB response never blocks its sends**, because the whole body reaches the TCP stack
  before `httpd_resp_send_chunk()` has to wait; the same throttled client left
  `/api/battery` at 0.12 s. So response size, not client speed, decides whether a slow reader
  can hold anything.
- **No HTTP-side observable can isolate a lock from httpd's own queueing.** An A/B built to
  show one was measuring the other, and the give-away was only visible because the control
  request touched no storage. `docs/measurements.md` has it.

**`env:smbprobe` writes its station credentials into the Web UI's own NVS keys**, so a later
`env:frame` flash comes up on the LAN without anyone configuring it.

**An access point that leads with WPA3-Personal** is one an ESP32-S3 station will associate to and
then fail in the 4-way handshake with `reason=204`, looking exactly like a wrong password.
`sae_pwe_h2e = WPA3_SAE_PWE_BOTH` plus `pmf_cfg.capable` fixes it and is in
`src/app/board_wifi.c`.
## The photo grid: thumbnails, a cache header, and one `stat()` that was free to delete

Three changes on 2026-09-07, all in `src/app/app_server.c`. The evidence is in
`docs/measurements.md`; read it before touching `send_file()`, `h_photos_list()` or
`h_thumb_serve()`.

- **`GET /thumb/<name>` serves a re-encoded 192×336 JPEG where the grid used to fetch the
  original.** A cold 12-photograph page went **1.9 MB → 148 KB**; the share's files are a
  median 162 KB and uniformly 768×1344. `h_photos_list()` returns `thumb_url` beside `url`,
  and `url` deliberately still points at the full file because View wants it. The encoder is
  `espressif/esp_new_jpeg` — **ESPRESSIF MIT and a prebuilt `.a`**, the only dependency here
  that is either, so no `--wrap` is available against it. The **decoder stays `esp_jpeg`**;
  ticket `31` closed the decoder question correctly and only the encoder is new.
  **The pixels are verified, 2026-09-08** — a JPEG-in and a PNG-in thumbnail were fetched from the
  device beside their originals and both reproduce composition, colour and crop, so
  `esp_new_jpeg`'s silent column-wrap-on-misaligned-input does not occur here. This had been recorded
  as needing the owner's eyes because "there is no JPEG decoder on this host", which is true of
  every scriptable path and irrelevant: **the `Read` tool displays an image file.** Before writing a
  check off as impossible here, ask whether it has to be scriptable.
- **The ceiling is the decoder's own `EPD_IMAGE_MAX_FILE_BYTES` (6 MB), not `send_file()`'s 2 MB**
  (ticket `47`, 2026-09-09) — sharing that constant meant every photograph over 2 MB was a broken
  tile, 404 in 50 ms with no log line. Absent is 404 and silent; over the ceiling is 413 and out of
  PSRAM is 503, each with one `ESP_LOGW`. **It is a JPEG ceiling**, so a 6-12 MB non-interlaced PNG
  stays displayable and untileable. A large tile costs **~15 s** — read plus whole-image decode, the
  reduce and encode being 67 ms — and the watchdog fires in the **`httpd`** task on each one.
  Figures and the three collision arms: `docs/measurements.md` 2026-09-09.
- **It costs 1.2-1.4 s a thumbnail for the SHARE's files and ~810 ms of that is a floor.**
  `epd_image`'s JPEG path
  decodes the **whole** image at open (`epd_image.c:463`), and `epd_image_open_mem_fit()` was
  added so a thumbnail asks for a quarter of the panel's pixels — worth **110 ms, not 4×**,
  because entropy decoding follows the compressed bytes and not the output size. So a cold
  grid page blocks the whole API for ~17 s, given one httpd worker. **Do not reach for a
  smaller output to fix this**; the lever is spent, and the remaining one is a cache.
  **That ~17 s is the DECODE path and it is no longer the usual one** (2026-09-10): `smb_resize`
  ships on, so a mirrored photograph is answered from its `.thm` sidecar in a median **0.172 s**
  against **1.642 s** decoding, and a page of mirrored files is ~2-3 s. **And since 2026-09-10 an
  upload writes its own sidecar too** (ticket `48` option 4), so the only files left paying the
  decode are the four factory PNGs and anything imported before ticket `49`. Measured on a 12 MP
  upload: **10.55 s a tile before, 0.068 s after — 155x**, and the sidecar costs one
  `sidecar <name> WxH … read= resize= encode=` line at the same ~10.6 s, once, at upload time.
  **Two things about that placement rather than the saving**: it runs after `send_json()`, so the
  uploading client is not made to wait, but the **next** request still queues behind it on the
  single worker — the first `/thumb/` after an upload measured 10.7 s *without decoding*, and only
  the absent `thumb` line in the console says which of the two it was. A short `.thm` write is
  unlinked, because nothing else on the device inspects sidecars and a truncated one would be a
  broken tile for that photograph's life.
  `epd_image_open_mem()` is the `_fit` call at `PANEL_W`×`PANEL_H`, so the display path is
  unchanged and still renders at `panel_ms=15014.2`.
- **A thumbnail is not stored on `/data` under a name `photo_list_is_image()` accepts**, or
  the slideshow would show it, and not without something to unlink it, or `smb_on_demand`'s
  FIFO eviction would leave it orphaned outside `.smbidx`. Ticket `49` answers both — a
  `.thm` suffix and `delete_local()` — and so `smb_resize` does write a sidecar there;
  the ruling was against the *naming and the orphan*, not against the location. **Since
  2026-09-23 "there" is `/data/.thumbs/`** (`docs/smb-mirror.md`), and `/thumb/<name>` is
  the only route that reads it. **The PSRAM LRU is
  no longer wanted** (2026-09-10): the population it was for was uploads, and uploads now write a
  sidecar of their own, leaving four factory PNGs and pre-`49` imports — not a card. Ticket `48`
  carries the measurement and closes on it.

  The `/thumb/` timing line gained the file's name that day (`thumb <name> 240x176 …` where it read
  `thumb 240x176 …`), because the same producer now serves two callers and a capture has to say
  which. A `grep` for `thumb ` still matches; one for `thumb [0-9]` does not.
- **Over 2 MB, `send_file()` streams with the storage lock held, so a View stops the panel**
  (ticket `47`, measured 2026-09-09): 21-23 s for a 6.5 MB file, and a refresh due during it waited
  **38.5 s**, cumulatively across back-to-back Views. `app_server.c:240-250` accepts the stall as
  "the rare large upload rather than anything the photo grid asks for" — **View asks for it**,
  because `url` points at the full file on purpose. Bounded-block staging is the fix, argued
  against why the whole-read path exists (a delete cannot truncate a response read under one lock).
- **An `int_min` of 2,899 B was seen once and has NO attributed cause** — the lowest internal
  figure recorded here, against ~72 KB idle, with the largest listing, a 6.5 MB upload and a
  1.47 MB PNG render each tested and excluded. **The heartbeat prints the minimum without the
  moment**, which is why the log cannot close it; a one-line `ESP_LOGW` on `int_free` crossing a
  floor would. Details in `docs/measurements.md`.
- **`send_file()` sets `Cache-Control: private, max-age=86400`, and no validator.** `ETag` and
  `Last-Modified` both need `stat()`, which is the 22-38 ms call below. The staleness this
  accepts is the catalogue's documented same-local-name ambiguity, whose worst case is a wrong
  thumbnail in the management grid rather than a wrong photograph on the panel. A `?v=<size>`
  buster was considered and **declined**: it needs the now-opt-in `size`, and `url[208]` has
  10 bytes spare over `encoded[192]`, so long names would `snprintf`-truncate into 404s — the
  same shape as the `?t=` query-buffer defect.
  **The staleness was wider than the paragraph above says, and it has been seen.** Upload names
  were NOT unique: `generate_next_photo_name()` reused the lowest free `imaged###`, so a photo
  uploaded after a delete took the deleted one's name. On 2026-09-23 Chrome showed the deleted
  photo's tile (8,808 B cached) while the frame served the new one (8,017 B). **Fixed the same day
  at the source**: the number is one past the highest ever given, kept in `/data/.imgseq` (on the
  card, so it travels with it) — measured: upload `imaged026`, delete it, upload again → `imaged027`.
  Only past 999 does it fall back to the lowest free number. A Nearest upload's album page is
  therefore found by looking (`showPhotoPageOf()`), not computed from the number, which only worked
  while numbers were packed. For that fallback and for anything cached before the fix,
  **Settings → System → "Clear thumbnail cache"** puts a fresh `?v=<ms>` on
  every thumbnail URL (`thumbGen` in `localStorage`). That is a cache miss per tile, once. It is
  appended by the browser, so `url[208]` is not involved; the longest `/thumb/` URL is 198 bytes,
  plus 16, against `CONFIG_HTTPD_MAX_URI_LEN=512`, and `request_file_name()` drops the query.
- **`send_file()` DOES NOT IMPLEMENT `Range`, and it does not say so — it answers `200` with the
  whole file.** There is no `206` and no `Accept-Ranges` anywhere in `app_server.c`. So
  `curl -r 0-3 /data/<name>` is not a way to peek at a header: on 2026-09-20 it dumped a 2 MB
  photograph into a terminal. **When you want only the size or the status of a `/data/` or
  `/thumb/` URL, `-o /dev/null -w "%{size_download}"`** — the `-w` values are computed whether or
  not the body is kept, so nothing is lost. Implementing `Range` would want `stat()` and a second
  read path; nothing here has asked for one.
- **`GET /api/photos/list` no longer returns `size` unless you ask for `with_size=1`.** It cost
  one `stat()` per photograph — **22-27 ms each**, the same defect ticket `36` fixed in
  `build_plan()`, scaled to this directory — for a field **`index.html` never reads**.
  `per_page=48` went **1.259 s → 0.107 s**. Opt-in rather than deleted so the ported API shape
  survives; the `with_size=1` arm is also the control that proves the sweep still measures
  something.
- **Three ideas were measured and rejected before these**, so they do not need re-deriving:
  simplifying the page's appearance (all the CSS is gzip 7.7 KB against 8.6 MB of free flash,
  and the page is sent from rodata so it costs no internal RAM), moving assets to the card
  (`env:frame` carries **one** asset, and `GET /` is the only route needing neither auth nor
  storage), and converting icons to SVG (already 11 inline `<svg>`, zero rasters).
  **The size in that middle clause said 37,960 B and is deleted rather than re-fixed**: it was
  already stale against `tools/gen_assets.py`'s own 52,937 B before ticket `74` took the asset to
  **86,831 B** (2026-09-21), so three places carried three numbers for one file. The script prints
  the current pair and the generated `assets_data.c` records it above the array — read that. What the
  rejection actually rests on is the *ratio*: one asset against 8.6 MB of free flash, at no cost in
  internal RAM, which is still true four-fold larger and is why the conclusion did not move.

The share is `\\192.168.1.10\photos` on **this Windows PC**, not a NAS, and
`Get-SmbServerConfiguration` reports `RequireSecuritySignature: True`, so it exercises the
signing path. Credentials live in git-ignored `platformio_local.ini` and in NVS; the pair is
the WSL side's, at `ComittoNxA/.omc/devicetest/smb.env`, which also names a share that
returns ACCESS_DENIED.

## The charge cap: `charge_limit_pct` and `/api/battery`'s `charger` object

Ticket `71`, 2026-09-20. **A battery-longevity setting that exists on both boards and can only be
obeyed by one**, so the two halves of that are answered by two different routes.

**`charge_limit_pct` on `/api/mode/mode_1/config`**, GET and POST, alongside `standby_deep` and
`maint_day`. Three values and nothing between them: **0** = leave the charger alone, **80** = cap,
**100** = restore the part's own default. Anything else is a **400 naming the field** rather than a
clamp — 90 is the value a user will try, and a clamp would answer 200 and do something else.
**POSTing it applies immediately**: the handler calls `app_charge_apply()`, unlike `standby_white`
and `standby_deep`, which only change what the next quiet period does.

**`/api/battery` gains a `charger` object, and it is ABSENT rather than zeroed where there is no
reachable charger** — which is the M5Paper Color, permanently (`docs/board-pinmap.md` §"Charging").
`board.h:64-66`'s rule is that a caller must not read a false field as a measurement, and
`"charging": false` on a board that cannot tell would be exactly that mistake served as JSON.
**The page uses that absence as the feature test**: `updateBattery()` unhides the Settings control
only when the object is present, so the frame that cannot obey the setting does not show it.

| Field | Meaning |
| --- | --- |
| `chrg_stat`, `power_good` | decoded `REG08`, positions per `pins_reterminal_e1002.h` |
| `vreg_mv` | the termination voltage the part currently holds |
| `limit_pct`, `target_mv`, `applied_mv` | the setting, what was asked for, **what read back** — the last is the only one that is evidence |
| `corrections` | times the tick found `REG04` had moved on its own. **An instrument, not a diagnostic**: it is what proved the charger's I²C watchdog reverts the register, and it staying at zero is the only evidence the watchdog is now disabled (ticket `71` §10.3) |
| `writes`, `watchdog_off` | writes issued; whether `REG05`'s watchdog bits read back clear |
| `regs` | **raw `REG00`–`REG0B` as hex**, because no datasheet for this part is on this disk and a later reading must be able to disagree with this build's decode. Same "emit raw numbers, compute the verdict at analysis time" rule the harness follows |
| `regs_age_ms` | how old `regs` is. **`voltage_mv` is live; the block is not** |

**The block is served from `app_charge`'s cache and NOT read in the handler**, deliberately: twelve
more I²C transactions per request, from the task holding the server's only worker, is not worth a
fresher battery icon. **The reason is cost and not safety** — until 2026-09-21 this paragraph cited a
bus "taking turns by luck", and that was wrong: the IDF `i2c_master` driver serialises transactions
on a bus with a mutex of its own (ticket `76`, `docs/board-and-storage.md`).

## Request identity: the path-traversal guard, and the one thing it does NOT do

Ticket `79`, 2026-09-22. `src/core/req_name.[ch]` — `req_url_decode()`, `req_url_encode()`,
`req_name_is_safe()` — plus `test/test_req_name` (21 cases) and `tools/api_path_sweep.py`.

- **Every route that turns request data into a path reaches the guard, and the ORDER is the
  property.** `DELETE /api/photos/delete` decodes then checks; `POST /api/photos/display` takes a
  JSON string and checks it; `GET /data/*` and `GET /thumb/*` go through `request_file_name()`, which
  is `req_url_decode(...) && req_name_is_safe(...)` — decode **then** check. Reversed, the API would
  accept `%2e%2e%2f` and every unit test of the parts would still pass, which is why
  `test_req_name` asserts the pair and not only the pieces. The other `snprintf(…BASE_PATH…)` sites
  take their name from `photo_list` (the device's own scan) or from `generate_next_photo_name()`.
- **`..` is not what stops a traversal — `/` and `\` are.** Found by mutation, not by reading:
  deleting the `..` test from the guard left every traversal case passing, because `../zzz.jpg` still
  contains a slash. What the `..` test stops is a name with no separator at all, i.e. `..` itself —
  naming the parent directory rather than escaping to it. Both conditions stay; the point is that a
  future tidy-up which keeps only the "obvious" one has removed the wrong half.
- **The guard REFUSES; it does not strip, canonicalise or re-decode.** That is why `....//`,
  `.../.../` and `..;/` have nothing to survive, and why double encoding is safe: `%252e` decodes
  once to the literal text `%2e`, which is a filename that does not exist rather than a path. **A
  second decode pass would turn that from safe into exploitable** — do not add one.
- **An encoded NUL truncates the name for the guard and for `fopen()` alike**, so
  `zzz%00%2e%2e%2fzzz.jpg` is the name `zzz`, inside `/data`. Recorded because it looks alarming.
- **A dotfile is reachable and that is the FR-4.6 port, not an oversight**: `GET /data/.smbidx`
  answers 200 with the mirror's ownership record, which names every share path it has fetched.
  `photo_list_is_image()` keeps dotfiles out of the album and the slideshow, and ticket `38`'s
  rejection is the *mirror's* rule. Every route needs the token, so this is inside the API's trust
  model — but anyone widening what `/data/` serves should know it is there.
- **Measured against both running boards** (`tools/api_path_sweep.py`, ticket `79` §2): 17 payloads
  × 4 routes + 2 controls = 70 rows, **0 payload rows in 2xx**, histogram `400×4, 403×18, 404×46`,
  both controls 200 with real file bytes. Identical on the M5 Paper Color (flash volume) and the
  E1002 (card), and **identical again after the extraction**, which is how the refactor was shown to
  change nothing that a device can see.
- **Two apparatus traps in that sweep, both of which produced a false result first.** `curl`
  collapses `..` in a path unless given `--path-as-is`, so a naive sweep tests its own client — the
  tool writes the request-target by hand for that reason. And `send_file()` answers with
  `httpd_resp_send_chunk()` on a connection this server keeps alive, so **a client that reads to EOF
  times out on a response that arrived in 200 ms**: the sweep's own controls "failed" that way twice
  before the client was taught to stop at the headers plus a peek. The server was healthy throughout,
  checked with `curl` immediately afterwards. `--selftest` adds a row that must be reported as a
  failure, so "0 payload rows in 2xx" is a sentence a broken sweep cannot print.
- **The listing's `char encoded[192]` is a real ceiling, not a formality.** `h_photos_list()` builds
  each `url` with `req_url_encode()` and skips the photograph when it does not fit — so a name over
  63 UTF-8 characters is **silently absent from the grid** while `photo_list` still counted it in
  `total`. Unreachable through either mirror (`SMB_MANIFEST_NAME_SIZE` is 64 and uploads are given
  generated names) and reachable with a card reader. Pinned by a test at exactly 63 and 64
  three-byte characters; left as it is rather than fixed, because the fix is a bigger buffer in a
  function that already reads 500 names.
