cmake_minimum_required(VERSION 3.16)
project(brook_clearwater_shell LANGUAGES CXX)

include("${BROOK_ROOT}/cmake/ChromeFonts.cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
brook_chrome_fonts(FONTS "${Python3_EXECUTABLE}" "${BROOK_DESKTOP_FONT_TTF}" "${BROOK_MONO_FONT_TTF}"
                   "${CMAKE_CURRENT_BINARY_DIR}")
set(KERNEL_SOURCE "${BROOK_ROOT}/src/kernel/src")
add_executable(CWSHELL.ELF main.cpp "${KERNEL_SOURCE}/chrome_shell.cpp"
    "${KERNEL_SOURCE}/chrome_raster.cpp" ${FONTS})
target_include_directories(CWSHELL.ELF PRIVATE "${KERNEL_SOURCE}" "${BROOK_ROOT}/src/shared")
target_compile_features(CWSHELL.ELF PRIVATE cxx_std_17)
target_compile_options(CWSHELL.ELF PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
target_link_options(CWSHELL.ELF PRIVATE -static)
install(TARGETS CWSHELL.ELF RUNTIME DESTINATION bin)
