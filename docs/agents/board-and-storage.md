# The board: internal RAM, storage, the panel, and running firmware

Moved out of the project index verbatim on 2026-09-09 so it is read when it bears on
the task rather than loaded every session. Nothing here was rewritten.

## How `src/` is layered, and where a board's specifics live

Since the ticket `43` layering pass of 2026-09-15 this project supports **two boards** -- the
M5Paper Color (EL040EF1, 400x600) and the Seeed reTerminal E1002 (ED2208-GCA, 800x480) -- and the
directory a file is in says which layer it belongs to.

```
src/core/     no ESP-IDF, no panel, no board. What env:native compiles and the host
              tests cover. Its ONLY dependency downward is panel/epd_geom.h.
src/panel/    epd_panel.h is the interface; epd_geom.h and epd_cmds.h are pure headers;
              el040ef1/ and ed2208gca/ are the two drivers.
src/board/    board.h is the interface; board_storage.c, board_led.c and board_buttons.c
              are the generic halves; m5papercolor/ and reterminal_e1002/ are the two
              implementations.
src/app/      the application: server, settings, display, slideshow, the two mirrors.
src/entry/    ten app_main()s, each guarded by its own -DBUILD_* flag.
```

**One build flag selects a board and there is no default.** `-DBOARD_M5PAPER_COLOR` or
`-DBOARD_RETERMINAL_E1002`; `board.h`'s `#else` arm is an `#error`. `docs/agents/build-system.md`
has what that costs and the one trap (`platformio_local.ini` replaces `build_flags`).

**The board interface is short because that is what the code needed.** `board.h` declares
`board_i2c_init`, `board_i2c_scan`, `board_epd_power`(`_state`), `board_power_read`,
`board_card_power`, `board_card_present`, `board_sht40_read`, `board_rtc_read`,
`board_rtc_seconds`, `board_state_read`/`_write`, plus `board_power_t` and `board_datetime_t`.

**`board_sht40_read()` has an application caller since 2026-09-18** — `GET /api/system/info`, which
puts the ambient temperature and humidity on the Settings → System panel. Two things follow. It is
the first caller to read the sensor *repeatedly*, and that is what exposed a wait of one FreeRTOS
tick against an 8.2 ms conversion: 7 of 8 reads failed, the fix is two ticks, and **both boards were
fixed and both measured 12 of 12 afterwards** (`docs/agents/defect-log.md` 2026-09-18).

**And the "no mutex on this I2C bus, so they take turns by luck" line that stood here until
2026-09-21 was WRONG, in a way that matters** — it was read from this project's own files and never
checked against the driver. **The IDF `i2c_master` driver has a per-bus mutex of its own**:
`s_i2c_synchronous_transaction()` takes `bus_lock_mux` (`esp_driver_i2c/i2c_master.c:930`) and every
one of the four public transfer calls — `i2c_master_transmit`, `_receive`, `_transmit_receive`,
`_multi_buffer_transmit` — goes through it. Both boards use `driver/i2c_master.h`. So **two tasks
cannot interleave the bytes of two transactions**, and neither `board_pm1.c` nor `board_e1002.c`
needs a lock for that.

**What the driver's lock does NOT cover is a SEQUENCE**, because it is released between
transactions — and `board_sht40_read()` is exactly that: write `0xFD`, wait 20 ms for the
conversion, then read six bytes. Two tasks do call it (the display task once per render for the
matte band's climate string, and the HTTP worker). A second `0xFD` arriving while the sensor is
converting is NACKed, so the loser's read fails and `temp_c` is **absent** — the same symptom as the
one-tick wait, from a different cause. **Ticket `76`** carries it, and the same shape applies to any
read-modify-write of a charger register (only `app_charge`'s single task does that today). Ticket
`73` is the identical hazard on the E1002's battery ADC, where it was fixed with a mutex around the
sequence rather than around the bus.
`board_spi.h`, `board_storage.h`, `board_led.h` and `board_buttons.h` are the other four
board-facing headers and kept their shapes.

**Three things worth knowing before touching it:**

- **`board_state_*()` is four bytes and writing it is not free.** It is where FR-5.6's slideshow
  index and the catalogue epoch cursor live (`app_slideshow.c`). On the M5Paper Color it is RX8130
  RTC RAM, chosen over NVS to avoid ~288 flash writes a day (ticket `37`); on the E1002 the PCF8563
  has no RAM at all, so it is NVS with one key per byte and a read-before-write, and ticket `39`'s
  per-key finding is what makes that defensible. Four is a ceiling, not a preference -- see
  `smb_catalog.h` for what already did not fit in it.
- **`board_card_present()` means different things on the two boards.** The M5Paper Color reads a
  real detect line (PM1 GPIO1, armed by GPIO4, low = present, confirmed against a fitted card
  2026-09-04). The E1002 has no detect line and returns `true` unconditionally -- read the comment
  on it in `board_e1002.c` before reading anything into the behaviour: a constant `false` would
  mean the frame never used a fitted card and would re-run the three-second frequency ladder
  forever, and a constant `true` costs only that a card inserted into a running frame is unnoticed.
- **The card's chip select belongs to `board_storage.c`, not to `sdspi_host`** (since 2026-09-22).
  The slot is attached with `SDSPI_SLOT_NO_CS` and `sd_do_transaction()` holds CS low around each
  whole transaction, under `board_spi_lock()`, because a 32 GB card misreads a command that starts
  on the first clock after CS falls and IDF sends exactly that. Handing CS back to the driver
  brings back `ESP_ERR_INVALID_CRC` on that card at every frequency; ticket `08` has the probe.
- **The panel interface is one function wide in practice.** `app_display.c` -- the entire render
  path -- calls `epd_refresh_frame()` and nothing else from the driver. Everything else it uses is
  computation in `src/core/`. So a third panel is a driver plus a pin header, not an audit.

**Six symbols were renamed by the layering pass, and older tickets and plans still use the old
names.** Those files are dated records of what was true when they were written and were left alone
deliberately; this is the table to read them through.

| Was | Is | Where |
| --- | --- | --- |
| `board_pm1_power_read()` | `board_power_read()` | `board.h` |
| `board_pm1_epd_power()` / `_state()` | `board_epd_power()` / `_state()` | `board.h` |
| `board_rtc_ram_write/read()` | `board_state_write/read()` | `board.h` |
| `RX8130_RAM_SIZE` | `BOARD_STATE_BYTES` | the board's pin header |
| `board_storage_card_power/present()` | `board_card_power/present()` | `board.h` |
| `epd_regs.h` | `panel/epd_cmds.h` + `panel/epd_geom.h` | the second is the pure one `core/` may include |
| `EPD_COLOR_ORANGE_7IN3_ONLY` | `EPD_COLOR_ORANGE_UNUSED` | it is unused on **both** panels |

**What is deliberately still per-board: the aspect ratio** — though far less of it than when this was
written. 400x600 is portrait 2:3 and 800x480 is landscape 5:3, which flips `epd_fit_centre()`'s
matte-versus-crop decision for the common case and invalidates every row of
`docs/agents/measurements.md`'s geometry table on the larger panel. **Matte-versus-crop was decided
2026-09-17 (matte, both boards) and the replacement geometry table was derived 2026-09-19** in ticket
`64`'s decision section, for both panels and under the settings that ship. What is left of ticket `62`
is folding that table in. The layering made the assumption visible; the decisions resolved it.

## Internal RAM is the scarce resource, not PSRAM

PSRAM holds **~6.36 MB** free in the shipping application, measured 2026-09-08 (the 7.5 MB this
file used to claim was never measured here); internal RAM is what task stacks, Wi-Fi, lwIP and
RMT come out of, and adding two small tasks (LED, buttons) was enough to make `httpd_start()`
return `ESP_ERR_HTTPD_TASK` — the frame booting with a panel, a slideshow and **no web UI**,
on one easily-missed log line. Every task stack is sized from a measured
`uxTaskGetSystemState()` high-water mark rather than a guess; `env:frame` prints the table
and the memory line every 10 s. Before adding a task or raising a stack, read
`.scratch/digital-frame/issues/21`, which lists four configuration knobs that were tried and
made things worse — two of them broke Wi-Fi association or throughput outright.

**And `app_settings_t` is a stack object on the main task, so do not grow it.** `frame_main.c` holds
one copy for app_main's whole life, `app_settings_dump()` takes one at boot, and `app_smb_sync.c`'s
`config_load()` takes another on the same task every 10 s; httpd's handlers take eight more. Adding
a 256-byte field — ticket `60`'s album link — took `stacks main=` from **648 to 224 bytes** at the
first heartbeat on 2026-09-13, and moving it out of the struct put it back at 648. Anything large
goes beside `s_settings` in `app_settings.c`, behind a getter. **Small scalars are fine and the rule
is not "never add a field"** — ticket `68` put `standby_white` and `maint_day` in, two bytes, and
`stacks main=` was 688 at the first heartbeat afterwards (2026-09-20). The figure to watch is that one;
256 bytes is what moved it and 2 bytes is not.

**Ticket `68`'s maintenance course has no task either, and that was the same decision.**
`src/app/app_maint.c` is ticked from `frame_main.c`'s 200 ms loop beside `app_slideshow_update()`, the
way ticket `55`'s LED poll is. A course spends its whole length waiting — ten refreshes and nine gaps —
so a task would have bought nothing but a stack. It has no mutex for a related reason: every field is
written only by the application task, and the two public verbs set `volatile` want-flags the tick
consumes, which is the shape `service_buttons()` already uses for the button callbacks.

**And the number that matters is `dma_largest`, not `int_free`.** lwIP's pbufs and `sdmmc`
draw from the DMA-capable pool, and it can run dry while a total free size says there is
room; below a largest block of about 2 KB, lwIP silently drops arriving frames and the frame
stops answering ICMP and TCP with a perfectly healthy console. The heartbeat prints
`int_largest`, `dma_free` and `dma_largest` for this reason. Shipping idle is `int_free`
~72 KB and `dma_largest` ~31 KB. The account is in `docs/agents/defect-log.md`.

**Those two figures are the HEARTBEAT's, and `GET /api/system/info`'s `diag` block is not comparable
with them.** The route reports the same fields, which makes it the convenient way to watch a long run
without a serial capture — a capture reboots the board when it closes (`docs/agents/hardware-runs.md`)
— **but it samples them from inside a request**, so the request's own buffers, the JSON being built
and the worker's stack are all in the reading. Measured 2026-09-21 on both boards under a checkpoint
that had just made eighteen requests: `int_free` **62.7 KB** (M5Paper Color) and **64.6 KB** (E1002)
against the ~72 KB above, with `dma_largest` 31,744 and 30,720 — i.e. **8-10 KB lower, and none of it
a change in the firmware.** Do not read a `diag` number as a drift against a heartbeat number; compare
`diag` with `diag` and heartbeat with heartbeat, and say which instrument a figure came from.

**`int_min` on that line is a since-boot watermark with no moment attached, and two tickets went
unattributed because of it** (`47`'s 2,899 B floor, `52`'s 15,487 B). Since 2026-09-11 the heartbeat
carries two more lines, from `src/app/app_heapwatch.c` — read `heapwatch wm=` and not `int_low=`:

| field | what it is |
| --- | --- |
| `heapwatch wm=<B> at=<ms> flags=<s\|d\|h\|g\|-> http=<ms>` | **the one to read.** The watermark's own fall: `wm` is exact and equals `int_min` on the line above (a difference is a fault in the instrument), `at` locates it to within 20 ms, `flags` is what was running — `s` a sync window including its prologue, `d` a panel refresh, `h` an HTTP request within a second, `g` an outbound TLS session (the Google Photos mirror's album read or photograph fetch, `app_gphotos_sync.c`, and the bench probe `app_gphotos.c`), `-` **none of them** |
| `heapwatch falls=<n>: <B>@<ms>/<flags> …` | the last eight falls, which because the watermark is monotone are the deepest eight. This is the descent to read against the log lines around each timestamp |
| `heapwatch int_low=… dma_low=…` | what a 50 Hz sampler of the *instantaneous* free size saw. **It undershoots the depth by 12-20 KB** and is kept only to show that it does. **Each is a since-boot record, not a per-heartbeat window**: it reprints unchanged every 10 s until a deeper sample displaces it (`heapwatch_note()`), so counting lines counts reprints — a reader that did so on 2026-09-13 reported one `dma_low=0` event as sixteen |

Three things about it that cost a build each to learn, all in ticket `47`:

- **Sampling the instantaneous free size does not work.** These troughs are shorter than a 20 ms
  tick; the watermark is monotone and that is the property that makes a poll sound.
- **The sampler has no task of its own.** It rides `board_led.c`'s 20 ms tick through
  `board_led_set_tick_hook()`, so it costs 512 bytes of that task's stack — an instrument for
  internal-RAM scarcity that took 2 KB of internal RAM would be measuring itself. The cost is
  fidelity: priority 3 is below the display task and `smbsync`.
- **`flags` has been wrong twice, in opposite directions.** A window's prologue (`app_server_stop()`,
  the `smbsync` task's own 8 KB stack) ran before `syncing` was published, so its lows read `-`; and
  `httpd_register_uri_handler()` calls `uri_match_fn`, so registering 25 routes at every window
  restart read as 25 requests and stamped `h` on a boot where nothing had ever connected. Both
  fixed; both looked like findings first.

**Where internal RAM actually goes, measured 2026-09-11 across five boots:**

| what | `int_min` | `dma_largest` at that instant |
| --- | --- | --- |
| **an on-demand SMB fetch, idle frame** (8 windows, 39 min; the floor is found by the second and then holds) | **5,419 B** | **5,888 B** |
| a 12 MP decode attempt, which then fails (ticket `56`) | 10,627 B | 12,800 B |
| the boot catalogue listing, every boot, over 200-250 ms | 13.3-18.5 KB | 19-20 KB |
| a 6.52 MB upload | unmoved | unmoved |
| a render, a 6.52 MB `GET` and a sync window overlapping inside 21 s | unmoved | unmoved |
| **an upload's thumbnail sidecar, 220 KB source** (2026-09-12, 5 boots) | **1,995-2,355 B in four of five; 7,647 B in the fifth**, median 2,339 | not read at the trough; 31,744 B on every heartbeat |
| an upload's thumbnail sidecar, 4.29 MB / 12 MP source (4 boots) | 6,143-10,555 B, median 8,771 B | as above |
| an upload's thumbnail sidecar, 931 KB source, with a bench TLS fetch every 20 s (2026-09-13, 1 boot; no fetch in flight at either fall) | 3,779 B and 3,935 B | 12,288 B at the nearest fetch's end, 461 ms before |
| **one outbound TLS session, mbedTLS defaults** — bench only, `-DFRAME_GPHOTOS_PROBE`, idle and under 931 KB uploads (ticket `60` §3b) | **23 B** watermark; `int_free` 5,315-8,763 B at handshake | **544-6,400 B at handshake; ten consecutive connect failures** |
| one outbound TLS session, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` — bench only, same load (§3c) | not isolable once the watermark is lower; `int_free` 34,767-47,267 B at handshake | 11,776-24,576 B at handshake; 32/32 fetches |
| a sound on the M5Paper Color (2026-09-22, ticket `16`), i.e. the I2S0 channel open for ~0.2-0.4 s with 2 × 240-frame DMA. `int_free` read before opening and with the channel open, 20 sounds (`audio-mem-20260922-223120.log`) | **2,148 B at the median, range 1,592-2,180**. Back to the starting figure after every teardown | moved in 1 of 20 (30,720 → 29,696); the buffers come from blocks other than the largest |

**The two sidecar rows are the deepest figures in this project and they invert the obvious rule: the
small source is the deeper one, by 3.75x at the medians.** The fall is inside `img_resize_to_edge()` —
header parse, whole decode, box reduction — 1.09 s after the upload's own log line, at the same
moment in all three repeats, so the depth varies and the timing does not. `img_resize.c` allocates
only `MALLOC_CAP_SPIRAM`, so the consumer is inside the prebuilt `esp_new_jpeg`, which cannot be read
or wrapped. **A 12 MP source is not the worst case for internal RAM.**

Three things to carry rather than re-derive (`.scratch/digital-frame/sidecar_wm_prereg.md` has the
arm):

- **`flags=` cannot see it.** All nine sidecar falls read `flags=-`, because the sidecar runs after
  `send_json()`: the work belongs to a request that has already been answered. That is a **third**
  way this flag misattributes, after the two ticket `47` fixed, and it is a gap rather than a wrong
  value.
- **The "a 6.52 MB upload: unmoved" row above is true for a reason nobody wrote down**: 6.52 MB is
  over `EPD_IMAGE_MAX_FILE_BYTES`, so `thumb_produce()` refuses it and **no sidecar is produced at
  all**. The one upload size whose internal RAM had been measured is exactly the one that skips the
  expensive step.
- **It is not ticket `47`'s unattributed 2,899 B.** That boot was 2026-09-09, before ticket `48`
  shipped the upload sidecar; no upload then decoded anything. The two share the code path — a
  `/thumb/` miss runs the same function — so the resemblance is a lead and not an attribution.

The listing's spread is which folder the round-robin reached (156 or 243 files), which is why **two
runs' `int_min` cannot be compared unless both are known to have listed the same folder** — that
mistake is what ticket `52` recorded as an unattributed regression.

**The softAP costs ~5.8 KB of internal RAM, and since ticket `67` it is down most of the time.** Idle
`int_free` on the shipping build, three readings each side, two boards: the M5Paper Color went
57,963 / 58,471 / 58,675 → **64,199** and the reTerminal E1002 60,575 / 60,635 / 60,679 → **66,483**.
+5.7 KB and +5.9 KB, consistent, well clear of either side's spread. Beacon buffers, a second netif's
traffic path and the DHCP server's state, returned when the mode drops to `WIFI_MODE_STA`. **So the
figures above and below this line were all taken with an access point up**, and a future one taken
between windows is not comparable with them — which is the same trap as ticket `52`'s folder sizes.
`ap=` on the heartbeat says which state a run was in.

**The connect card (ticket `66`) costs nothing worth a row**, and its ~480 bytes went into `.bss`
rather than onto a stack deliberately. `app_display.c`'s request struct carries eight strings where it
used to carry two, and `display_task()` copies it into a **file-static** in-flight buffer instead of a
local — the display task's stack is 8192 and was sized with 1.5 KB of measured spare over a PNG decode,
so a 480-byte local was the wrong side of that margin. Safe because the display task is the only reader
and its requests are serialised by construction, which is the same argument `s_qr_box` already rests on.
Measured on hardware 2026-09-19 over four boots and both boards: **a card render produces no watermark
fall at all.** The deepest point of the portrait boot was the 31-route sweep (`int_min` 41,987 /
`dma_largest` 20,480, `flags=h`) and of the landscape boot a **PNG photograph** render at
`wm=43159 at=9967 flags=d` — attributed by the timestamps, because the card's own render on that boot ran
fifteen seconds later and left the watermark untouched. **`flags=d` says "a panel refresh" and cannot say
which one**, so a card drawn near a photograph will look like it cost what the photograph cost.

**Superseded 2026-09-12 as the deepest figure, not as the point.** The sidecar rows are lower, and the
sentence below is still the one to read for what the instrument bought — a routine operation, nobody
doing anything, and a margin of three rather than thirty. A
routine fetch, with nobody doing anything, takes the DMA pool's largest block to 5,888 B — within a
factor of three of the ~2 KB at which lwIP starts dropping frames silently. Every heartbeat
*sampled* on either side of it read 23,552. Nothing here says a frame was ever dropped; what it says
is that the margin is three times, not thirty, and that it has presumably been so since ticket `37`
Phase 3 shipped.

## Storage and the panel

**The storage lock takes the SPI bus only for the microSD.** `board_storage_lock()` used to
take `board_spi_lock()` unconditionally, which is right for a card on SPI2 and pointless for
the internal FAT volume on the main flash controller — and it made every filesystem route
wait out a 15.6 s refresh, so `GET /api/photos/list` during a refresh timed out at 10 s. It
now decides from the active medium and remembers the decision for `unlock`;
`board_storage_init()` takes the bus when either end of a media switch is SD. Ticket `03`'s
"a guarded read waits 14.1 s" is therefore the **SD** number — measured directly at 13.61 s,
where the **unguarded** read waits exactly as long, because `spi_device_acquire_bus()`
serialises a card transaction that never passes through `board_storage_lock()`.

**A file on `/data` that gets parsed back is written with `board_storage_write_atomic()`, never with
`fopen(path, "wb")`** (ticket `78`). It writes `<path>.part` and renames, so the real name only ever
holds the old file, nothing, or the new file whole — `fopen("wb")` truncates at open, so writing at
the real name leaves a prefix there for as long as the write takes, and the frame's own record files
have a reading for a prefix. The one that matters: **a `.smbidx` cut exactly on a record boundary
parses as an index of a smaller cache and nothing anywhere reports it.** It takes the storage lock
itself for the whole open-write-rename sequence, so **a caller must not already hold it** — the lock
is not recursive and `board_storage_lock()` asserts on re-entry by the same task. All four writers
that had their own copy of this sequence, or lacked one, call it now (`docs/agents/smb-mirror.md`).

**One file is written with `fopen("wb")` on purpose, and the lock is the reason** (ticket `82`):
`/data/.imgseq`, the upload sequence's high-water mark, is written inside
`generate_next_photo_name()`, which is called with the storage lock already held — so the helper
would assert. It is two small numbers whose torn reading is `sscanf` failing, and the fallback is
the highest name present on the card, i.e. the behaviour of before the file existed. **That is the
test for any future exception**: not "is it small", but "does a prefix of it parse as a valid,
smaller truth". A `.smbidx` does; this does not.

**The `storage` partition still holds the factory firmware's own photos.** Flashing an app at
`0x10000` never touches `0xA00000`, so `imaged001.png`–`imaged004.png` survived every erase
and reflash and mount without a format. Anything that formats `/data` destroys them — the
copies in `.scratch/digital-frame/fixtures/` are the backup. **They are not visible at
`/data` while a card is mounted**, which is FR-4.1 working: the card wins, so a slideshow
listing one image is the card's contents rather than a broken scan. To see them again, the
card has to come out.

**The panel answers register reads on MOSI, not MISO.** `SI0` (J5 pin 33) is the EL040EF1's
only data pin on this board, and GPIO14 belongs to the microSD, so a read issued on MISO
returns whatever that line floats to — it floated to 0x00 before the SD chip select was
parked high and to 0xFF after, and those decode to 0 °C and −1 °C, two numbers that look like
measurements and are not. `epd_read_temperature()` now does its whole transaction, TSE write
included, on a second half-duplex `SPI_DEVICE_3WIRE` handle at 1 MHz, and ends in
`epd_reset()` inside the bus lock because the panel does not release SI0 when its CS goes
high. Two things follow. `spi_device_acquire_bus()` locks the bus to **one** device handle,
so a command sent on the 4 MHz write handle cannot be followed by a read on the 1 MHz read
handle inside that window — mixing them hung the board on the first call. And `epd_regs.h`'s
"tested and confirmed NOT working" list (PBC, REV, CRC, ROTP) came from upstream and has
never been retried on a working read path; treat it as untested rather than as a result.
Ticket `18` has the raw bytes.

**The first temperature read after boot returns 0 °C; later ones are real** (ticket `18`).
`env:bringup` reads 0 at startup and then 31-33 °C across six refreshes, and in runs where an
sdspi attach happened before the first read it came back at 32 °C instead. Five runs, exact
correlation: the read starts working once something else has driven the bus. No harness
measurement is affected — every `temp_c` in a harness log is taken alongside a refresh — but
anything wanting a temperature before its first refresh gets 0.

**`EPD_SEQ_BUSY` is the default**, chosen against scans; FRS stays a parameter and `0x08`
stays the setting. The numbers, the alternatives and why the fast settings are not adopted
are in `docs/agents/measurements.md`.

## Charging is NOT in the board interface, and adding it is a one-board affair

`board.h`'s whole power surface is **read-only** — `board_power_read()` returning `vin_present`,
`vinout_present`, `bat_present` and `vbat_mv` — plus the two rails, `board_epd_power()` and
`board_card_power()`. There is no charge-enable, no current limit and no termination voltage, and
`/api/battery` is a `GET`.

**Before adding one, know that it can only ever be implemented on one of the two boards.** The
M5Paper Color's charge-enable pin is not connected, its charger's I²C is not connected, its charge
current is set by a resistor, and its system rail **is** the battery node through a 0 Ω link — so
"stop charging" there would mean "start discharging" even if the first three were solved. The E1002's
SY6974B is a real I²C charger with a real power path. **`docs/board-pinmap.md` §"Charging, and why
only one board can control it" is the table**, and ticket `71` is the account with the schematic
readings.

So a charge feature is a **board-conditional** one whose M5Paper Color arm is permanently "not
supported". `board.h:64-66` already has the convention for that — a field a board cannot answer reads
false or zero, and **a caller must not read a false field as a measurement** — so the shape exists;
whether the second code path earns its keep is a product decision, not a layering one.

**Both of those unknowns are now answered on hardware, and the charge cap is built** (ticket `71`
§10, 2026-09-20). `board_charger_read/dump/set_vreg_mv/watchdog_disable()` are in `board.h`, all four
answering `ESP_ERR_NOT_SUPPORTED` on the M5Paper Color; `app_charge.c` is the policy and
`charge_limit_pct` (0/80/100, default 80) the setting.

**The one finding to carry into any future charger work: the part's I²C watchdog restores register
defaults, and it made the first version of the cap decorative** — `REG04` reverted and the cell
charged back up between 20 s re-asserts. Disabling the watchdog is what makes a write hold. So
**write, read back, and keep counting** — `app_charge`'s `corrections` is served on `/api/battery`
precisely so the next unit or revision that behaves differently is visible rather than silent. And
`REG08`'s field positions are corroborated rather than proven: `[7:5]`, `[4:3]` and `[2]` all decode
correctly at once, which rules the ESPHome component's `[5:4]` out structurally, but a sample taken
while actively charging would settle `[4:3]` against `[1:0]` outright.

## Working with the hardware

**`docs/agents/hardware-runs.md` is the procedure — read it before flashing anything.**
It carries the pre-flight supply check, the exact `tools/bringup_capture.py` invocations
per env, the stop conditions and the recovery ladder. `bringup_capture.py` is not a camera
helper; it is how firmware gets run here, and it already has the timeout, the non-zero
exit and the correct reset that a hand-rolled serial script will not.

**While the operator is remote, PM1 `SYS_CMD` is forbidden** — a power-off, and VBUS does not
boot this board — enforced by `-DBOARD_NO_POWER_OFF` on every device env.

**`tinyusb_driver_install()` is called in exactly one place: ticket `09`'s USB drive mode**
(`src/board/board_usb_msc.c`, M5Paper Color only, since 2026-09-22). It is a boot of its own,
entered by `POST /api/storage/usb` through a word in RTC no-init memory and left by a restart, and
`app_main()` branches into it right after `board_storage_select()` and before anything else starts
— so the host is the volume's only writer and `board_storage_prepare_access()` still reclaims
nothing. Two things to know before touching it: **the PHY selection (`RTC_CNTL_USB_CONF`) survives
`esp_restart()`**, so every boot hands it back to USB-Serial-JTAG first (`board_usb_msc_pending()`),
and a power cut clears the request, so the power button always returns a normal frame. Everywhere
else, use the MSC storage API alone.
`espressif/esp_tinyusb` is linked and `CONFIG_TINYUSB_MSC_ENABLED=y`, because its storage API
is also this project's mount layer (`src/board/board_storage.c`). Creating MSC storage is safe —
verified against the component's source, it only allocates class bookkeeping and calls
`esp_vfs_fat_register`/`f_mount`. *Installing the device driver* switches the USB-C port from
USB-Serial-JTAG to USB-OTG, one PHY on GPIO19/20, and takes the console and the esptool
auto-reset path on COM11 with it. That is why the shipping firmware puts its console on the
Grove UART — and `src/app/trace.h` already uses those two pins. Ticket `09` owns the trade.

**Press the power button before expecting firmware to run.** The panel keeps its last
image with no power, `esptool` connects to and flashes a board that is otherwise off, and
the console stays completely silent — not even the ROM banner. So every signal except one
says the device is alive when it is not. The one that tells the truth is the panel failing
to change. Once powered on, an RTS reset restarts it normally. This cost an hour;
`.scratch/digital-frame/issues/17` has the account.

**`bringup_capture.py --no-reset` still reboots the board when the capture ends.** The flag
suppresses the reset on *open*; the DTR/RTS state pyserial leaves on *close* resets this chip
through USB-Serial-JTAG, and the next attach shows `# reset reason: 11` = `ESP_RST_USB`. It
reproduces on demand. A capture killed by `SIGPIPE` — `... | head -n` does it — leaves no
clean close and does **not** reset, which is what made it look intermittent. So a reboot that
lands next to a capture ending is the instrument; look for a panic in the log first. And a
long network run cannot be watched by attaching and detaching, because each cycle restarts
what is being watched — take one capture across the whole run, or drive it over HTTP and
leave COM11 alone. A capture holding COM11 also makes `-t upload` fail; detach first.

Device is COM11 (AP is `PaperFrame-A1B2C3`; `PaperColor-` before the 2026-09-17 rename). The factory firmware
is dumped and verified at `~/M5PaperColor-factory-backup/` with restore instructions — the
photos recovered from its `/data` are test fixtures in `.scratch/digital-frame/fixtures/`.

**The pairing screen is TOP-long-press**, and the flatbed can verify its QR without a phone:
two codes — the `WIFI:` join URI and `http://192.168.4.1/?t=<token>` — drawn through
`epd_canvas_fill_rect()` at a scale computed from the box, because the module count follows
the text length. `python tools/qr_check.py --dpi 300 --expect-join '…' --expect-url-prefix
'…'` scans the panel's corner of the bed and decodes; at 300 dpi both come back exact. **A
flatbed is an easier reader than a camera** — even light, dead flat, no perspective — so a
pass is necessary and not sufficient while a failure is decisive, and that asymmetry is what
makes it a good filter before spending the operator's time. `-DFRAME_PAIRING_AT_BOOT` in
`platformio_local.ini` draws it once at boot so the scan loop needs no finger, and **should
not ship**. The screen carries **no text** — `epd_canvas` has no font — so since 2026-09-10 the
two codes are told apart by a **count**: one filled square beside the code that joins the network,
two beside the one that opens the page. The marker strip comes out of the **longer** side of each
code's box, because the shorter side is what sets the module scale, so the codes cost nothing for
it (`qr modules`/`scale` identical before and after, origin moved 24 px).

**It clears itself after three minutes, and the clear is TWO refreshes because one is not a
clear.** `clear_pairing_if_due()` renders white and then the photograph, ~31 s: drawing the
photograph straight over the codes leaves them **plainly legible as a ghost on top of it**, and
the console cannot see that — `renders` +1, `failures` 0, a normal `panel_ms`, and a token still
readable off the glass (measured on the flatbed 2026-09-10, ticket `30`). `FRAME_PAIRING_CLEAR_MS`
shortens the three minutes for a bench run, and it has to exist: `bringup_capture.py` stops at
`@@DONE` at **t=120 s** and closing the port resets the board, so anything scheduled later than
two minutes has its clock restarted on every attach and is never observed.

**A `cv2`-based tool will not run on this machine as it stands** — `tools/qr_check.py` dies with
`ModuleNotFoundError: No module named 'cv2'` under the PlatformIO interpreter, and neither the
system nor the miniconda Python has it either. What works with no dependency at all is
`tools/scan_panel.ps1 -Dpi 150` plus `tools/bmp_to_png.ps1`, and then **reading the PNG**: it
shows the panel, the codes and any ghost directly, which is how the finding above was made.
