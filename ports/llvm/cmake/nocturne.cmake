# CMake toolchain for programs and libraries that run on Nocturne
# (x86_64-unknown-nocturne), built on the host with the clang and lld that
# `ports/llvm/build.py host` installs. The sysroot is the SDK as it is on
# Nocturne (C:\Nocturne\SDK): build.py stages it in tools/llvm/sysroot from the
# OS build's SDK and adds the C++ runtimes to it.
set(CMAKE_SYSTEM_NAME Nocturne)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

get_filename_component(NOCTURNE_TOOLS "${CMAKE_CURRENT_LIST_DIR}/../../../tools/llvm" ABSOLUTE)
set(NOCTURNE_TRIPLE x86_64-unknown-nocturne)
set(CMAKE_SYSROOT "${NOCTURNE_TOOLS}/sysroot")

set(CMAKE_C_COMPILER "${NOCTURNE_TOOLS}/host/bin/clang.exe")
set(CMAKE_CXX_COMPILER "${NOCTURNE_TOOLS}/host/bin/clang++.exe")
set(CMAKE_ASM_COMPILER "${NOCTURNE_TOOLS}/host/bin/clang.exe")
set(CMAKE_C_COMPILER_TARGET ${NOCTURNE_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${NOCTURNE_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${NOCTURNE_TRIPLE})
set(CMAKE_AR "${NOCTURNE_TOOLS}/host/bin/llvm-ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "${NOCTURNE_TOOLS}/host/bin/llvm-ranlib.exe" CACHE FILEPATH "")
set(CMAKE_NM "${NOCTURNE_TOOLS}/host/bin/llvm-nm.exe" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${NOCTURNE_TOOLS}/host/bin/llvm-objcopy.exe" CACHE FILEPATH "")
set(CMAKE_STRIP "${NOCTURNE_TOOLS}/host/bin/llvm-strip.exe" CACHE FILEPATH "")

# Programs come from the host; headers and libraries only from the sysroot.
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
