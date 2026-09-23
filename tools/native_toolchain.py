"""Make env:native build with clang, because this machine has no gcc.

clang 22 is installed under Program Files but not on PATH, and Visual Studio 2022
Build Tools supplies the MSVC headers and libraries it targets.

Replacing env["CC"] does not work here: PlatformIO builds project sources, libraries
and the test runner in separate cloned SCons environments, and the platform's own
build script sets CC=gcc in each. Only `projenv` would be reachable from a post
script, which leaves the Unity library and the test runner still calling gcc.

So instead: generate gcc-named shims that forward to clang, and put them on PATH.
Every cloned environment inherits ENV["PATH"], so one change covers all of them.
clang's driver is gcc-compatible, so the forwarding is transparent.

Referenced from platformio.ini as `extra_scripts = pre:tools/native_toolchain.py`.
"""

import os

Import("env")  # noqa: F821  (injected by SCons)

CANDIDATE_DIRS = [
    r"C:\Program Files\LLVM\bin",
    r"C:\Program Files (x86)\LLVM\bin",
]

# gcc-name -> LLVM executable it forwards to.
SHIMS = {
    "gcc": "clang.exe",
    "g++": "clang++.exe",
    "ar": "llvm-ar.exe",
    "ranlib": "llvm-ranlib.exe",
}

llvm_bin = next(
    (d for d in CANDIDATE_DIRS if os.path.isfile(os.path.join(d, "clang.exe"))), None
)

if llvm_bin is None:
    # clang may be on PATH already, or this may not be Windows. Do nothing and let the
    # build fail with the compiler's own message rather than a guess from here.
    print("native_toolchain: no LLVM found in %s; leaving toolchain alone"
          % ", ".join(CANDIDATE_DIRS))
else:
    shim_dir = os.path.join(env.subst("$PROJECT_BUILD_DIR"), "native-shim")  # noqa: F821
    os.makedirs(shim_dir, exist_ok=True)

    for name, target in SHIMS.items():
        with open(os.path.join(shim_dir, name + ".cmd"), "w") as f:
            f.write('@echo off\r\n"%s" %%*\r\n' % os.path.join(llvm_bin, target))

    env.PrependENVPath("PATH", shim_dir)  # noqa: F821
    env.PrependENVPath("PATH", llvm_bin)  # noqa: F821
