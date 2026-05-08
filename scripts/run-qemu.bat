@echo off
REM Brook OS — Windows QEMU launcher
REM
REM Prerequisites:
REM   1. Install QEMU for Windows: https://www.qemu.org/download/#windows
REM      Add QEMU to your PATH, or edit QEMU_DIR below.
REM   2. Build artifacts must be present (ESP directory, disk images).
REM      Copy them from your Linux build or use pre-built releases.
REM
REM Usage:
REM   run-qemu.bat                  Launch with desktop (window manager)
REM   run-qemu.bat --script wm.rc   Launch with specific boot script
REM   run-qemu.bat --no-audio       Disable audio output
REM   run-qemu.bat --smp 4          Set CPU count (default: 8)

setlocal EnableDelayedExpansion

REM ---- Configuration ----
set "ROOT_DIR=%~dp0.."
set "BUILD_DIR=%ROOT_DIR%\build\release"
set "ESP_DIR=%BUILD_DIR%\esp"
set "QEMU_DIR="

REM Auto-detect QEMU installation
where qemu-system-x86_64.exe >nul 2>&1
if %errorlevel% equ 0 (
    for /f "delims=" %%i in ('where qemu-system-x86_64.exe') do set "QEMU_EXE=%%i"
) else (
    REM Common Windows install paths
    if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" (
        set "QEMU_EXE=C:\Program Files\qemu\qemu-system-x86_64.exe"
    ) else if exist "%LOCALAPPDATA%\Programs\qemu\qemu-system-x86_64.exe" (
        set "QEMU_EXE=%LOCALAPPDATA%\Programs\qemu\qemu-system-x86_64.exe"
    ) else (
        echo ERROR: qemu-system-x86_64.exe not found.
        echo.
        echo Install QEMU for Windows from: https://www.qemu.org/download/#windows
        echo Then either add it to your PATH or edit QEMU_DIR in this script.
        exit /b 1
    )
)

REM Find OVMF firmware (ships with QEMU Windows installer)
set "QEMU_INSTALL_DIR=%QEMU_EXE:\qemu-system-x86_64.exe=%"
set "OVMF_CODE="

REM Check QEMU's share directory first
if exist "%QEMU_INSTALL_DIR%\share\edk2-x86_64-code.fd" (
    set "OVMF_CODE=%QEMU_INSTALL_DIR%\share\edk2-x86_64-code.fd"
    set "OVMF_VARS=%QEMU_INSTALL_DIR%\share\edk2-i386-vars.fd"
) else if exist "%QEMU_INSTALL_DIR%\share\OVMF_CODE.fd" (
    set "OVMF_CODE=%QEMU_INSTALL_DIR%\share\OVMF_CODE.fd"
    set "OVMF_VARS=%QEMU_INSTALL_DIR%\share\OVMF_VARS.fd"
) else if exist "%QEMU_INSTALL_DIR%\share\ovmf\OVMF_CODE.fd" (
    set "OVMF_CODE=%QEMU_INSTALL_DIR%\share\ovmf\OVMF_CODE.fd"
    set "OVMF_VARS=%QEMU_INSTALL_DIR%\share\ovmf\OVMF_VARS.fd"
)

if "%OVMF_CODE%"=="" (
    echo ERROR: OVMF firmware not found.
    echo.
    echo The QEMU Windows installer usually bundles OVMF in its share\ directory.
    echo If not, download OVMF from: https://github.com/tianocore/edk2/releases
    echo Place OVMF_CODE.fd and OVMF_VARS.fd next to this script.
    exit /b 1
)

REM ---- Parse arguments ----
set "SCRIPT_NAME=desktop.rc"
set "SMP=8"
set "NO_AUDIO=0"
set "EXTRA_ARGS="

:parse_args
if "%~1"=="" goto done_args
if "%~1"=="--script" (
    set "SCRIPT_NAME=%~2"
    shift & shift
    goto parse_args
)
if "%~1"=="--smp" (
    set "SMP=%~2"
    shift & shift
    goto parse_args
)
if "%~1"=="--no-audio" (
    set "NO_AUDIO=1"
    shift
    goto parse_args
)
set "EXTRA_ARGS=%EXTRA_ARGS% %~1"
shift
goto parse_args
:done_args

REM ---- Verify build artifacts ----
if not exist "%ESP_DIR%\EFI\BOOT\BOOTX64.EFI" (
    echo ERROR: Bootloader not found at %ESP_DIR%\EFI\BOOT\BOOTX64.EFI
    echo.
    echo Build on Linux first:  ./scripts/build.sh Release
    echo Then copy build/release/esp/ and *.img files to this machine.
    exit /b 1
)

REM ---- Set up writable OVMF vars copy ----
set "OVMF_VARS_COPY=%TEMP%\brook_OVMF_VARS.fd"
copy /y "%OVMF_VARS%" "%OVMF_VARS_COPY%" >nul 2>&1

REM ---- Write boot script selection into BROOK.CFG ----
if not "%SCRIPT_NAME%"=="" (
    set "SCRIPT_UPPER=%SCRIPT_NAME%"
    REM The bootloader reads BROOK.CFG from the ESP
    (
        echo # Brook OS boot configuration
        echo TARGET=KERNEL\BROOK.ELF
        echo DEBUG_TEXT=0
        echo LOG_MEMORY=0
        echo LOG_INTERRUPTS=0
        echo SCRIPT=%SCRIPT_NAME%
    ) > "%ESP_DIR%\BROOK.CFG"
)

REM ---- Build disk drive arguments ----
set "DISK_ARGS="

REM Main FAT disk (shortcuts, DOOM WADs, etc.)
set "DISK_IMG=%ROOT_DIR%\brook_disk.img"
if exist "%DISK_IMG%" (
    set "DISK_ARGS=%DISK_ARGS% -drive if=virtio,format=raw,file=%DISK_IMG%"
    echo   Boot disk: %DISK_IMG%
) else (
    echo WARNING: Main disk image not found: %DISK_IMG%
)

REM Ext2 disk
set "EXT2_DISK=%ROOT_DIR%\brook_ext2_disk.img"
if exist "%EXT2_DISK%" (
    set "DISK_ARGS=%DISK_ARGS% -drive if=virtio,format=raw,file=%EXT2_DISK%"
    echo   Ext2 disk: %EXT2_DISK%
)

REM Nix store disk
set "NIX_DISK=%ROOT_DIR%\brook_nix_disk.img"
if exist "%NIX_DISK%" (
    set "DISK_ARGS=%DISK_ARGS% -drive if=virtio,format=raw,file=%NIX_DISK%"
    echo   Nix disk:  %NIX_DISK%
)

REM Home disk
set "HOME_DISK=%ROOT_DIR%\brook_home_disk.img"
if exist "%HOME_DISK%" (
    set "DISK_ARGS=%DISK_ARGS% -drive if=virtio,format=raw,file=%HOME_DISK%"
    echo   Home disk: %HOME_DISK%
)

REM Media folder (if present)
set "MEDIA_DIR=%ROOT_DIR%\media"
if exist "%MEDIA_DIR%" (
    set "DISK_ARGS=%DISK_ARGS% -drive if=virtio,format=raw,file=fat:rw:%MEDIA_DIR%"
    echo   Media:     %MEDIA_DIR%
)

REM ---- Audio ----
set "AUDIO_ARGS=-audiodev dsound,id=hda0 -device ich9-intel-hda,bus=pcie.0,addr=0x1b -device hda-output,audiodev=hda0"
if "%NO_AUDIO%"=="1" (
    set "AUDIO_ARGS=-audiodev none,id=hda0"
)

REM ---- Launch ----
echo.
echo Starting Brook OS...
echo   QEMU:    %QEMU_EXE%
echo   OVMF:    %OVMF_CODE%
echo   ESP:     %ESP_DIR%
echo   Script:  %SCRIPT_NAME%
echo   CPUs:    %SMP%
echo.

"%QEMU_EXE%" ^
    -machine q35 ^
    -cpu qemu64 ^
    -smp %SMP% ^
    -m 8G ^
    -drive if=pflash,format=raw,readonly=on,file="%OVMF_CODE%" ^
    -drive if=pflash,format=raw,file="%OVMF_VARS_COPY%" ^
    -drive format=raw,file=fat:rw:"%ESP_DIR%" ^
    %DISK_ARGS% ^
    -device virtio-tablet-pci ^
    -device virtio-rng-pci ^
    -device virtio-net-pci,netdev=net0 ^
    %AUDIO_ARGS% ^
    -netdev user,id=net0,hostfwd=tcp::11237-:1234 ^
    -serial stdio ^
    -no-reboot ^
    -no-shutdown ^
    %EXTRA_ARGS%

REM Clean up
del "%OVMF_VARS_COPY%" >nul 2>&1

endlocal
