"""PlatformIO library build script for OpenLNK (esp32/library.json).

Compiles the single-source cleanroom C99 core (../cleanroom/src/*.c) alongside
esp32/src/*.c without duplicating source files in version control, and applies
-std=c99 strictly to C translation units (never breaking C++ Arduino sketches).
When running inside a self-contained release bundle (where src/openlnk_core/
already exists), PlatformIO builds src/ directly via library.json srcFilter.
"""

import os

Import("env")  # noqa: F821

CORE_SOURCES = [
    "openlnk.c",
    "crc16.c",
    "protocol.c",
    "storage_mem.c",
    "server.c",
    "client.c",
]

lib_dir = os.path.abspath(".")
bundled_core_dir = os.path.join(lib_dir, "src", "openlnk_core")
cleanroom_root = os.path.abspath(os.path.join(lib_dir, "..", "cleanroom"))
cleanroom_inc = os.path.join(cleanroom_root, "include")
cleanroom_src = os.path.join(cleanroom_root, "src")

# Apply C99 only to C files so C++ sketches (.cpp/.ino) compile cleanly.
env.Append(CFLAGS=["-std=c99"])  # noqa: F821

if (not os.path.isdir(bundled_core_dir)) and os.path.isdir(cleanroom_inc) and os.path.isdir(cleanroom_src):
    env.Append(CPPPATH=[cleanroom_inc])  # noqa: F821
    try:
        Import("projenv")  # noqa: F821
        projenv.Append(CPPPATH=[cleanroom_inc])  # noqa: F821
    except Exception:
        pass

    src_filter = ["-<*>"] + [f"+<{name}>" for name in CORE_SOURCES]
    env.BuildSources(  # noqa: F821
        os.path.join("$BUILD_DIR", "openlnk_core"),
        cleanroom_src,
        src_filter=src_filter,
    )
