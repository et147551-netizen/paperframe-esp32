# Third-party notices

paperframe is MIT-licensed (see `LICENSE`). The files and dependencies below carry their own
terms, which apply in place of or in addition to that licence.

## Files in this repository

| Files | Derived from | Licence |
| --- | --- | --- |
| `src/core/epd_dither.c`, and one row of `src/panel/el040ef1/epd_init_list_el040ef1.h` | [LovyanGFX](https://github.com/lovyan03/LovyanGFX)'s `Panel_ED2208` (via the M5GFX repository) | BSD-2-Clause, Copyright (c) 2020 lovyan03. Full text in `LICENSES/LovyanGFX-BSD-2-Clause.txt` |
| `src/panel/el040ef1/epd_init_list_el040ef1.h` | Waveshare's 4inch e-Paper (E) driver, `EPD_4in0e.cpp`, in [waveshareteam/e-Paper](https://github.com/waveshareteam/e-Paper) | MIT, Waveshare team. Full notice in `LICENSES/Waveshare-MIT.txt` |
| `src/core/epd_colour.c`, `src/core/epd_epdopt.c`, `src/core/epd_adjust.c`, `src/core/epd_auto.c`, `src/core/epd_classify.c`, `src/core/epd_diffuse.c` (diffusion kernels), the palettes in `src/core/epd_dither.h` and `src/core/epd_dither.c`, and the expected-output data in `test/*/*_fixtures.h` | [paperlesspaper/epdoptimize](https://github.com/paperlesspaper/epdoptimize) | Apache-2.0, Copyright the epdoptimize authors. Full text in `LICENSES/epdoptimize-Apache-2.0.txt` |
| The reTerminal E1002 panel driver, init list and pin map (`src/panel/ed2208gca/`, `src/panel/epd_cmds.h`, `src/board/reterminal_e1002/`, and the E1002 notes in `src/board/board_spi.h` and `src/board/board_buttons_hw.h`) | [esp32-photoframe](https://github.com/aitjcize/esp32-photoframe) | MIT, Copyright (c) 2025 Wei-Ning Huang (AZ). Full text in `LICENSES/esp32-photoframe-MIT.txt` |
| `src/app/dns_server.c`, `src/app/dns_server.h` | Espressif's ESP-IDF captive-portal example | Unlicense OR CC0-1.0 |
| Parts of the application layer (`src/app/`) and the web page `assets/index.html` | [m5stack/M5PaperColor-UserDemo](https://github.com/m5stack/M5PaperColor-UserDemo) | MIT, Copyright (c) 2026 M5Stack Technology CO LTD. Full text in `LICENSES/M5PaperColor-UserDemo-MIT.txt` |
| The eight icons in `assets/index.html` (nine uses; `eye` appears twice) | [Lucide](https://lucide.dev) v1.47.0 (`zoom-out`, `zoom-in`, `rotate-ccw`, `rotate-cw`, `refresh-cw`, `eye`, `eye-off`, `lock`) | ISC, Copyright (c) 2026 Lucide Icons and Contributors; `lock`, `zoom-in` and `zoom-out` derive from Feather and are MIT, Copyright (c) 2013-present Cole Bemis. Full text in `LICENSES/lucide.txt` |
| `assets/test-photo.jpg`, `assets/test-small.bmp` | Utagawa Kuniyoshi, *Image of a kabuki actor on a folding fan*, c. 1856, Smithsonian National Museum of Asian Art F1993.22, via [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Utagawa_Kuniyoshi_-_Image_of_a_kabuki_actor_on_a_folding_fan_-_Google_Art_Project.jpg) — cropped and scaled | Public domain |
| `assets/test-chart.png` | Drawn by this project's own bench-card script | MIT, as the rest of this repository |

## Dependencies fetched at build time

These are downloaded by the ESP-IDF component manager into `managed_components/` (not committed)
and linked into the firmware.

| Component | Licence |
| --- | --- |
| `sahlberg/libsmb2` | **LGPL-2.1-or-later** for the library. The firmware links it statically; if you distribute a built image, the LGPL's terms for that library apply to it |
| `espressif/esp_new_jpeg` | ESPRESSIF MIT — MIT with the grant limited to use on Espressif products. Prebuilt binary only |
| `espressif/esp_jpeg`, `espressif/esp_tinyusb`, `espressif/mdns`, `espressif/led_strip`, `espressif/qrcode`, `espressif/iperf` | Apache-2.0 |
| `espressif/tinyusb` | MIT |
| `espressif/libpng` | libpng licence |
| `espressif/zlib` | zlib licence |

ESP-IDF itself is Apache-2.0.
