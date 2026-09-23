# Building this project

Moved out of the project index verbatim on 2026-09-09 so it is read when it bears on
the task rather than loaded every session. Nothing here was rewritten.

## Building

**PlatformIO with `framework = espidf`**, not native ESP-IDF and not Arduino. `pio` lives at
`~/.platformio/penv/Scripts/pio.exe`, not on `PATH`. Ticket `01` carries the full
configuration; ESP-IDF 5.5.0, green since 2026-08-29.

```bash
~/.platformio/penv/Scripts/pio.exe run  -e frame             # THE APPLICATION: wifi, web UI, slideshow
~/.platformio/penv/Scripts/pio.exe run  -e m5papercolor      # harness build
~/.platformio/penv/Scripts/pio.exe run  -e storage           # /data mount, scan, concurrency
~/.platformio/penv/Scripts/pio.exe run  -e colourchart       # colour chart, 6 h camera run
~/.platformio/penv/Scripts/pio.exe run  -e colourchart_smoke # the same, 2 min, run it first
~/.platformio/penv/Scripts/pio.exe run  -e smbprobe          # SMB session cost in internal RAM
~/.platformio/penv/Scripts/pio.exe run  -e iperf             # TCP ceiling, station-only (ticket 27)
~/.platformio/penv/Scripts/pio.exe run  -e gphotosprobe      # one TLS session's cost (ticket 60)
~/.platformio/penv/Scripts/pio.exe run  -e tempprobe         # which pin carries the reply (ticket 18)
~/.platformio/penv/Scripts/pio.exe run  -e frame_e1002       # the application on a reTerminal E1002
~/.platformio/penv/Scripts/pio.exe test -e native            # host-side unit tests, 400x600
~/.platformio/penv/Scripts/pio.exe test -e native_e1002      # the same at 800x480
~/.platformio/penv/Scripts/pio.exe run  -d observer -e capsule  # M5Capsule observer
```

**Twelve device envs and two host ones.** When a change touches anything shared, build all of them
and name them rather than sampling — "all envs built" has been claimed here on a subset before:
`m5papercolor bringup photo storage colourchart colourchart_smoke frame tempprobe smbprobe iperf
gphotosprobe frame_e1002`.

`observer/` is a **separate project**: the M5Capsule is an ESP32-S3FN8 with no PSRAM, so it
cannot share this project's `sdkconfig.defaults`. It acts as the two-channel timing capture
that checks the harness's own timestamps, replacing a logic analyser. Wiring is in
`.scratch/epd-refresh-optimization/issues/05` — **the Grove 5V wire must not be connected.**

### Two boards, one flag, and no default

Since the ticket `43` layering pass of 2026-09-15, **every env carries a board flag** —
`-DBOARD_M5PAPER_COLOR` or `-DBOARD_RETERMINAL_E1002` — and it selects three things at once:
which pin header `src/board/board.h` includes, which panel `src/panel/epd_panel.h` includes, and
which per-board `.c` files compile to something rather than to nothing.

**There is no default and that is the point.** `board.h`'s `#else` arm is an `#error`, so an env
that builds has selected a board. Given that `compile_commands.json` cannot see these flags at all
(below), a build failure is the only cheap way to know a flag arrived.

**`platformio_local.ini` REPLACES `build_flags`, it does not merge them.** That file is git-ignored
and carries same-named `[env:]` sections for `frame`, `smbprobe` and `gphotosprobe` — so adding the
board flag to `platformio.ini` reached eight envs and silently not those three. It cost one build to
find, and only because the `#error` fired on the first file compiled; **a default board would have
produced a green build for the wrong hardware.** Any local override must repeat every flag it
means to keep, including `-DBOARD_<board>` and `-DBOARD_NO_POWER_OFF`.

**Per-board `.c` files are guarded whole-file, for the reason `build_src_filter` cannot be used**
(next section). Each one opens `#ifdef BOARD_<board>` after its first include and closes at the
bottom. That includes the panel drivers: `epd_el040ef1.c` compiles to nothing on the E1002 and
`epd_ed2208gca.c` to nothing on the M5Paper Color.

**Header basenames must be unique across board and panel directories.** CMake cannot see the build
flag, so `src/CMakeLists.txt` puts *every* layer directory on `INCLUDE_DIRS` unconditionally. Two
files called `pins.h` would resolve by directory order rather than by the selected board — hence
`pins_m5papercolor.h`, `epd_geom_el040ef1.h` and so on.

**Three sdkconfig lines that look board-specific, and only one is.**
`CONFIG_ESPTOOLPY_FLASHSIZE` genuinely is: `sdkconfig.defaults` carries 16MB for the M5Paper Color,
so `sdkconfig.frame_e1002` has to be edited to 32MB by hand, by Edit and not `sed -i`, or the 32 MB
`partitions_e1002.csv` does not fit the declared flash. **`CONFIG_SPIRAM_CLK_IO=30` and
`CONFIG_SPIRAM_CS_IO=26` are not** — they are ESP-IDF's own ESP32-S3 defaults and are not even
user-settable (`components/esp_psram/esp32s3/Kconfig.spiram:41-47` declares both `int` with no
prompt), so they are no-ops on both boards. They were written up as the port's biggest hidden leak
before anybody read that Kconfig; one grep removed the work item. `CONFIG_SPIRAM_SPEED_40M` is a
real choice, came verbatim from M5's demo, and is deliberately left alone on both.

### The two frame envs sign their image (ticket 61, 2026-09-22)

`env:frame` and `env:frame_e1002` run `tools/sign_firmware.py` after the build, which signs
`firmware.bin` in place with `~/.m5paper-keys/firmware_signing_rsa3072.pem`. **The key must never
enter the repository, and a build without it fails on purpose.** An unsigned image boots normally,
but it can accept no OTA: `esp_ota_end()` trusts only the key carried in the running image's own
signature block. Losing the key means every board needs one more USB flash, with an image signed by
a new key. Back it up.

`espsecure.py` runs in PlatformIO's ESP-IDF venv, and **that venv lacks six of its dependencies**. A
fresh or rebuilt venv needs this once:

```bash
~/.platformio/penv/.espidf-5.5.0/Scripts/python.exe -m pip install ecdsa intelhex pyserial reedsolo bitstring pyyaml
```

The two frame envs' generated sdkconfigs carry `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` (RSA
scheme, **no eFuse**) and `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Both were edited in place, and
neither is in `sdkconfig.defaults`, so the other envs build unsigned and without rollback. **Both
partition tables have two OTA slots and no `factory`.** Every env therefore puts its app in `ota_0`
and flashes a blank `otadata`, which PlatformIO does by itself.

### What this build system does that nothing warns you about

**`build_src_filter` does nothing under `framework = espidf`.** Sources come from
`src/CMakeLists.txt`, which globs `src/*.c` and never sees PlatformIO's filter, so several
entry points collide with "multiple definition of `app_main`". Each entry point is instead
guarded by a build flag — `-DBUILD_HARNESS` (`src/entry/harness_main.c`, `env:m5papercolor`),
`-DBUILD_BRINGUP` (`src/entry/bringup_main.c`, `env:bringup`), `-DBUILD_PHOTO`
(`src/entry/photo_main.c`, `env:photo`), `-DBUILD_STORAGE` (`src/entry/storage_main.c`,
`env:storage`), `-DBUILD_COLOURCHART` (`src/entry/colour_chart_main.c`, `env:colourchart`, and
`env:colourchart_smoke` which adds `-DCOLOURCHART_SMOKE`), `-DBUILD_FRAME`
(`src/entry/frame_main.c`, `env:frame`), `-DBUILD_SMBPROBE` (`src/entry/smbprobe_main.c`,
`env:smbprobe`), `-DBUILD_IPERF` (`src/entry/iperf_main.c`, `env:iperf`). It *does* work for
`env:native`, which is not an espidf environment.

**`compile_commands.json` does not carry PlatformIO's `build_flags`.** The generated
`.pio/build/<env>/compile_commands.json` lists nine `-D` defines for `src/board/m5papercolor/board_pm1.c` and
none of them is `-DBUILD_STORAGE` or `-DBOARD_NO_POWER_OFF`, even though both are demonstrably
in effect. Do not use it to check whether a flag reached a translation unit — it will say no
when the answer is yes. The decisive test is a temporary `#ifndef FLAG / #error / #endif`
in the file itself, and it is worth also inverting it to `#ifdef` once, to confirm the
`#error` actually fires rather than trusting a test that has only ever passed.

**And a probe that refuses to run also refuses to be verified.** 2026-09-12, `env:gphotosprobe`:
`nm` on the ELF found **zero** `esp_http_client` symbols, no `mbedtls_ssl_handshake` and no
certificate bundle, which reads exactly like a build flag that never arrived. The build was right
and the check was shallow. The probe guards itself with `if (!GPHOTOSPROBE_URL[0]) { ...; return; }`
and the macro defaults to `""`, so **`!""[0]` is a compile-time constant true**, the refuse-to-run
return is unconditional, and GCC eliminated every line after it — the tasks, the fetch, the whole
TLS reference graph. The object still defined `app_main` plus three statics, so the link succeeded
and nothing looked wrong.

So when a probe env's required flag is unset, **its ELF cannot answer "did the flag reach the
file"** — the two failures are indistinguishable. Give it a throwaway value, verify, and take it
back out: re-built with `-DGPHOTOSPROBE_URL='"https://example.com/linkcheck"'` every symbol appeared
and static RAM went **15,076 -> 39,508 B** with flash **229,165 -> 960,681 B**, which is the same
answer from two directions. Diff the file against a backup afterwards; the flag must not survive
into a measurement.

**And the same refusal corrupted the measurement taken from it.** That static-RAM delta was then
quoted for a day as "linking the TLS path costs ~24 KB of static internal RAM", and it is not: the
no-URL build had also lost `wifi_station_up()` to the same elimination, so its ELF had no Wi-Fi,
lwIP or PHY stack either — **22,432 B** of statics in the TLS-linked map, which is the delta. Measured
with Wi-Fi in both builds — shipping `env:frame` against `env:frame` plus the probe — the TLS client
is **+315 B**, and the TLS archives hold 674 B in total (2026-09-13,
`.scratch/digital-frame/gphotos_prereg.md`, knob arm step 0a). **A two-build delta attributes a cost
only when the builds differ in one thing, and dead-code elimination changes more than the line it
starts from.** Before quoting a delta as belonging to a component, sum that component's archives in
the link map — which is `.pio/build/<env>/paperframe.map`, named after the CMake project and so
**renamed from `M5PaperColor.map` on 2026-09-23**, **not** `firmware.map`, whose absence reads like
a build that produced no map. A map named in a capture older than that carries the old name.

**Moving or renaming the checkout costs a FULL rebuild of every env, and the incremental path does
not survive it.** After the rename of 2026-09-23 only `.pio/build/m5papercolor/` was left; `frame`
and `frame_e1002` rebuilt from nothing at **199 s and 210 s**, against the 11-50 s an incremental
build of the same two takes. Nothing had to be deleted by hand and no `sdkconfig` moved — CMake's
cached absolute paths simply were not there to be wrong. Both images signed, both boards were
flashed and answered (`.scratch/captures/rename-check-*.log`). Budget the two builds; do not read
the first one's silence for four minutes as a hang.

**A single-token `-Wl,--wrap=` DOES work, and it is how a vendored component's function gets
replaced without patching it.** `src/CMakeLists.txt` adds
`target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=AES128_ECB_encrypt")` and
`src/app/smb_aes_hw.c` supplies `__wrap_AES128_ECB_encrypt`; PlatformIO's SCons link carries it
through, `objdump` shows the call site redirected and `nm` shows the original symbol collected
out of the image entirely. It only intercepts **cross-translation-unit** calls, which is why
this worked for the AES block function (called from `smb2-signing.c`) and would not have worked
for `smb3_aes_cmac_128` (called from within the same file). And a green build is not the check —
verify the symbol in the ELF and have the wrapper announce itself at runtime, because this build
system has silently dropped CMake work before.

**No two-token compiler flag works.** PlatformIO's espidf builder **sorts** the flag list
before handing it to SCons (`platforms/espressif32/builder/frameworks/espidf.py:466-468`,
`sorted(...)`), which separates `-include` from `esp_random.h`, `-isystem` from its
directory, and so on. The pair survives all the way into `.pio/build/<env>/build.ninja`
looking perfectly correct and is still dead on arrival, so reading the ninja file will
*confirm* a flag that does not work. Use a single-token flag, or a different mechanism.
`src/CMakeLists.txt` carries the case and what was done instead.

**Partition table.** `board_build.partitions = partitions.csv` is **required**. The option
resolves against the *project* directory — which is why naming M5's `default_16MB.csv` fails,
since that file lives in the Arduino package — but a csv that really is in the project
resolves fine. Setting `CONFIG_PARTITION_TABLE_CUSTOM` in `sdkconfig.defaults` alone does
**not** work: PlatformIO generates its own `.pio/build/<env>/partitions.bin` from the board
manifest and flashes that at `0x8000`, ignoring the IDF-side option, so the device silently
keeps the esp32s3box default of 1 MB factory and no data partition while
`sdkconfig.m5papercolor` reports `CONFIG_PARTITION_TABLE_CUSTOM=y`. Both are set now.

**Two things need a deleted build directory to take effect.**
`sdkconfig.m5papercolor` does not track `sdkconfig.defaults` — delete the generated file to
pick edits up. And `src/idf_component.yml` is honoured on a *fresh configure* only: editing
it and running `pio run` again does nothing and still reports SUCCESS, so delete
`.pio/build/<env>/`.

**And deleting a generated `sdkconfig.<env>` silently drops settings that live only there.**
The generated files are *tracked in git*, so a value can be committed without ever reaching
`sdkconfig.defaults` — and then the instruction above destroys it. The live case is the TCP
receive window: `sdkconfig.smbprobe` carries `CONFIG_LWIP_TCP_WND_DEFAULT=16384` from commit
`8796ee9`, because ticket `23` found 4 KB SMB reads at the stock `5760` stall every ~150-200
PDUs and that `4 KB + 16384` was the only no-stall row in its table. It is not in
`sdkconfig.defaults`, so it also never propagated: `sdkconfig.frame` runs the shipping
application at `5760`, which is why the mirror needs its reconnect-and-resume recovery at all.
**Diff a generated `sdkconfig.<env>` against `sdkconfig.defaults` before deleting it**, and
before quoting a network figure from any env, read that env's own settings rather than
assuming them. **Better still, do not delete it: edit it in place**, which is what `menuconfig`
does and what the 2026-09-05 clock and cache change did across all ten files.

**And do not edit an sdkconfig with `sed -i` — it rewrites CRLF to LF on this machine.** Every
`sdkconfig.*` here is CRLF. `sed -i` silently dropped one byte per line from `sdkconfig.photo`,
79 818 bytes to 77 196, which turns a three-line change into a 2 622-line diff that hides the real
edit. Use the Edit tool for a line or two, or for a sweep a script opened with `newline=""` and
`encoding="utf-8"` on both ends that anchors each replacement to a whole line and refuses to write
a file where an expected line is not found exactly once. This is the same hazard class as the
Python `encoding=` one, in a different tool.

**The board manifest is fiction under `framework = espidf`, and it lies in both directions.**
`esp32s3box.json` declares `"f_cpu": "240000000L"`, `"f_flash": "80000000L"` and
`"flash_mode": "qio"`; before 2026-09-05 the device ran at **160 MHz** with **DIO**, because for
these the sdkconfig wins and the manifest is ignored — the exact opposite of the partition table,
where PlatformIO's own generation wins and `board_build.partitions` is required. Worse for flash
mode: PlatformIO **deliberately downgrades** it, `_get_board_flash_mode()` in
`platforms/espressif32/builder/main.py:88-100` returning `"dio"` whenever the mode is `qio` or
`qout`. So **byte 2 of `firmware.bin` is not the check for flash mode** and never becomes `0x00`;
the ROM uses the header to load the bootloader and the bootloader then applies the sdkconfig's
mode itself. The check is the app-level `spi_flash: flash io: qio`.

**`fw_version` on `GET /api/system/info` is the `git describe` from the last CMake CONFIGURE, not
from the last build — so it can name an older commit than the binary contains.**
`CONFIG_APP_PROJECT_VER_FROM_CONFIG` is not set, so ESP-IDF fills `PROJECT_VER` from git at configure
time; an incremental build that only recompiles sources does not re-run that step. Measured
2026-09-20: both boards were flashed from commit `6ad2793` and both answered
`"fw_version": "cbab660-dirty"`, the commit before it. **So do not use `fw_version` to confirm which
firmware is on a board.** Use something the binary itself demonstrates — that day it was
`server: http server on :80, 32 routes` on the console and the presence of the `rotation` field in
the config response, either of which is a fact about the running code rather than about a string. A
deleted build directory refreshes it, which is also why this has never been noticed before: every
`sdkconfig`/`idf_component.yml` change already forces one.

**The CPU runs at 240 MHz and the data cache line is 64 bytes, since 2026-09-05.** Neither was
ever *chosen*: both were absent from `sdkconfig.defaults` and arrived as ESP-IDF defaults, the same
category as the `-Og` that everything here is still built at. Measured one variable at a time, they
gave 1.5× on everything CPU-bound plus 9-27 % on anything touching PSRAM — the shipping row dither
went **1 660 → 1 084 ms** — at no cost in internal RAM, because a cache *line* is not a cache
*size*. `-O2` was measured too and **rejected**: net 4 % slower on the adjustment stages.
**Every CPU-bound figure in this repository taken before that date is stale**, including the
1 618 ms row path and the 24.0 s pipeline quoted in `docs/render-pipeline.md`; the
panel-side numbers are not, and
`docs/measurements.md` has the evidence for that distinction along with what each arm's
apparatus check was.

So **do not derive a figure by subtracting two of them.** 24.0 s (the 160 MHz era) minus 8.259 s
(the 240 MHz column of the cumulative-knobs table) gave 15.7 s for a stage whose measured cost —
11.2 s — was already recorded three lines from the first of those two numbers. Look for the delta
before computing it.

**The TCP window is a PAIR of settings, and raising one alone makes the board look broken.**
`CONFIG_LWIP_TCP_WND_DEFAULT` needs `CONFIG_LWIP_TCP_RECVMBOX_SIZE` raised with it — IDF's own
`components/lwip/Kconfig:690` gives the requirement as `WND/MSS + 2` and states that a full
mailbox makes "LWIP drop the packets". `sdkconfig.smbprobe` has both (`16384` and `16`);
the default mailbox is `6`, which is exactly right for `5760` and far short of the `13` that
`16384` needs. Measured on hardware 2026-09-05, ten 10 s samples per cell, RSSI −22 to −24
throughout:

| Condition | board sends | board receives |
| --- | --- | --- |
| `WND 5760 / mbox 6` — `env:frame` today | 891 KB/s | 893 KB/s |
| `WND 16384 / mbox 6` — window raised alone | 1821 KB/s | **118 KB/s** |
| `WND 16384 / mbox 16` — `env:smbprobe` | 1722 KB/s | 1438 KB/s |

The middle row is a misconfiguration, not a hardware property, and it is dangerous precisely
because it is directional and self-consistent: send doubles while receive collapses 7.6×, which
reads as a discovery. It also lands at 118 KB/s, the same figure as a row in ticket `27`'s own
stall table — a third-digit coincidence that made the false reading more convincing, not less.
**Raising the window also costs ~34.8 KB of peak internal RAM**, which `env:iperf` does not
notice at 265 KB free and `env:frame` would, idling at ~72 KB. Ticket `27` has the full
account; `env:iperf` runs at both valid conditions deliberately and prints the pair with the
requirement computed at boot.

**GCC intermittently dies with `internal compiler error: Segmentation fault` on IDF's own
`esp_lcd/rgb/esp_lcd_panel_rgb.c:681`. Run the build again.** It hits whichever environment
is compiling that file from scratch; every other environment has a cached object from an
earlier session. Established 2026-09-03 and worth not rediscovering: the compile command is
**byte-identical** between a failing environment and a passing one, so it is not
configuration, not `build_flags` and not the libsmb2 dependency; running that exact command
by hand crashed **once in seven** identical attempts; and SCons recompiles only the file that
died, so the second run costs seconds. `-O1`, `-Os` and `-O0` also avoid it, and lowering a
component's optimisation level mid-measurement would be a confound.

**Neither way of embedding a binary works.** CMake's `EMBED_FILES` makes PlatformIO's
SCons layer hunt for an assembly stub it never finds (``Source `.pio\build\photo\
test-chart.png.S' not found``). PlatformIO's own `board_build.embed_files` takes the
espidf branch of `_embed_files.py`, which *generates* the `.S` and then never adds it to
the build, so the link fails on undefined `_binary_*`. Assets go through
`tools/gen_assets.py` instead, which writes `src/app/assets_data.c` from `assets/`.

**And nothing runs it for you.** It is in neither `platformio.ini` nor `src/CMakeLists.txt`, so
editing `assets/index.html` and building reports SUCCESS and flashes the **previous** UI — the same
silent shape as the `UTILITY` target above, from a different cause. Four separate work plans each had
to rediscover this and write the reminder into their own steps. Run it after every asset edit, and
check `src/app/assets_data.c`'s mtime against `assets/`, not the build's exit code.

**And a third script that is a check rather than a generator.** `tools/battery_parity.py` extracts
`batteryPercentFromMV()` from `assets/index.html`, compiles `src/core/battery.c`, and compares them at
every millivolt — the same shape as `ink_preview_parity.py`, for the same reason: the page and the
panel must not disagree about the same battery. Run it after touching either side, and run
`--self-check` before believing it, because a parity test that cannot fail looks exactly like one that
passes. It earned its keep on its first run: the C was integer arithmetic and differed at 71 of 5001
millivolts, because the page rounds a floating-point error.

**There are TWO generators now and the second one ships empty on purpose.** `tools/gen_cities.py`
(ticket `64`) writes `src/core/geo_city_table.c` — the offline reverse-geocoding table — from a
Natural Earth CSV, and run with no argument it emits a table of **zero cities**, which is what is
committed. That is a deliberate state, not a stale artefact: the lookup returns false, the frame
draws no location, and the build is green. So the check is the same shape as above but the artefact
is different: read `CITY_COUNT` in the generated file. A build that compiles proves nothing about
whether the table has data in it.

**And the third way fails silently, which is worse than either.** `espressif/esp_mmap_assets`
2.0.1 was tried in `env:frame` on 2026-09-05 (ticket `31`): with its
`spiffs_create_partition_assets()` called correctly, the build **reports SUCCESS and packs
nothing**. No `assets.bin` is produced anywhere in `.pio/build/frame/`. CMake did create the
target — the file API reply gives `name: assets_assets_bin`, `type: UTILITY` — and
`build.ninja` mentions it six times; PlatformIO reads the CMake file API and rebuilds in
SCons, carrying across library and object targets but **not `UTILITY` ones**, so it never
runs. This is the same mechanism as the sorted-flags trap above, so the same warning applies
one level up: **a green build under this build system is not evidence that a CMake custom
target ran.** Check for the artifact, not the exit code. Any component whose work happens in
a `UTILITY` target — `cmake_utilities`' `target_add_binary_data` included — should be
expected to fail this way.

**Three licence classes among the dependencies, and two of them are one component each.**
The bulk is MIT-family. `sahlberg/libsmb2` (the read-only SMB client) is **LGPLv2.1**,
accepted deliberately — the alternative was hand-writing NTLMSSP and SMB2 negotiation, which
is a larger job than the frame itself. `espressif/esp_new_jpeg` (the thumbnail encoder, added
2026-09-07) is **"ESPRESSIF MIT"**: MIT with the grant restricted to "use on all ESPRESSIF
SYSTEMS products", which an ESP32-S3 is, so the grant applies. It is also the **only
prebuilt-binary dependency here** — `lib/esp32s3/` plus headers, no source — so neither
reading it nor a `--wrap` against it is available. Recorded here because there is no README
to put it in; `src/idf_component.yml` repeats both at the point of use.

**And `src/core/epd_colour.c` / `src/core/epd_epdopt.c` are a port of Apache-2.0 code**
(`paperlesspaper/epdoptimize`), which is compatible with MIT-family but carries an
attribution requirement that a dependency in `refs/` did not: this is source in this
repository derived from theirs, not a component fetched at build time. Both files say so at
the top, and `EPD_PALETTE_EPDOPT_*` in `epd_dither.c` says it for the palettes.
