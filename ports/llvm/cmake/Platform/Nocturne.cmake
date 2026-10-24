# CMake's platform definition for Nocturne (CMAKE_SYSTEM_NAME Nocturne).
# Nocturne is neither UNIX nor Windows: programs are static ELF executables
# with no shared libraries and no dynamic loader, and the SDK (the sysroot)
# keeps headers in Include and libraries in Lib.
set(NOCTURNE 1)
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)

set(CMAKE_EXECUTABLE_SUFFIX "")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

set(CMAKE_SYSTEM_INCLUDE_PATH /Include)
set(CMAKE_SYSTEM_LIBRARY_PATH /Lib)
set(CMAKE_SYSTEM_PROGRAM_PATH /System)
