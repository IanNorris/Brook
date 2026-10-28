@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem The Windows installer does not add QEMU to PATH. Keep existing PATH
rem entries preferred, then try the default installation directory.
if exist "%ProgramFiles%\qemu\qemu-system-x86_64.exe" set "PATH=%PATH%;%ProgramFiles%\qemu"
py -3 "%~dp0launch.py" %*
set "BROOK_EXIT_CODE=%ERRORLEVEL%"
if not "%BROOK_EXIT_CODE%"=="0" (
    echo.
    echo If QEMU is missing, download the Windows installer from:
    echo https://www.qemu.org/download/#windows
    echo The default installation folder is "%ProgramFiles%\qemu"
    pause
)
exit /b %BROOK_EXIT_CODE%
