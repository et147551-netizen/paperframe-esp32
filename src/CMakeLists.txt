# The original auto-generated version globbed ${CMAKE_SOURCE_DIR}/src/*.* -- every file,
# not every source. That swept up idf_component.yml as if it were code. Sources only.
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

# Embedded test images are NOT declared here. CMake's EMBED_FILES generates an assembly
# stub that PlatformIO's SCons layer then fails to find:
#     Source `.pio\build\photo\test-chart.png.S' not found
# PlatformIO has its own option for this, `board_build.embed_files`, which produces the
# same `_binary_<name>_start` symbols. See platformio.ini.
# The layer directories, all of them, unconditionally. CMake cannot see PlatformIO's
# build_flags (docs/build-system.md), so it cannot know which board was selected --
# which is why every board's and panel's header basename must be UNIQUE. Two files called
# `pins.h` would resolve by directory order rather than by the selected board, silently.
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "core"
                                    "panel" "panel/el040ef1" "panel/ed2208gca"
                                    "board" "board/m5papercolor"
                                    "board/reterminal_e1002"
                                    "app" "entry")

# src/app/smb_aes_hw.c needs mbedTLS's own headers to see the ESP32 port's aes_alt.h, which is
# what turns mbedtls_aes_init() into esp_aes_init() and puts it on the AES peripheral.
# Without the dependency the file compiles against the generic declarations and the link
# fails on undefined mbedtls_aes_* -- the hardware symbols are named esp_aes_*. Added with
# target_link_libraries rather than a REQUIRES on the register call above, so it cannot
# narrow the implicit dependency set this component gets as the project's main one.
target_link_libraries(${COMPONENT_LIB} PRIVATE idf::mbedtls)

# GCC dies with `internal compiler error: Segmentation fault` on IDF's own
# esp_lcd/rgb/esp_lcd_panel_rgb.c:681, in rgb_panel_draw_bitmap, during `RTL pass: ira`.
# docs/build-system.md records it as non-deterministic and curable by running the build
# again -- and on 2026-09-04 it stopped being either: env:photo hit it **14 times in a row**, 4 s apart,
# including after its build directory was deleted. Lowering the optimisation level for that
# one component avoids it, which is consistent with the earlier finding that -O1, -Os and
# -O0 all did.
#
# Scoped to esp_lcd, and safe here because **this project does not use esp_lcd at all** --
# the EPD driver is src/panel/el040ef1/epd_el040ef1.c on a plain SPI master, and nothing links an
# esp_lcd panel. So this cannot affect any measurement: it changes the code generation of a
# component that is compiled and then never called.
if(TARGET __idf_esp_lcd)
    idf_component_get_property(esp_lcd_lib esp_lcd COMPONENT_LIB)
    target_compile_options(${esp_lcd_lib} PRIVATE -O1)
endif()

# libsmb2 (ticket 24) does not build under ESP-IDF's default -Werror=all: its vendored
# SHA/HMAC code declares hmacResult()'s digest as `uint8_t[64]` in the header and
# `uint8_t *` in the definition, which GCC 11+ reports as -Warray-parameter.
#
# The fix is scoped to that component on purpose. -Werror=all is the project's, and
# dropping it -- or setting COMPILER_DISABLE_DEFAULT_ERRORS, which is the only global
# switch IDF offers -- would silence the same class of mistake in our own code to
# accommodate a third party's. None of CONFIG_COMPILER_DISABLE_GCC12/13/14_WARNINGS
# covers this warning; they only cover -Wno-address, -Wno-use-after-free,
# -Wno-xor-used-as-pow, -Wno-enum-int-mismatch and -Wno-calloc-transposed-args.
#
# Guarded because the component is absent from the CMake graph in env:native.
if(TARGET __idf_sahlberg__libsmb2)
    idf_component_get_property(smb2_lib sahlberg__libsmb2 COMPONENT_LIB)
    target_compile_options(${smb2_lib} PRIVATE -Wno-error=array-parameter)

    # smb3-seal.c does `#include <esp_system.h>` and `#define random esp_random`, which
    # was correct for ESP-IDF 4.x. esp_random() moved to esp_random.h in 5.x, so on
    # 5.5.0 the call is an implicit declaration and -Werror=all stops the build. The
    # component is 3.0.1, the newest on the registry.
    #
    # `-include esp_random.h` is the obvious fix and it CANNOT WORK HERE. PlatformIO's
    # espidf builder sorts the flag list before handing it to SCons
    # (platforms/espressif32/builder/frameworks/espidf.py:466-468, `sorted(...)`), which
    # separates `-include` from its argument. The flags reach build.ninja intact and are
    # still dead on arrival -- so no two-token compiler flag works under this build
    # system, for any component. Worth knowing before spending an hour on the next one.
    #
    # So: accept the implicit declaration, scoped to this component. It is safe *here*
    # specifically -- esp_random() returns uint32_t, an implicit declaration assumes
    # int, and on xtensa both are 32 bits in the same return register. The only call is
    # `pdu->crypt[i] = random() & 0xff`. Verify the warning count stays at one; a second
    # implicit declaration in this library is a new question, not a covered one.
    target_compile_options(${smb2_lib} PRIVATE -Wno-error=implicit-function-declaration)

    # Ticket 33: send libsmb2's per-block AES to the S3's AES peripheral. The component
    # re-runs the whole key expansion for every 16-byte block (lib/aes.c:444-456), which
    # measured 79.9 us per block and 82 % of what a 4 KB signed read costs. See
    # src/app/smb_aes_hw.c for why this is a wrap and not a patch, and for how the run says
    # whether the wrap actually took -- a green build here does not.
    #
    # One token, deliberately: the two-token case above is why.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=AES128_ECB_encrypt")
endif()
