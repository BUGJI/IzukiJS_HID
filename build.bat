@echo off
setlocal
rem Helper to run ESP-IDF commands from any shell (clears MSYSTEM set by Git Bash).
set MSYSTEM=

rem Locate export.bat: prefer IDF_PATH, fall back to the conventional install dir.
if defined IDF_PATH (
    set "IDF_EXPORT=%IDF_PATH%\export.bat"
) else (
    set "IDF_EXPORT=%USERPROFILE%\esp\esp-idf\export.bat"
)

if not exist "%IDF_EXPORT%" (
    echo [build.bat] ESP-IDF export.bat not found at:
    echo [build.bat]   %IDF_EXPORT%
    echo [build.bat] Set IDF_PATH to your ESP-IDF checkout, or edit this script.
    exit /b 1
)

call "%IDF_EXPORT%" >nul
cd /d "%~dp0"
idf.py %*
