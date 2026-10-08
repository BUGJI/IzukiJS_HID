@echo off
rem Helper to run ESP-IDF commands from any shell (clears MSYSTEM set by Git Bash).
set MSYSTEM=
call "C:\Users\BUGJI\esp\esp-idf\export.bat" >nul
cd /d "%~dp0"
idf.py %*
