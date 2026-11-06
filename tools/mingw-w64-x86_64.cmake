# CMake toolchain file: cross-compile for Windows x86-64 (MinGW-w64 GCC, UCRT, POSIX threads, SEH) on macOS.
# Used by tools/anyps5_build_win.sh to build AnyPS5's Windows PS5 libraries (docs/ps5.md, "AnyPS5 from main").
#
#   cmake -S <src> -B <build> -DCMAKE_TOOLCHAIN_FILE=tools/mingw-w64-x86_64.cmake [-DMINGW_SYSV_SEH=OFF]
#
# Base toolchain: Homebrew's mingw-w64 (`brew install mingw-w64`: GCC 16.2.0, UCRT, POSIX threads, SEH), i.e. the
# configuration of MSYS2's UCRT64 GCC, one of the two toolchains AnyPS5's docs/dev/BUILD.md supports.
# MINGW_SYSV_SEH=ON (default): compile with tools/anyps5_cross/x86_64-w64-mingw32-g++, Homebrew's driver plus cc1/cc1plus
#   rebuilt with the GCC fix MSYS2 applies (third_party/gcc-mingw-sysv-seh, built by tools/gcc_sysv_seh_build.sh).
#   Stock GCC 16.2 cannot compile catch handlers in sysv_abi functions on Win64, and without unwind tables it
#   falls back to SJLJ, so AnyPS5's configure check ("DWARF exceptions or GCC with System V ABI SEH support") fails.
# MINGW_SYSV_SEH=OFF: stock Homebrew GCC (enough for plain MinGW programs, not for AnyPS5's libraries).
# (clang/llvm-mingw cannot be used: clang has no __builtin_sysv_va_* and refuses va_start in sysv_abi functions
#  on Windows, which AnyPS5's libc uses for every PS5-ABI variadic export.)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MINGW_PREFIX x86_64-w64-mingw32)
if(NOT DEFINED MINGW_SYSV_SEH)
    set(MINGW_SYSV_SEH ON)
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MINGW_SYSV_SEH MINGW_ROOT)

if(NOT DEFINED MINGW_ROOT)
    find_program(_mingw_gcc ${MINGW_PREFIX}-gcc HINTS /opt/homebrew/bin /usr/local/bin REQUIRED)
    get_filename_component(_mingw_gcc "${_mingw_gcc}" REALPATH)
    get_filename_component(MINGW_ROOT "${_mingw_gcc}/../.." ABSOLUTE)   # .../toolchain-x86_64
endif()
set(_mingw_bin "${MINGW_ROOT}/bin")
if(NOT EXISTS "${_mingw_bin}/${MINGW_PREFIX}-gcc")
    message(FATAL_ERROR "mingw-w64 toolchain not found at ${MINGW_ROOT} (brew install mingw-w64, or set MINGW_ROOT)")
endif()

if(MINGW_SYSV_SEH)
    # the wrapper finds Homebrew's toolchain in $MINGW_ROOT (default /opt/homebrew/opt/mingw-w64/toolchain-x86_64)
    # and the patched compilers in $GCC_SYSV_SEH (default third_party/gcc-mingw-sysv-seh/overlay)
    get_filename_component(_overlay "${CMAKE_CURRENT_LIST_DIR}/../third_party/gcc-mingw-sysv-seh/overlay" ABSOLUTE)
    if(NOT EXISTS "${_overlay}/cc1plus" AND NOT DEFINED ENV{GCC_SYSV_SEH})
        message(FATAL_ERROR "patched GCC compilers missing in ${_overlay}: run tools/gcc_sysv_seh_build.sh")
    endif()
    set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/anyps5_cross/${MINGW_PREFIX}-gcc")
    set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/anyps5_cross/${MINGW_PREFIX}-g++")
else()
    set(CMAKE_C_COMPILER "${_mingw_bin}/${MINGW_PREFIX}-gcc")
    set(CMAKE_CXX_COMPILER "${_mingw_bin}/${MINGW_PREFIX}-g++")
endif()
set(CMAKE_RC_COMPILER "${_mingw_bin}/${MINGW_PREFIX}-windres")
# AnyPS5 checks that CMAKE_RANLIB exists ("Stable import libraries require ranlib") and runs `ranlib -D`;
# it renames .eh_frame with CMAKE_OBJCOPY.
set(CMAKE_AR "${_mingw_bin}/${MINGW_PREFIX}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_mingw_bin}/${MINGW_PREFIX}-ranlib" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${_mingw_bin}/${MINGW_PREFIX}-objcopy" CACHE FILEPATH "")
set(CMAKE_STRIP "${_mingw_bin}/${MINGW_PREFIX}-strip" CACHE FILEPATH "")
set(CMAKE_NM "${_mingw_bin}/${MINGW_PREFIX}-nm" CACHE FILEPATH "")
set(CMAKE_DLLTOOL "${_mingw_bin}/${MINGW_PREFIX}-dlltool" CACHE FILEPATH "")

# Target headers/libraries only from the mingw sysroot; programs (python3, git, sh) from the build host.
set(CMAKE_FIND_ROOT_PATH "${MINGW_ROOT}/${MINGW_PREFIX}" "${MINGW_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Runtime DLLs shipped beside the libraries: ${MINGW_ROOT}/${MINGW_PREFIX}/lib/{libgcc_s_seh-1,libstdc++-6}.dll and
# ${MINGW_ROOT}/${MINGW_PREFIX}/bin/libwinpthread-1.dll (tools/anyps5_build_win.sh copies them).
