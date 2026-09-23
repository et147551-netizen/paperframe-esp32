# The SMB mirror

Moved out of the project index verbatim on 2026-09-09 so it is read when it bears on
the task rather than loaded every session. Nothing here was rewritten.

**Two of this module's decisions are host-tested since 2026-09-21, and they are the two that used to
be readable only by running the mirror.** Both moved to `src/core/`, leaving an adapter behind in
`app_smb_sync.c`; the reason each is worth its own file is in its header.

| Decision | Now in | Its test |
| --- | --- | --- |
| may a window open right now (`run_is_due()`) | `src/core/smb_window.c` | `test/test_smb_window/`, the four branches in precedence order |
| may this cached photograph be deleted | `src/core/smb_evict.c` | `test/test_evict/`, the protected set |

`smb_window_is_due()` is the rule tickets `28`, `42`, `37` Phase 3 and `59` each revised — five arms
and two wrong explanations in `59` alone — and it had never executed on the host. **The distinction to
know before touching it: ticket `42`'s active-hours hold is on the periodic branch ALONE**, and the
other three (an open window, a manual request, the first run) are each exempt for their own stated
reason. Moving the hold above the on-demand branch strands a want declared just before the quiet hours,
and two cases in `test_smb_window` now fail if you do — the second of them was added only after that
reordering was tried on purpose and the case claiming "the periodic branch alone" stayed green.

`smb_evict_may_drop()` is the protected set and nothing else: **the walk stays in `app_smb_sync.c`**
because it deletes files, mutates the manifest it is iterating and holds the lock. What each mistake
costs — a blank frame, an unbounded fetch-and-evict loop, a stranded want — is in `smb_evict.h`.
The periods are still `app_smb_sync.h`'s: `SMB_SYNC_PERIOD_MS` is `#ifndef`-guarded so a build can
override it, and they reach the predicate as a parameter.

## The SMB mirror

Given a host, share, folder and credentials the frame mirrors photographs into `/data` on a
timer and keeps them in step. The measured good path is eight files in two windows at
**189 KB/s** (76-82 before ticket `33`), an idempotent second sync, a deletion propagating, a
Japanese name landing as `smb_<8 hex>.jpg` because FAT/CP437 cannot hold it, and eight failure
arms each reporting as themselves.

**Verify a sync by content, not by `last_error`.** `.scratch/digital-frame/mirror_digest_check.py`
pulls every file `/data` serves back over HTTP and compares sha256 against the share. That is
what found ticket `34` — a manifest naming 13 files where the card held 8, with `last_error: ok`,
`capped: false` and nothing in the console — and it is the only check that can catch a mirror
that writes the wrong bytes.

- **`/data/.smbidx` is the ownership record: a file it does not name is never touched.**
  That is what protects web-UI uploads and the four factory PNGs. A manifest swapped before
  `reconcile_deletions()` turns a deletion into a leak, and that is an open defect in
  ticket `27`. **But the manifest is not evidence about the card**: until ticket `34`,
  `build_plan()` read "already current" out of `.smbidx` without ever asking whether the file
  was there, so a photograph deleted from the card was never fetched again. It now `stat()`s.
- **Every file this mirror writes goes through `board_storage_write_atomic()`, and until ticket `78`
  the ownership record was the one that did not.** Four files on `/data` are written whole, parsed
  back and believed — the photograph, `.smbdir`, `.smbidx` and the Google side's `.gpidx`/`.gpdir<n>`
  — and two of the four wrote at the real name, where `fopen("wb")` truncates at open and leaves a
  prefix there for the whole duration of the write. The two were `.smbidx` and `.gpidx`, i.e. **the
  two whose loss cannot be recovered by re-reading anything else.** A truncated `.smbidx` reads back
  two ways and only one of them is reportable: a cut inside a record line fails
  `smb_manifest_parse()` and `manifest_load()` logs it, while **a cut exactly on a record boundary
  parses, silently, as an index of a smaller cache** — 100 of this card's 8,197 cut points, measured
  by the truncation sweep in `test/test_smb_manifest`. Either way the files the missing records named
  become unowned, and **an unowned file is invisible to eviction**, so the cache's ceiling then bounds
  only the owned set. Do not write a record file here any other way; the helper takes the storage
  lock itself, which is why a caller must not already hold it.
- **Pointing the mirror at an empty folder empties the mirror**, correctly — an empty
  listing is a success with zero files, not a failure.
- **A sync stops httpd for a bounded window** — four files or 60 s — and the worker task is
  created per window and self-deletes, because a resident 8 KB stack would hold half the idle
  internal headroom to do nothing. **Whether it still needs to stop httpd is now half
  answered, and the halves go opposite ways** (2026-09-06): a *connect* coexists with httpd
  comfortably — `POST /api/smb/test` costs 75 ms and ~13.7 KB of internal RAM with httpd up
  and does not move `dma_largest` — while a *listing* does not, because a run over a
  **243-entry** directory bottoms out at `int_min` 13,207 B **with httpd already stopped**,
  against httpd's own 10 KB task stack. So the window's exclusion is load-bearing for the part
  that lists, and the cheap operation was separable all along. See
  `docs/agents/measurements.md`.
- **`smb_on_demand` (2026-09-06, default off) makes `/data` a CACHE OF WHAT IS BEING SHOWN rather
  than a mirror of one folder**, and it is what puts the whole share on the wall — verified drawing
  photographs from all six folders. The hourly run stops fetching a folder and only refreshes the
  catalogue, on its **own** clock; photographs are fetched because `app_smb_sync_want()` asked, in a
  window that does **no listing at all**; the cache is bounded at `SMB_SYNC_CACHE_FILES` = 100,
  evicted oldest-fetch-first. Ticket `37` Phase 3 has the design, the results and what is given up.
  **Since 2026-09-13 the slideshow reaches this catalogue through `src/app/app_catalog.c`**, which puts
  the Google Photos album's list after it (ticket `60` §3e) and counts the share only while
  `smb_enabled` is on as well as `smb_on_demand` — a switched-off share used to go on being selected
  and missing.
  **That bound is not medium-aware and nothing on the on-demand path is** (ticket `40`, 2026-09-08):
  100 files at this share's 190 kB mean is ~19 MB against the internal volume's 6 MB, and
  `compute_caps()`'s caps are checked in `build_plan()` only, so with no card fitted the cache never
  converges. It fails noisily rather than corrupting — `write_local()` short-writes, unlinks the
  `.part` and returns `ESP_FAIL` — and it is unreached only because a card has always been in.
  Four things not to undo:
  - **This is the "the mirror never evicts" rule surviving a changed meaning of `/data`, not a
    reversal** — that rule was about a library, and the library is now the share.
  - **Eviction is FIFO because on-demand appends straight to `s_have` and nothing else does**, so
    record order *is* fetch order. Hence `smb_manifest_remove_at()` is order-preserving and its test
    asserts the sequence, not the count.
  - **A fetch REPLACES any existing record for the path.** Appending grew a second record per
    re-fetch and eviction then unlinked the file while the other record still owned it; it is also
    on-demand's only healing path for ticket `34`'s direction, there being no `build_plan()` here.
  - **THE FRAME NOW FETCHES ONE PHOTOGRAPH PER PHOTOGRAPH SHOWN, and it used to fetch 5.7** (tickets
    `44` and `45`, five runs 2026-09-08). Two independent causes, and the second was found by fixing
    the first. **`SMB_SYNC_WANT_MAX` is 1**: the selector declares that many ABSENT entries per
    advance and the cursor consumes ONE, so anything above 1 makes the horizon diverge — at 6 it
    settled ~60 positions out with **85.2 % of fetches evicted before being drawn** (73.5 an hour,
    ~321 MB a day), at 2 it walked out one position per advance, at 1 it holds. **And the hourly
    catalogue refresh used to renumber the share** (`45`), which is why a photograph fetched within
    ~20 min of a refresh was *never* drawn; `smb_catalog_restore_at()` puts a re-listed folder back
    where it was. Together: **12.5 fetches an hour, ~55 MB a day, `miss=` 0 of 22 advances, nothing
    fetched and not shown.** **The counting is the trap here**: `fetch_distinct` and any `sort -u`
    over `want pop` count catalogue INDICES, so this was first written up as "204 fetches for 90
    photographs, 55.9 % re-fetched" and **that is retracted** — by path it was 3.5 %, and 28 of 28
    index repeats were different files. Count by path; `catalog_count` holding at 1,032 proves
    nothing, because a refresh reorders without changing a total. **And a fetch count cannot see a
    lookahead at all** — map the index to its epoch position with `shuffle_predict.py`.
- **`smb_resize` (2026-09-09, **default ON** by operator instruction) stores a photograph at the
  size the panel draws it and writes its thumbnail beside it, instead of the share's own bytes**
  (ticket `49`, verified on hardware). Measured: **0.287x** stored size on the two thirds of this
  share that reduce, decode 1073-1196 ms plus encode 107-142 ms a photograph, `/thumb/`
  **0.089-0.249 s with a sidecar against 1.41-1.63 s without**, no watchdog, PSRAM floor 4.58 MB.
  It buys storage, thumbnails and card wear and **not display speed** — the decode is already
  264 ms of a 3 165 ms render. Six things not to re-derive:
  - **It did nothing for a 12 MP photograph until the copy that does nothing was deleted, and
    now a 4032x3024 JPEG stores at 180,378 B where it stored 2,712,401** (measured 2026-09-09,
    both arms, the before one on the unmodified firmware). **For JPEG the box filter is ALWAYS a
    no-op**: `choose_jpeg_scale()` leaves the decode between 1x and 2x the fit, so an integer
    factor of 2 would undershoot and the factor is always 1 — `epd_image_take_rgb()` hands over
    the decoder's own buffer instead of allocating a second one, and the alignment crop happens
    in place. The box filter still earns its keep for PNG and BMP, which the decoder cannot
    reduce. **Two things it does not fix**: the decode trips the task watchdog in `smbsync`, and
    `httpd_down` is 33.4 s for that one file.
  - **And the ratio the integer factor could not express is now applied too** (ticket `52`,
    measured 2026-09-09). `src/core/img_scale.c` reduces by the RATIO — area average with fractional
    edge weights — so the 12 MP photograph went **180,378 → 50,107 B** at 592x448, and against
    its 2.7 MB source that is **0.018x**. Eight fixtures, `fit=` matching the produced size on
    every row. `store_photo()`'s gate had the same defect one level up and asked
    `epd_fit_reduction()` for an integer: it now asks `img_scale_fit_target()`, which is what
    stopped a 1200x1200 photograph being kept whole. **A file that does not shrink is not a
    failure** — two fixtures were already at the box fit, and re-encoding them would lose a
    generation for nothing. Costs ~1.4 s on a 12 MP photograph (an estimate, n=1 either side).
    **The host suite for it passed with the fractional weight replaced by 1** until a case was
    built that could tell them apart; `docs/agents/measurements.md` has that account, and it is
    the "a check that cannot fail is not a check" rule landing on a unit test.
  - **THE BENCH SHARE IS TEST DATA and every figure derived from its composition is retracted**
    (2026-09-09): synthetic images, portrait only because of how they were made. A real
    library mixes portrait and landscape. So "a third is not reduced", "0.287x", "0 files carry
    EXIF" and "auto-rotation affects 7 files" are all artefacts. What is NOT retracted is
    everything measured about the machinery. `real_corpus.py` re-derives it; ticket `50` ranks
    what to do about it, and **auto-rotation was the largest single improvement available** —
    now built and measured, ticket `51` and the rotation section of
    `docs/agents/render-pipeline.md`.
  - **The factor comes from the SOURCE HEADER, before anything is allocated** (`img_image_dims()`).
    It did not, and every 1024x1024 photograph failed: a full-resolution decode plus a
    full-resolution copy is 6.29 MB against 6.44 MB of PSRAM. `resize_core()` can still reach an
    internal factor of 1 after the decoder's own reduction and copy for nothing — that is the
    12 MP case, unreachable under the 4 MB ceiling and open.
  - The reduction is `epd_fit_reduction()`'s binding-ratio rule and **the long-edge rule upscales
    on the glass**; the fit target is a **square** of the panel's long edge so a stored file serves
    either `orientation` — 600x600 here. **`SMB_RESIZE_FIT_EDGE` was `EPD_HEIGHT` until 2026-09-17**,
    which is the long edge only on a portrait-native panel: on the ED2208-GCA it was the short one
    (480) and every mirrored photograph was stored too small for the 800 px axis. It is
    `max(EPD_WIDTH, EPD_HEIGHT)` now — a no-op on this panel, so no figure below moves, and ticket
    `63`'s E1002 resize figures were taken at 480 and are superseded.
  - The local name is kept even when a PNG becomes JPEG bytes, because it is the manifest's
    identity and the eviction key, and `send_file()` therefore types by content sniff.
  - The sidecar is `<name>.thm` so `photo_list_is_image()` cannot list it as a photograph;
    `delete_local()` and `h_photos_delete()` are the two places it is unlinked, and
    `store_photo()` clears a stale one before every write.
  - **Both sidecars live in `/data/.thumbs/`, not beside the photograph, since 2026-09-23**
    (operator's request, so the card reads as photographs on a PC; it also frees a FAT16 root).
    Every path is built by `app_smb_sync_sidecar_path()`. `app_smb_sync_sidecars_prepare()` runs
    at boot before the slideshow and after a media change: it creates the folder, moves any
    root-level `.thm`/`.mta` into it, and prints `# sidecars: dir= moved= failed= part=`. Nothing
    that lists `/data` sees the folder, because both scans skip `DT_DIR`. The API cannot name
    it either, because `req_name_is_safe()` refuses `/`.
    **At boot only, it also unlinks ORPHANS**: sidecars whose photograph is not in the root. A
    photograph deleted on a PC leaves its hidden sidecars behind, and a later photograph given
    the same name would be served the old thumbnail by the frame itself. Boot only, because
    `store_photo()` writes the `.mta` BEFORE the photograph, so a sweep beside a running mirror
    would delete a fresh one. After a runtime media change, `sweep=off`. It refuses
    (`sweep=skipped`) unless the root listing was complete, including when the root would not
    open. Ran on both boards 2026-09-23 with `orphans=0` and every thumbnail still a sidecar hit.
    **Deleting a real orphan has not been observed**: making one needs the card on a PC.
  - **`mirror_digest_check.py` cannot verify a resized mirror** — its `--resized` is a
    name-and-size plausibility check, so verify with the setting off.
  - **Internal RAM fell**: the `int_min` floor went 24,355 B with the setting off to 15,643 B
    with it on in the same boot, and 6,027 B over a longer run — the second-lowest recorded here.
    The encoder's ~10 kB of DRAM, opened twice per import, is the hypothesis; the heartbeat
    prints the minimum without the moment, so it is not attributed.
- **A JPEG's EXIF orientation is applied at IMPORT, in `resize_core()`, and nowhere else**
  (ticket `53`, 2026-09-09). A phone stores a portrait photograph as a LANDSCAPE frame plus
  `Orientation=6`, and before this the tag was not read at all: three fixtures differing only in
  the tag stored **byte-identically**, and `auto_rotate` then turned the landscape-shaped file to
  fill the panel — **upside down for a tag of 6 and upright for 8** at `orientation=portrait`,
  measured on the glass later that day and the opposite way round from what was first written
  here. Measured after: `e6` stores 448x592 where its control stores 592x448, `e3` moves four
  bytes, the controls do not move, and the thumbnails read correctly on the glass-free check.
  Four things not to re-derive:
  - **It is applied AFTER the reduction for a memory reason.** A 90 degree turn needs a second
    buffer of the same size; here it is bounded by the square fit box (1.08 MB), while doing it in
    `jpeg_open()` — the seam that would cover the display path too — puts a 12 MP peak at ~7.2 MB
    against 6.36 MB of PSRAM. **The draw path is therefore still unturned**, which matters for
    files imported before this, and for `smb_resize` off; freeing `r->src.owned` after the decode
    is the first step of that follow-up. A **browser** upload is already upright, because the
    canvas honours the tag — **`POST /api/photos/upload` does not**, which a tagged `curl` upload
    scanning identically to its untagged control is what proves (ticket `51`).
  - **So there is a SECOND sidecar, `<name>.mta`, written where the orientation is read** (ticket
    `64`, 2026-09-19). Three optional lines -- `C TOKYO`, `N JAPAN` and `D 2019-08-14` -- and
    **nothing at all when the source had no EXIF**, so an EXIF-less file costs no directory entry.
    **The city and the country are separate lines and that is load-bearing**: the band drops the
    country first when its one line is full, and recovering the pieces from a joined `TOKYO JAPAN`
    by splitting at the last space works for JAPAN and fails for UNITED STATES. They were joined
    here until 2026-09-19 only because the band then took one string. The position
    is geocoded against a flash-resident public-domain table (`src/core/geo_city.h`) and then
    **discarded**: what reaches the card is the name, never the coordinates, which is "derive before
    you retain" applied to the one place it would otherwise put four thousand GPS tags on a card
    that leaves the building. Written at the TOP of `store_photo()`, before any branch, because
    every path out of that function either re-encodes `buf` or stores it for the next re-fetch to
    replace. Same lifecycle as `.thm` in all four places that matter: cleared before a re-fetch,
    unlinked by `delete_local()` and by `h_photos_delete()`, skipped by the directory scan's suffix
    test, and invisible to `photo_list_is_image()`. The full year is stored and the band shortens it
    to two digits at draw time -- storing two would make a 1985 photograph read as 2085.
  - **And the re-encode destroys the whole EXIF block, not just the tag it honoured** (read
    2026-09-19, ticket `64`). `store_photo()` re-encodes through esp_new_jpeg on every path except
    its `nothing_to_reduce` branch, so with `smb_resize` on — the default — **a mirrored photograph on
    the card carries no EXIF at all**. Import is therefore the ONLY opportunity to read anything from
    it: capture date, GPS, lens, any of it. `img_exif_orientation(buf, len)` is already called at
    exactly the right moment for that, just before the `nothing_to_reduce` test. Anything wanted must
    be taken there and persisted as derived data; a draw-time EXIF read returns nothing for most of
    the library and **looks like a feature that is merely rare rather than one that cannot work.**
    Uploads never had EXIF to begin with — the browser's canvas re-encodes first.
  - **Nothing above `resize_core()` had to change, because both entry points pass a SQUARE rule
    argument** — so the hint, `epd_fit_reduction()` and `img_scale_fit_target()` are invariant
    under the exchange of width and height. That is `SMB_RESIZE_FIT_EDGE` earning its keep a
    second time, after ticket `51`.
  - **`store_photo()`'s `nothing_to_reduce` shortcut also requires orientation 1**, or a tagged
    file already inside the box keeps its own bytes while its thumbnail is turned, and the grid
    and the panel disagree — which reads as a thumbnail bug rather than as this.
  - **The re-encoded file carries no APP1**, read off the device rather than asserted, so nothing
    downstream turns it a second time.
- **The on-demand cache is bounded by the VOLUME's free space as well as by a file count**
  (ticket `54`, 2026-09-09). It had no space check on any medium — `compute_caps()`'s "checked
  before every fetch" describes `build_plan()`, and ticket `37` Phase 3 did not carry the caps
  into the on-demand window. `cache_has_room()` now runs once per window and **before the
  connect**, because eviction takes the storage lock and that must never be held with a live SMB
  session waiting; under `smb_cache_reserve_bytes()` = `clamp(total/16, 1 MB, 32 MB)` it evicts,
  re-reads and then **skips the window's fetches with `capped`** rather than short-writing. The
  catalogue refresh still runs. Five things not to re-derive:
  - **`board_storage_usage()` costs `usage=0 ms`** on the fitted card, printed every window. The
    once-per-window placement is now a lock-ordering decision, not a cost one.
  - **Eviction does not free a computed number of bytes.** The manifest's `size` is the SHARE's,
    and a `stat()` per record is the 22-38 ms call ticket `36` removed. It evicts up to
    `SMB_SYNC_EVICT_PER_WINDOW` and re-reads the volume; a window that did not free enough does
    not fetch. Measured draining 59 → 51 → 43 → 35 → 8 with free space rising every time, which
    is the check that FATFS accounts an unlink immediately and the loop cannot spin.
  - **`SMB_SYNC_CACHE_MIN_FILES` = 8 is a floor only the space path has**, or a volume full for
    reasons the cache did not cause would be answered by emptying the cache to nothing.
  - **Which is what makes ticket `27`'s remaining orphan window couple to this bound** (read
    2026-09-10). Eviction walks the **manifest**, so a file no manifest owns is never a candidate:
    it holds space that is then reclaimed by evicting something the manifest *does* own, so an
    orphan **permanently displaces a cached photograph** and enough of them present as `capped` on
    a card with no room. It is ~0.4-0.8 MB per power cut, so this is about what the symptom will
    look like rather than about when. **Do not "fix" it with a name sweep** —
    `smb_manifest_local_name()` keeps the share's leaf verbatim, so mirrored files and Web-UI
    uploads are not distinguishable by name. Ticket `27`'s 2026-09-10 section has the whole
    argument and recommends counting orphans in `GET /api/smb/status` rather than preventing them.
  - **That count now exists, and what it is called is not what it counts.**
    `unowned_files` / `unowned_exact` (built 2026-09-10) report **photographs on `/data` that the
    manifest does not own** — the factory images, every Web-UI upload and every orphan alike,
    because by `smb_manifest_local_name()`'s design those are the same thing by name. **It is a
    baseline plus the leak, so the reading is the number MOVING with no upload behind it**, never
    the number itself; `unowned_exact` false makes it a lower bound. Bench baseline 2026-09-10:
    **10**, of 110 photographs against 100 mirrored. The count is a by-product of
    `local_set_build()`'s existing walk on the mirror path and a walk of its own
    (`count_unowned()`, ~82-98 ms) on the on-demand path, because **`build_plan()` is the mirror
    path only** and on-demand would otherwise never produce it.
  - **`bytes_mirrored` is the SHARE's bytes, not the card's** — 198,820 a file against a 42,090 B
    stored median — and has been since import-time resize. `GET /api/storage` is the honest route.
  - **The internal-volume pause in `app_smb_sync_tick()` STAYS.** Ticket `40`'s account of an
    unconverging cache was already unreachable because of it, and lifting it — which would let a
    cardless frame show the share — needs ticket `08` steps 4-8, the card physically out. An
    override exercised the reserve branch with a card in; it cannot fill a 6 MB FATFS volume.
- **A photograph over `SMB_SYNC_FILE_MAX` (4 MB) is catalogued, selected, skipped and stepped past
  — and the ceiling's stated reason is not the cost it has** (ticket `47`, measured 2026-09-09).
  `accept_photo()` is name-based, so the entry enters the catalogue; the fetch logs
  `skip <path>: <bytes>` and the cursor holds for `EPOCH_MISS_MAX` before stepping past. **The cost
  is an outage proportional to the file, not the throughput** — a 3 MB fetch held `httpd_down` for
  45.5 s — so raising the ceiling only moves a PSRAM failure into reach; chunking the fetch into
  `<name>.part` is the option. **Three apparatus facts that save an hour:** `POST /api/smb/sync`
  does **not** refresh the catalogue, the first window after a **boot** always does, and
  `GET /data/.smbdir` serves the catalogue over HTTP.
- **Do not quote the 123 KB/s aggregate as a throughput.** 14 % of 4 KB reads still stall at ~4 s;
    the 86 % that do not give 188.1 KB/s. `httpd_down` is 27 % of a run at a one-minute interval.
- **A window that opens during a large upload records an `httpd_down` as long as the upload**, and
  that is the largest figure this project has: **56,943 ms**, measured 2026-09-10 while an 8.1 MB
  `POST /api/photos/upload` was in flight (`fetch=0 catalogue=0`, so the window itself did nothing).
  `httpd_stop()` waits for the in-flight handler, so the window inherits the upload's length. **Why a
  window opened on top of an upload at all is answered 2026-09-12 in ticket `59`: `mark_activity()` is
  called when a RESPONSE is sent and nowhere in the receive loop, so a body streaming for longer than
  `SMB_SYNC_QUIET_MS` (30 s) makes the frame look idle to the quiet gate.** An 8.1 MB upload is over
  that; the 931 KB ones measured on 2026-09-12 are not, and no window ever opened mid-upload for them.
  **That explanation is right about the gate and wrong about the interval, corrected 2026-09-16 by
  five arms (ticket `59`).** The receive loop now stamps — so does the handler's first statement — and
  windows still opened on connected clients, because **the gate's own reading says the frame really was
  idle: `window due: idle=` 37.8-42.5 s, `run=0 manual=0`, while a client sat connected and
  undispatched.** A 931 KB upload is handler-visible for only **7.7-12.7 s**, against a client wall
  of 76-83 s; the rest is time no stamp can reach. Read `window due:` on any future arm before
  attributing a window to anything — `run=1` means the run was already active and the gate did not
  apply at all.
  Ticket
  `26`'s 12.7 s is the figure for a window that does *not* overlap one. **The suspected consequence,
  PROVEN 2026-09-12, n=7 (ticket `59`):** an upload that *arrives* while httpd is down dies after
  **exactly 130,826 bytes** with `curl` exit 56 and `http=000`, the same count every time, in
  1.04-1.30 s — while one already in flight finishes, 10 of 10. The earlier 8.1 MB death at 38.0 s was
  this. Tonight also priced the **sidecar's** share of the wait: a window following a 931 KB upload
  recorded 34,372 ms against 20,768 ms for the same `fetch=6.8 s` window with nothing near it, so an
  upload's post-response thumbnail work is inside the outage. 56,943 ms above is still the largest.
  The original wording, kept because what it cost is the lesson: an earlier 8.1 MB upload died at
  38.0 s with `http=000` and nothing on the
  console, which is what a stopped `httpd` would look like from `curl` and is indistinguishable from
  anything else — same trap ticket `10` recorded for `POST /api/wifi/config`.
- **`smb_path` is a COMMA-SEPARATED LIST of folders, and `/data/.smbdir` is the catalogue of what
  is on the share** (ticket `37`, 2026-09-06). Element 0 keeps its old meaning — the folder the
  mirror itself works in — so a single-valued setting is a one-item list and nothing changed for
  it. One folder is listed per run, round-robin, and `smb_catalog_drop_folder()` replaces that
  folder's records rather than merging them — **and `smb_catalog_keep_folders()` (2026-09-09) drops
  the records of a folder that has LEFT `smb_path`, which nothing did before**: `drop_folder()`
  only ever runs for a folder the round-robin still lists, so a removed folder's entries stayed for
  good, were selected like any other, failed to fetch and cost `EPOCH_MISS_MAX` advances each. Found
  by ticket `51`, fixed under ticket `37`, and the device was carrying **12** of them across two
  dead folders. Two things not to undo: it runs in `catalogue_next_folder()` rather than where
  `s_folder_list` is written, because that function already holds the lock and already stores; and
  **an empty list drops nothing**, because an empty `smb_path` means the share root, which a list
  cannot name. Verified on hardware: **1,032 photographs catalogued
  across six folders**, every folder's contribution matching a server-side count to the unit,
  `int_min` unmoved at 13,207 B, and the total unchanged across a reboot and a re-listing.
  **The round-robin cursor and the shuffle seed ride in `.smbdir`'s own header
  (`#smbdir 2 <cursor> <seed>`), because the RX8130's battery-backed RAM is only 4 bytes and the
  slideshow's index already holds two of them** — the six bytes the plan assumed were there are not,
  and NVS would mean ~288 flash writes a day. The file is rewritten once per run anyway, so this
  costs nothing. Version 1 is rejected rather than migrated and says so in the log.
  `GET /api/smb/status` reports `catalog_count` / `catalog_folders` / `catalog_truncated` —
  **`catalog_count` is what exists to choose from and `files_mirrored` is what is on the card, and
  the two being different is the design working.** Since Phase 3 the frame *selects* over the
  catalogue and fetches on demand, so with `smb_on_demand` on the difference is permanent by design
  rather than a shortfall; with it off the frame still shows only what the mirror fetched.
- **`SMB_SYNC_LIST_MAX` is 500, not 200, and raising it was free** — it buys PSRAM (136 KB of
  7.5 MB) and nothing in internal RAM, because the listing's cost follows the directory. The 200
  had been hiding 30 photographs of `202606`'s 230 behind a `TRUNCATED` flag.
- **A listing costs 142.5 bytes per DIRECTORY ENTRY, linearly and with no intercept, and
  `SMB_SYNC_LIST_MAX` does not control it** (measured 2026-09-06, 21 samples at 129 dirents and
  one at 1,080, each predicting the other to 0.6 %). `smb2_opendir` materialises the whole
  directory before the first `smb2_readdir()`, so the draw follows what the folder holds and not
  what the cap keeps: the `photos` root's 1,080 dirents cost **153,784 B**, which is over
  twice `env:frame`'s entire internal free heap. **`env:frame` can list about 315-420 entries and
  no more**, and paging the listing cannot help because the allocation happens before the walk.
  Two earlier claims died here: the `0.5 KB an entry` was 3.6x too high, and "the dip grows and
  then flattens" was two whole-window aggregates hiding a clean slope. `board_smb_list()` now
  records the directory's own `dirents` and logs it, so every new capture carries its own count.
- **The mirror rejects any leading-dot name, and that is a deliberate departure from FR-4.6**
  (ticket `38`). `smb_manifest_local_name()` is a superset of `photo_list_is_image()`, not the
  same filter: the extension test alone accepted the twelve
  `.trashed-<epoch>-album_v27_<id>.jpg` on this bench's share, which are photographs the user
  deleted on their phone. They stayed off the card only because 230 visible `.jpg` overflow the
  200-entry cap and `.` sorts after `a` descending — an accident, and ticket `37` removes that
  cap. `photo_list` keeps the ported rule verbatim for `/data`.
- **The period is one hour, and a run with nothing to fetch costs 1.2 s of httpd** (measured
  2026-09-06, 200 mirrored files against a truncated 200-entry listing): connect 156 ms, plan
  1025 ms — of which the server's own listing is ~908 — fetch 0, finish 31 ms. The period used
  to be six hours because a run was assumed to be a mirroring pass; `window_task()` in fact
  skips the fetch loop entirely when the plan is empty, so the cheap check the Android side gets
  from a separate worker was already here.
- **`build_plan()` finds out what is on the card with one `readdir`, not one `stat()` per file,
  and `stat()` is why.** Measured before the change: 200 probes cost 7663 ms, **38.3 ms each**,
  while taking the storage lock 200 times cost 7 ms in total — so the cost is FATFS rescanning a
  215-entry directory to resolve each name, which made the loop quadratic. One scan is 83 ms, and
  the check run went 8.8 s → 1.2 s. **Ticket 34's invariant is unchanged**; what went with the
  `stat()` is its `st_size > 0` test, which is no longer needed because `write_local()` writes
  `<name>.part` and renames — a zero-length mirrored file is now unreachable rather than detected,
  and a leftover `.part` is unlinked by the next scan. A directory over `PHOTO_LIST_MAX`, or a name
  longer than a set slot, marks the scan incomplete and falls back to `stat()` for what it cannot
  answer, because absence from an incomplete listing is not evidence of absence.
- **`POST /api/smb/test` connects, tree-connects and disconnects — no listing, no transfers,
  and it does not stop httpd.** It is the only SMB route not gated on `smb_enabled`, because
  the point of testing is to find out before switching the mirror on. The verdict arrives in
  `GET /api/smb/status` as `test_state` / `test_error`, reported apart from `last_error` so a
  stale sync failure cannot masquerade as a verdict on settings the user just fixed. A dead
  host answers "cannot reach the server" in 12,020 ms, which is `BOARD_SMB_CONNECT_DEADLINE_MS`
  and not a coincidence.
- **A manual sync bypasses the quiet gate; a scheduled one waits at most 10 min** from
  becoming due and then goes anyway, and the gate applies to *starting* a run rather than to
  continuing one. The captive portal's wildcard DNS makes any associated client poll `/`, so
  a gate on total silence never opens.
- **Reads go in 4096-byte chunks** and every libsmb2 call runs inside `board_smb.c`'s own
  deadline loop, because 16 KB and 32 KB `smb2_pread` stall deterministically and libsmb2's
  own timeout cannot fire for this stall by construction. Stalls still happen and are
  recovered by reconnect-and-resume at a cost of ~3 s.
- **What cost the mirror its speed was SMB3 signature verification, and it is now fixed —
  189 KB/s in the shipping application, 2.4× (ticket `33`, 2026-09-06).** The transport was
  exonerated first: it receives at 893 KB/s at the shipping window against the mirror's
  84 KB/s. But the round trip was not the chunking either. **82 % of a 4 KB read was AES-CMAC
  signature verification on the CPU**, at 4.895 µs a byte, because libsmb2 re-runs the whole AES
  key expansion for every 16-byte block (`lib/aes.c:444-456`) and this share requires signing.
  `src/app/smb_aes_hw.c` redirects that AES to the S3's AES peripheral with a linker wrap. Measured
  three ways plus an A/B in one run, and the control is the SMB-2.1 arm, which signs with
  HMAC-SHA256 and did not move.
- **Pipelining the reads exists, is measured, and is off.** `board_smb_set_read_depth()` keeps
  several `smb2_pread_async` in flight; it is worth 1.33× at `WND 16384 / mbox 16` and **at the
  shipping `5760 / 6` every depth above 1 fails** with `Wrong signature in received PDU`. The
  default is 1, `board_smb.h` carries the condition, and the wrap is not the cause — signature
  verification is deterministic, so a wrong implementation could not fail intermittently.
  Half the signing cost is still there and `docs/agents/measurements.md` says what closing it
  would take.
- **Watching a sync by polling `/api/smb/status` is what stops the sync.** It applies to an
  on-demand fetch as much as to a whole-mirror run: a 30 s poll held one want unfetched for seven
  minutes on 2026-09-22 (ticket `81` §3). Wait in silence and read once.
- **An empty `/data` is where an on-demand cache starts, and until ticket `81` it could not.**
  `step()` returned on `count == 0` before the catalogue epoch, so nothing was ever wanted and a
  fresh card stayed blank with the whole share catalogued and `last_error: "ok"`. That branch now
  steps the catalogue for its wants, and the first photograph is drawn as soon as it lands (§8
  there), not at the next advance an interval later.
