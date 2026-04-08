# Kernel Toolchain - x86_64, Clang, ELF output (bare metal)
set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  x86_64)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS   1 CACHE BOOL "" FORCE)
set(CMAKE_CXX_COMPILER_WORKS 1 CACHE BOOL "" FORCE)

find_program(CLANG_CXX NAMES clang++ REQUIRED)
set(CMAKE_CXX_COMPILER ${CLANG_CXX})
set(CMAKE_CXX_COMPILER_TARGET x86_64-elf)

find_program(LD_LLD NAMES ld.lld REQUIRED)
set(CMAKE_LINKER ${LD_LLD})

set(CMAKE_CXX_FLAGS_INIT
    "--target=x86_64-elf -ffreestanding -fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables -mcmodel=large -fno-exceptions -fno-rtti -nostdlib")

# Use ld.lld directly for linking
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
