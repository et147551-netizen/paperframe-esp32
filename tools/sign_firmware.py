"""PlatformIO post script: sign firmware.bin in place for OTA (ticket 61).

The frame envs build with CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT, so esp_ota_end() refuses an
image whose signature does not verify against a key the RUNNING image carries in its own signature
block. That makes the image flashed over USB part of the chain: unsigned, it can accept no OTA at
all. PlatformIO signs only under hardware secure boot, so this does it for every build.

The key lives OUTSIDE the repository, beside the rule-file key:
    ~/.m5paper-keys/firmware_signing_rsa3072.pem
A build without it FAILS rather than producing an unsigned image, because an unsigned image is a
board that needs USB again before it can be updated.

espsecure.py needs cryptography, ecdsa, intelhex, pyserial, reedsolo, bitstring and pyyaml, and
PlatformIO's ESP-IDF venv ships only the first. docs/build-system.md has the one-line
install.
"""

import os
import subprocess
from pathlib import Path

Import("env")  # noqa: F821 -- provided by PlatformIO

KEY = Path.home() / ".m5paper-keys" / "firmware_signing_rsa3072.pem"


def sign(target, source, env):
    image = str(target[0])
    if not KEY.is_file():
        print("sign_firmware: no signing key at %s -- refusing to leave an unsigned image" % KEY)
        env.Exit(1)
    python = env.subst("$ESPIDF_PYTHONEXE")
    tool = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "espsecure.py")
    signed = image + ".signed"
    rc = subprocess.call([python, tool, "sign_data", "--version", "2", "--keyfile", str(KEY),
                          "--output", signed, image])
    if rc != 0:
        print("sign_firmware: espsecure.py failed (%d)" % rc)
        env.Exit(1)
    os.replace(signed, image)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", sign)  # noqa: F821
