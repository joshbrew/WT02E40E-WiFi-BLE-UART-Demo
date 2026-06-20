@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Same as the main flash helper, but forces the legacy HLA ST-Link config.
rem Use this only if interface/stlink-dap.cfg is missing or your ST-Link setup
rem refuses DAP mode.

set "SCRIPT_DIR=%~dp0"
set "OPENOCD=openocd"

if "%~1"=="" (
    set "BUILD_DIR=C:\ncs\v3.3.1\nrf\samples\wifi\sta\build"
) else (
    set "BUILD_DIR=%~1"
)

set "CFG=%SCRIPT_DIR%wt02e40e_nrf5340_stlink_legacy_hla.cfg"

set "APP_HEX="
if exist "%BUILD_DIR%\sta\zephyr\merged.hex" set "APP_HEX=%BUILD_DIR%\sta\zephyr\merged.hex"
if not defined APP_HEX if exist "%BUILD_DIR%\sta\zephyr\zephyr.hex" set "APP_HEX=%BUILD_DIR%\sta\zephyr\zephyr.hex"

set "NET_HEX="
if exist "%BUILD_DIR%\hci_ipc\zephyr\merged_CPUNET.hex" set "NET_HEX=%BUILD_DIR%\hci_ipc\zephyr\merged_CPUNET.hex"
if not defined NET_HEX if exist "%BUILD_DIR%\hci_ipc\merged_CPUNET.hex" set "NET_HEX=%BUILD_DIR%\hci_ipc\merged_CPUNET.hex"
if not defined NET_HEX if exist "%BUILD_DIR%\hci_ipc\zephyr\zephyr.hex" set "NET_HEX=%BUILD_DIR%\hci_ipc\zephyr\zephyr.hex"

if not defined APP_HEX (
    echo Missing APP_HEX under %BUILD_DIR%
    exit /b 3
)

if not defined NET_HEX (
    echo Missing NET_HEX under %BUILD_DIR%
    exit /b 4
)

"%OPENOCD%" -f "%CFG%" ^
    -c "init" ^
    -c "targets" ^
    -c "targets nrf53.cpunet" ^
    -c "reset init" ^
    -c "program {%NET_HEX%} verify" ^
    -c "targets nrf53.cpuapp" ^
    -c "reset init" ^
    -c "program {%APP_HEX%} verify" ^
    -c "reset run" ^
    -c "shutdown"

exit /b %ERRORLEVEL%
