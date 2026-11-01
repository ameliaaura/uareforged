# Cross-compile toolchain: macOS (Homebrew mingw-w64, UCRT) -> Windows x86_64.
# UA_WIN_SYSROOT = the dependency sysroot built by packaging/windows/fetch-deps.py.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH "$ENV{UA_WIN_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# The engine's own C++ runtime is linked in statically so it never fights the
# MSYS2-built libstdc++-6.dll that the bundled SDL/OpenAL DLLs load.
# No link time in the PE header (reproducible builds).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp")
