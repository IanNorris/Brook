# meson.build

project('brook-bootloader', 'cpp', version : '0.1.0')

build_type = get_option('buildtype') # 'debug', 'release', 'debugoptimized', etc.
if build_type == 'debug' or build_type == 'debugoptimized'
  add_project_arguments('-DBOOT_MODE_DEBUG', language : 'cpp')
endif

inc_dirs = include_directories('../vendor/uefi-headers/Include', '../vendor/uefi-headers/Include/X64')

src_files = files(
  'bootloader.cpp'
)

executable('bootloader.efi',
           src_files,
           include_directories : inc_dirs,
           install : false # Installs to {prefix}/bin by default
           )
