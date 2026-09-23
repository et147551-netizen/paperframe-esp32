# tools/

Scripts that sit beside the firmware. Each one's own docstring is the manual; this is the index.

## Build

| Script | Role |
| --- | --- |
| `sign_firmware.py` | PlatformIO post-script for `frame` and `frame_e1002`: signs the image for OTA. Fails without the key |
| `native_toolchain.py` | PlatformIO pre-script for `native` and `native_e1002`: puts clang on PATH on Windows; does nothing elsewhere |
| `gen_assets.py` | Turns `assets/` into `src/app/assets_data.c`. **Not run by the build** — run it after editing `assets/` |
| `gen_cities.py` | Turns Natural Earth's populated places into `src/core/geo_city_table.c`. Not run by the build either |
| `gphotos_rules_sign.py` | Signs the Google Photos scrape-rule file the frame accepts; the private key lives outside the repository |

## Device

| Script | Role |
| --- | --- |
| `find_port.py` | Prints the serial port of a board by USB VID:PID (`m5papercolor` or `e1002`) |
| `bringup_capture.py` | Watches the serial console and captures the panel (camera or flatbed) when the firmware marks a frame as settled |

## Checks against a running frame

| Script | Role |
| --- | --- |
| `api_path_sweep.py` | Asks every route that turns request data into a path under `/data` for a traversal, and checks each answer |
| `settings_roundtrip.py` | Writes a distinct value into every setting, reboots, and reads them all back |
| `http_stall.py` | Drives a multipart upload that stalls or trickles, and times what the frame does |
| `iperf_peer.py` | The PC-side peer for `env:iperf` |
| `qr_check.py` | Scans the panel on the flatbed and decodes the pairing screen's QR codes |

## Host-side parity with the firmware

| Script | Role |
| --- | --- |
| `render_preview.c`, `render_preview.py` | Runs the real quantiser on the host: what a photograph will look like on the panel |
| `epdopt_reference.mjs` | Generates expected bytes for the epdoptimize port by running epdoptimize itself (needs `refs/epdoptimize` built) |
| `ink_preview_parity.py`, `ink_preview_quantise.js` | Checks the web page's ink preview against `src/core/epd_dither.c` |
| `battery_parity.py` | Checks `src/core/battery.c` against the web page's battery curve |

## Bench instruments

| Script | Role |
| --- | --- |
| `capture_panel.py` | Photographs the panel with a fixed USB webcam |
| `scan_panel.ps1`, `bmp_to_png.ps1` | Scans the panel on a WIA flatbed without a GUI, and converts the BMP |
| `chart_patches.py` | Mean RGB of each colour-chart patch across a series of scans |
| `colour_check.py` | Scores colorimeter readings against the panel module's optical spec |
| `colour_timeseries.py` | Colour drift over time from webcam captures |
| `analyse.py` | Medians, spread and verdicts for the measurement harness's CSV |
| `make_smb_fixture.py` | Builds the listing-truncation fixtures on the bench SMB share |
