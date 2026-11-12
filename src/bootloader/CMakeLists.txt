cmake_minimum_required(VERSION 3.25)
project(brook-bootloader CXX)

if(NOT DEFINED BROOK_ROOT)
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../..")
endif()

set(UEFI_INCLUDE_DIRS
    "${BROOK_ROOT}/vendor/uefi-headers/Include"
    "${BROOK_ROOT}/vendor/uefi-headers/Include/X64"
)

set(SHARED_INCLUDE_DIRS
    "${BROOK_ROOT}/src/shared"
    "${BROOK_ROOT}/src/shared/inc_km"
)

find_program(BOOT_PYTHON python3 REQUIRED)
set(BOOT_LOGO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/brook_boot_logo_data.h")
add_custom_command(OUTPUT "${BOOT_LOGO_HEADER}"
    COMMAND ${BOOT_PYTHON} "${BROOK_ROOT}/scripts/bake_logo.py"
            "${BROOK_ROOT}/data/branding/brook-mark.png" "${BOOT_LOGO_HEADER}" 300
    DEPENDS "${BROOK_ROOT}/scripts/bake_logo.py" "${BROOK_ROOT}/data/branding/brook-mark.png"
    VERBATIM)
add_executable(bootloader
    "${BOOT_LOGO_HEADER}"
    src/main.cpp
    src/config.cpp
    src/console.cpp
    src/graphics.cpp
    src/memory.cpp
    src/acpi.cpp
    src/update_slots.cpp
    src/recovery_firmware_reader.cpp
    src/recovery_firmware_discovery.cpp
    src/fs.cpp
    src/elf_loader.cpp
    src/paging.cpp
    src/runtime.cpp
)

target_include_directories(bootloader PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${UEFI_INCLUDE_DIRS}
    ${SHARED_INCLUDE_DIRS}
    src   # so files can include each other without path prefix
)

target_compile_options(bootloader PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wno-unused-parameter
)

set_target_properties(bootloader PROPERTIES
    OUTPUT_NAME "BOOTX64"
    SUFFIX      ".efi"
)

target_link_options(bootloader PRIVATE
    "/entry:EfiMain"
)

# Separate one-time diagnostic: does not alter the regular or guarded target.
option(BROOK_BUILD_BOOT_INVENTORY "Build a boot-variable inventory loader" OFF)
if(BROOK_BUILD_BOOT_INVENTORY)
    get_target_property(INVENTORY_SOURCES bootloader SOURCES)
    add_executable(bootloader_inventory ${INVENTORY_SOURCES} src/boot_variable_inventory.cpp)
    target_include_directories(bootloader_inventory PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" ${UEFI_INCLUDE_DIRS} ${SHARED_INCLUDE_DIRS} src)
    target_compile_options(bootloader_inventory PRIVATE -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter)
    target_compile_definitions(bootloader_inventory PRIVATE BROOK_EFI_BOOT_INVENTORY=1)
    target_link_options(bootloader_inventory PRIVATE "/entry:EfiMain")
    set_target_properties(bootloader_inventory PROPERTIES OUTPUT_NAME "BROOKINV" SUFFIX ".efi")
endif()

# Separate experimental binary: never replace the generic EFI target by
# enabling this build option. Deployment requires every firmware-reachable EFI
# entrypoint to be inventoried and migrated independently.
option(BROOK_BUILD_GUARDED_BOOTLOADER "Build a mandatory recovery-guard EFI variant" OFF)
option(BROOK_GUARD_CHECK_DATA "Experimental: select verified recovery for dirty or invalid data" OFF)
option(BROOK_GUARD_ALTERNATE_RECOVERY "Try independently staged RECOV2 before legacy recovery" OFF)
if(BROOK_GUARD_CHECK_DATA AND NOT BROOK_BUILD_GUARDED_BOOTLOADER)
    message(FATAL_ERROR "Data inspection requires the guarded bootloader")
endif()
if(BROOK_GUARD_ALTERNATE_RECOVERY AND NOT BROOK_BUILD_GUARDED_BOOTLOADER)
    message(FATAL_ERROR "Alternate recovery requires the guarded bootloader")
endif()
if(BROOK_BUILD_GUARDED_BOOTLOADER)
    set(BROOK_GUARD_IDENTITY "" CACHE STRING "96 lowercase hex digits: disk GUID, partition GUID, ext2 UUID (on-disk byte order)")
    string(LENGTH "${BROOK_GUARD_IDENTITY}" BROOK_GUARD_IDENTITY_LENGTH)
    if(NOT BROOK_GUARD_IDENTITY_LENGTH EQUAL 96 OR BROOK_GUARD_IDENTITY MATCHES "[^0-9a-f]")
        message(FATAL_ERROR "BROOK_GUARD_IDENTITY must contain exactly 96 lowercase hexadecimal digits")
    endif()
    get_target_property(BOOTLOADER_SOURCES bootloader SOURCES)
    add_executable(bootloader_guarded ${BOOTLOADER_SOURCES})
    target_include_directories(bootloader_guarded PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}" ${UEFI_INCLUDE_DIRS} ${SHARED_INCLUDE_DIRS} src)
    target_compile_options(bootloader_guarded PRIVATE
        -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter)
    target_compile_definitions(bootloader_guarded PRIVATE BROOK_RECOVERY_GUARD_REQUIRED=1
        BROOK_GUARD_IDENTITY="${BROOK_GUARD_IDENTITY}")
    if(BROOK_GUARD_ALTERNATE_RECOVERY)
        target_compile_definitions(bootloader_guarded PRIVATE BROOK_GUARD_ALTERNATE_RECOVERY=1)
    endif()
    if(BROOK_GUARD_CHECK_DATA)
        set(BROOK_GUARD_DATA_FIRST "" CACHE STRING "Expected data partition first LBA")
        set(BROOK_GUARD_DATA_SECTORS "" CACHE STRING "Expected data partition sector count")
        if(NOT BROOK_GUARD_DATA_FIRST MATCHES "^[1-9][0-9]*$" OR NOT BROOK_GUARD_DATA_SECTORS MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "Guarded data inspection requires positive decimal partition bounds")
        endif()
        target_compile_definitions(bootloader_guarded PRIVATE BROOK_GUARD_CHECK_DATA=1
            BROOK_GUARD_DATA_FIRST=${BROOK_GUARD_DATA_FIRST} BROOK_GUARD_DATA_SECTORS=${BROOK_GUARD_DATA_SECTORS})
    endif()
    target_link_options(bootloader_guarded PRIVATE "/entry:EfiMain")
    set_target_properties(bootloader_guarded PROPERTIES OUTPUT_NAME "BROOKGUARD" SUFFIX ".efi")
endif()

# Private QEMU-only probe. It writes its result to QEMU's debugcon and halts;
# it is never installed as a normal or recovery bootloader.
option(BROOK_BUILD_RECOVERY_PROBE "Build private OVMF disk-inspection probe" OFF)
if(BROOK_BUILD_RECOVERY_PROBE)
    set(BROOK_RECOVERY_PROBE_IDENTITY "" CACHE STRING "96 lowercase hex identity digits")
    set(BROOK_RECOVERY_PROBE_FIRST "" CACHE STRING "Expected data partition first LBA")
    set(BROOK_RECOVERY_PROBE_SECTORS "" CACHE STRING "Expected data partition sector count")
    string(LENGTH "${BROOK_RECOVERY_PROBE_IDENTITY}" BROOK_RECOVERY_PROBE_IDENTITY_LENGTH)
    if(NOT BROOK_RECOVERY_PROBE_IDENTITY_LENGTH EQUAL 96 OR BROOK_RECOVERY_PROBE_IDENTITY MATCHES "[^0-9a-f]" OR
       NOT BROOK_RECOVERY_PROBE_FIRST MATCHES "^[1-9][0-9]*$" OR NOT BROOK_RECOVERY_PROBE_SECTORS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Probe requires a 96-digit identity and positive decimal data geometry")
    endif()
    add_executable(recovery_probe src/recovery_probe_main.cpp src/recovery_firmware_discovery.cpp src/runtime.cpp)
    target_include_directories(recovery_probe PRIVATE ${UEFI_INCLUDE_DIRS} ${SHARED_INCLUDE_DIRS} src)
    target_compile_options(recovery_probe PRIVATE -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter)
    target_compile_definitions(recovery_probe PRIVATE BROOK_RECOVERY_PROBE_IDENTITY="${BROOK_RECOVERY_PROBE_IDENTITY}"
        BROOK_RECOVERY_PROBE_FIRST=${BROOK_RECOVERY_PROBE_FIRST} BROOK_RECOVERY_PROBE_SECTORS=${BROOK_RECOVERY_PROBE_SECTORS})
    target_link_options(recovery_probe PRIVATE "/entry:EfiMain")
    set_target_properties(recovery_probe PROPERTIES OUTPUT_NAME "BROOKPROBE" SUFFIX ".efi")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
