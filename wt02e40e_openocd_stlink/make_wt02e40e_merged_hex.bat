@echo off
setlocal EnableExtensions

rem Optional helper. This only merges hex files. It does not flash.
rem Requires Nordic mergehex.exe from nRF Command Line Tools.
rem
rem Usage:
rem   make_wt02e40e_merged_hex.bat
rem   make_wt02e40e_merged_hex.bat C:\ncs\v3.3.1\nrf\samples\wifi\sta\build

if "%~1"=="" (
    set "BUILD_DIR=C:\ncs\v3.3.1\nrf\samples\wifi\sta\build"
) else (
    set "BUILD_DIR=%~1"
)

set "MERGEHEX=mergehex"
set "APP_HEX=%BUILD_DIR%\sta\zephyr\zephyr.hex"
set "NET_HEX=%BUILD_DIR%\hci_ipc\zephyr\merged_CPUNET.hex"
set "OUT_HEX=%BUILD_DIR%\wt02e40e_full_merged.hex"

if not exist "%APP_HEX%" (
    echo Missing app hex: %APP_HEX%
    exit /b 3
)

if not exist "%NET_HEX%" (
    echo Missing net hex: %NET_HEX%
    echo Try build\hci_ipc\zephyr\zephyr.hex or rebuild with sysbuild.
    exit /b 4
)

"%MERGEHEX%" -m "%APP_HEX%" "%NET_HEX%" -o "%OUT_HEX%"

if errorlevel 1 (
    echo mergehex failed.
    exit /b 5
)

echo Wrote:
echo   %OUT_HEX%
