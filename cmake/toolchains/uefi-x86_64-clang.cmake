# UEFI Bootloader Toolchain - x86_64, Clang, PE/COFF output
# Targets: x86_64-unknown-windows (produces native PE/COFF, no objcopy needed)

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  x86_64)

# Skip CMake's compiler sanity checks - we're a freestanding cross-compilation
# target and the test program won't compile/link in a hosted environment.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS   1 CACHE BOOL "" FORCE)
set(CMAKE_CXX_COMPILER_WORKS 1 CACHE BOOL "" FORCE)

find_program(CLANG_C   NAMES clang   REQUIRED)
find_program(CLANG_CXX NAMES clang++ REQUIRED)

set(CMAKE_C_COMPILER   ${CLANG_C})
set(CMAKE_CXX_COMPILER ${CLANG_CXX})

set(CMAKE_C_COMPILER_TARGET   x86_64-unknown-windows)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-windows)

# Use LLVM's lld-link for PE/COFF linking
find_program(LLD_LINK NAMES lld-link REQUIRED)
set(CMAKE_LINKER ${LLD_LINK})

# Base compile flags for UEFI freestanding environment.
# --target must be in the flags (not just CMAKE_CXX_COMPILER_TARGET) to ensure
# Clang emits COFF objects regardless of the host platform's default.
set(_UEFI_COMMON_FLAGS
    "--target=x86_64-unknown-windows"
    "--no-default-config"
    "-Wno-unused-command-line-argument"
    "-fno-pic"
    "-ffreestanding"
    "-fno-stack-protector"
    "-fshort-wchar"
    "-mno-red-zone"
    "-fno-asynchronous-unwind-tables"
    "-nostdlib"
)
string(JOIN " " CMAKE_C_FLAGS_INIT   ${_UEFI_COMMON_FLAGS})
string(JOIN " " CMAKE_CXX_FLAGS_INIT ${_UEFI_COMMON_FLAGS} "-fno-exceptions" "-fno-rtti")

# Build type flags
set(CMAKE_C_FLAGS_DEBUG_INIT          "-O0 -g")
set(CMAKE_CXX_FLAGS_DEBUG_INIT        "-O0 -g")
set(CMAKE_C_FLAGS_RELEASE_INIT        "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT      "-O2 -DNDEBUG")

# lld-link flags: no default libs, EFI application subsystem
# Note: entry point is set per-target in the bootloader CMakeLists
set(CMAKE_EXE_LINKER_FLAGS_INIT "/nodefaultlib /subsystem:efi_application")

# lld-link uses /OUT: syntax; tell CMake how to link
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> /OUT:<TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> /OUT:<TARGET> <LINK_LIBRARIES>")
