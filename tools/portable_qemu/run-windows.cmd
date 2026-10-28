@echo off
py -3 "%~dp0launch.py" %*
if errorlevel 1 pause
