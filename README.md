# paperframe

Firmware for a digital photo frame on a colour E Ink (Spectra 6) panel, written for ESP-IDF with
its own panel driver — no M5GFX, M5Unified or M5Stack component in the build.

It runs on two boards:

| Board | Panel | Build flag | Main env |
| --- | --- | --- | --- |
| M5Stack M5Paper Color | EL040EF1, 400x600 | `-DBOARD_M5PAPER_COLOR` | `frame` |
| Seeed reTerminal E1002 | ED2208-GCA, 800x480 | `-DBOARD_RETERMINAL_E1002` | `frame_e1002` |

## Features

- Slideshow from a microSD card (or internal flash when no card is present), with configurable
  interval, order, rotation, quiet hours and an optional caption band.
- Web UI in English, Japanese and Traditional Chinese: album grid, upload, settings. Every API route
  requires a token, obtained by pairing from the QR code or the 8-character code shown on the panel.
- Photographs mirrored from an SMB share and from a link-shared Google Photos album, fetched on
  demand and resized on the device.
- On-device quantisation to the six-colour panel, including a port of
  [epdoptimize](https://github.com/paperlesspaper/epdoptimize)'s automatic adjustment.
- Signed over-the-air updates with automatic rollback.
- A panel maintenance mode (full-screen colour flats and a white park for idle hours).
- USB mass-storage mode for the card (M5Paper Color).

The functional requirements follow M5Stack's shipping firmware,
[M5PaperColor-UserDemo](https://github.com/m5stack/M5PaperColor-UserDemo); see
`docs/requirements/digital-frame.md`.

## Building

PlatformIO with `framework = espidf` (ESP-IDF 5.5.0).

```bash
pio run -e frame          # M5Paper Color
pio run -e frame_e1002    # reTerminal E1002
pio test -e native        # host-side unit tests (also native_e1002)
```

`platformio.ini` lists the other envs (bring-up, measurement and probe builds).

### Signing key

The two frame envs sign their image after the build and **fail without a key**, because an unsigned
image cannot accept an OTA update. Create one outside the repository and keep a backup — losing it
means reflashing every board over USB:

```bash
espsecure.py generate_signing_key --version 2 --scheme rsa3072 ~/.m5paper-keys/firmware_signing_rsa3072.pem
```

PlatformIO's ESP-IDF Python environment may need `ecdsa intelhex pyserial reedsolo bitstring pyyaml`
installed for `espsecure.py`; `docs/build-system.md` has the command.

### Local settings (optional)

`platformio_local.ini` is git-ignored and merged over `platformio.ini`. Use it to seed Wi-Fi
credentials or a fixed API token at build time. A same-named `[env:...]` section **replaces**
`build_flags`, so repeat the board flag and `-DBOARD_NO_POWER_OFF`:

```ini
[env:frame]
build_flags = -DBUILD_FRAME -DBOARD_M5PAPER_COLOR -DBOARD_NO_POWER_OFF
  -DFRAME_WIFI_SSID='"your-ssid"' -DFRAME_WIFI_PASS='"your-password"'
```

Without it, a first boot raises the frame's own access point and shows its credentials and a
pairing code on the panel.

## Documentation

- `docs/requirements/digital-frame.md` — requirements.
- `docs/` — build system, render pipeline, web API, SMB mirror, and the board and storage
  layers.
- `docs/board-pinmap.md` — GPIO and peripheral map for both boards.
- `tools/README.md` — the build, device, check and bench scripts.

These are working documents. They cite a bench notebook (tickets, capture logs, plans),
measurement records and reference and research notes that are not published.

## Licence

MIT — see `LICENSE`. Some files and dependencies carry other terms (BSD-2-Clause, Apache-2.0,
LGPL-2.1 for libsmb2); see `THIRD_PARTY_NOTICES.md`.
