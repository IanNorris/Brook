# Kernel Toolchain - x86_64, Clang, ELF output (bare metal)
# Targets: x86_64-elf (no OS, no stdlib)

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  x86_64)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

find_program(CLANG_C   NAMES clang   REQUIRED)
find_program(CLANG_CXX NAMES clang++ REQUIRED)

set(CMAKE_C_COMPILER   ${CLANG_C})
set(CMAKE_CXX_COMPILER ${CLANG_CXX})

set(CMAKE_C_COMPILER_TARGET   x86_64-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-elf)

find_program(LD_LLD NAMES ld.lld REQUIRED)
set(CMAKE_LINKER ${LD_LLD})

set(_KERNEL_COMMON_FLAGS
    "-ffreestanding"
    "-fno-stack-protector"
    "-mno-red-zone"
    "-fno-asynchronous-unwind-tables"
    "-mcmodel=kernel"
    "-nostdlib"
    "-nostdinc"
)
string(JOIN " " CMAKE_C_FLAGS_INIT   ${_KERNEL_COMMON_FLAGS})
string(JOIN " " CMAKE_CXX_FLAGS_INIT ${_KERNEL_COMMON_FLAGS} "-fno-exceptions" "-fno-rtti")
