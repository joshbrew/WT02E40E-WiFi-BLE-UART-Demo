# WT02E40E Wi-Fi + BLE + UART Command Bring-Up

This project is a bring-up and command bridge application for a WT02E40E module, which combines an nRF5340 host MCU with an nRF7002 Wi-Fi companion.

This was compiled successfully with the nRF Connect Toolchain ver v3.3.1 in VSCode.

The app starts in BLE-only mode by default, exposes a UART/RTT shell, exposes a matching BLE command interface, and can turn Wi-Fi on later when the antenna and credentials are ready. It is based on Nordic's Wi-Fi station sample, but the application code has been split into smaller modules so the behavior is easier to trace.

There's an additional BLE_Webapp you can use for quick testing if the programming was successful. You can server the server.js from Node or host the index.html yourself e.g. with the LiveServer extension on VSCode. Use Chrome for web BLE support.

<img width="500" alt="rn_image_picker_lib_temp_8a1bb018-cece-4d18-a960-d8604912be75" src="https://github.com/user-attachments/assets/7c44a742-f9b0-41f6-8764-3b026d386278" />
<img width="500" alt="image" src="https://github.com/user-attachments/assets/a4b94aed-e316-48fd-9c9b-f96997f97339" />


<!-- WT02E40E_TOC_START -->
## Table of contents

- [What this firmware does](#what-this-firmware-does)
- [Hardware assumptions](#hardware-assumptions)
- [Debug/programming hookup used during bring-up](#debugprogramming-hookup-used-during-bring-up)
- [LEDs](#leds)
- [UART shell pins](#uart-shell-pins)
- [Build and flash](#build-and-flash)
- [Required sysbuild config](#required-sysbuild-config)
- [Default radio mode](#default-radio-mode)
- [BLE self-disable behavior](#ble-self-disable-behavior)
- [Wi-Fi antenna note](#wi-fi-antenna-note)
- [Wi-Fi credentials](#wi-fi-credentials)
- [UART/RTT command shell](#uartrtt-command-shell)
- [BLE service](#ble-service)
- [Payload limits and parser behavior](#payload-limits-and-parser-behavior)
- [Source layout](#source-layout)
- [Important project files](#important-project-files)
- [Troubleshooting](#troubleshooting)
- [Notes before productizing](#notes-before-productizing)
- [BLE_Webapp](#blewebapp)
- [Three-way command bridge](#three-way-command-bridge)
- [Device identity, BLE renaming, and discovery](#device-identity-ble-renaming-and-discovery)
- [Full command feature pass](#full-command-feature-pass)
- [Live UDP command port rebind](#live-udp-command-port-rebind)
- [Wi-Fi AP scanning](#wi-fi-ap-scanning)
- [BLE command execution note](#ble-command-execution-note)
- [TX command quoting note](#tx-command-quoting-note)
- [BLE TX write-path fix](#ble-tx-write-path-fix)
- [Parser fix note](#parser-fix-note)
- [Full command list summary](#full-command-list-summary)
  - [Status, identity, and config](#status-identity-and-config)
  - [Boot behavior](#boot-behavior)
  - [Radio mode switching](#radio-mode-switching)
  - [BLE control](#ble-control)
  - [Wi-Fi control](#wi-fi-control)
  - [Wi-Fi credentials](#wi-fi-credentials)
  - [Wi-Fi AP scanning](#wi-fi-ap-scanning)
  - [Wi-Fi UDP command receive](#wi-fi-udp-command-receive)
  - [Discovery beacon](#discovery-beacon)
  - [Transmit commands](#transmit-commands)
  - [Bridge rules](#bridge-rules)
  - [Ping and latency tests](#ping-and-latency-tests)
  - [Firmware and reboot hooks](#firmware-and-reboot-hooks)
  - [Request IDs](#request-ids)
- [Packet and transport structure](#packet-and-transport-structure)
  - [BLE GATT structure](#ble-gatt-structure)
  - [UART/RTT shell structure](#uartrtt-shell-structure)
  - [Wi-Fi UDP command structure](#wi-fi-udp-command-structure)
  - [Wi-Fi UDP payload TX structure](#wi-fi-udp-payload-tx-structure)
  - [Discovery packet structure](#discovery-packet-structure)
  - [Wi-Fi scan JSON structure](#wi-fi-scan-json-structure)
- [Use case demo command flows](#use-case-demo-command-flows)
  - [1. First BLE sanity test](#1-first-ble-sanity-test)
  - [2. UART to BLE TX test](#2-uart-to-ble-tx-test)
  - [3. BLE to UART test](#3-ble-to-uart-test)
  - [4. BLE to Wi-Fi UDP test](#4-ble-to-wi-fi-udp-test)
  - [5. Wi-Fi command to BLE TX test](#5-wi-fi-command-to-ble-tx-test)
  - [6. Wi-Fi command to UART test](#6-wi-fi-command-to-uart-test)
  - [7. Discover the board on the LAN](#7-discover-the-board-on-the-lan)
  - [8. Scan Wi-Fi networks and connect by index](#8-scan-wi-fi-networks-and-connect-by-index)
  - [9. Rename the BLE device](#9-rename-the-ble-device)
  - [10. Change Wi-Fi command port live](#10-change-wi-fi-command-port-live)
  - [11. Use request IDs](#11-use-request-ids)
  - [12. Safe delayed radio cutoff](#12-safe-delayed-radio-cutoff)
  - [13. Bridge all enabled outputs](#13-bridge-all-enabled-outputs)
  - [14. Reboot safely from BLE or Wi-Fi](#14-reboot-safely-from-ble-or-wi-fi)

<!-- WT02E40E_TOC_END -->


## What this firmware does

- Boots in BLE mode by default.
- Advertises as `WT02E40E-CMD`.
- Provides a custom BLE service with:
  - TX notify characteristic.
  - Status read/notify characteristic.
  - Command write characteristic.
  - Command response read/notify characteristic.
- Provides the same command system over UART and RTT shell.
- Lets the host switch radio mode at runtime: idle, BLE, Wi-Fi, or both.
- Lets the host override Wi-Fi credentials at runtime.
- Scans nearby APs and lets the host select a scanned SSID by index.
- Sends UDP payloads over Wi-Fi when connected.
- Sends payloads over BLE notifications when a client has enabled them.
- Prints UART payloads received from BLE commands for host-side testing.
- Uses two indicator LEDs for basic radio state.

This is a lab bring-up app, not a locked-down product firmware. The BLE command characteristic is intentionally open so it is easy to test with nRF Connect.

## Hardware assumptions

Target module:

```txt
WT02E40E = nRF5340 + nRF7002
```

Build target:

```txt
nrf7002dk/nrf5340/cpuapp
```

The WT02E40E nRF5340-to-nRF7002 internal wiring is close enough to the nRF7002 DK path for this bring-up, so the app uses the nRF7002 DK board target instead of the plain nRF5340 DK target.

Do not build this as:

```txt
nrf5340dk/nrf5340/cpuapp
```

That target does not expose the onboard nRF7002 devicetree node, so `CONFIG_WIFI_NRF70` will be forced off.

## Debug/programming hookup used during bring-up

The working debug hookup was:

```txt
nRF5340 DK debug-out hookup -> WT02E40E carrier

DK VDD       ----+
                +---- DK VTG

DK VDD_nRF   ----------------->  Custom board VDD_NRF
DK GND       ----------------->  Custom board GND
DK SWDIO     ----------------->  Custom board SWDIO_NRF
DK SWDCLK    ----------------->  Custom board SWCLK_NRF
DK RESET     ----------------->  Custom board RESET_NRF
DK SWO       ----------------->  Custom board SWO_NRF  optional
```

The DK still appears as the J-Link programmer in tools. The important part is that the SWD target is routed to the external module instead of the onboard DK nRF5340.

If your custom board is powered from its own regulator instead, do not blindly tie regulators together. In that case, the custom board 3.3 V rail should feed the debug header VTG/VTref sense pin, with common ground and SWD connected.

## LEDs

The carrier overlay maps the indicators as:

```txt
LED0   P1.07
LED1   P0.10
```

Blink meanings:

```txt
LED0 blinking   Firmware alive, BLE active/advertising, no BLE client connected
LED0 solid      BLE client connected
LED1 off       Wi-Fi off or not associated
LED1 blinking  Wi-Fi associated, waiting for IPv4/DHCP
LED1 solid     Wi-Fi connected and IPv4 bound
```

Normal first boot without a BLE client should be:

```txt
LED0 blinking, green off
```

After connecting from a phone with nRF Connect:

```txt
LED0 solid, green off
```

## UART shell pins

The app enables a UART shell on the carrier UART nets:

```txt
nRF P1.04 UART_TX -> host RX net
nRF P1.06 UART_RX -> host TX net
GND               -> host/debug UART GND
Baud              -> 115200 8N1
```

RTT shell is also enabled, so the same commands are available over J-Link RTT if UART is not wired yet.

## Build and flash

Use sysbuild. BLE on the nRF5340 needs a network-core Bluetooth controller image, and sysbuild builds that automatically through `SB_CONFIG_NETCORE_HCI_IPC`.

From the app directory:

```powershell
rmdir /s /q build
west build -b nrf7002dk/nrf5340/cpuapp . --sysbuild --pristine
west flash -d build
```

Useful checks after build:

```powershell
type build\domains.yaml
dir build\hci_ipc\zephyr
```

You should see an `hci_ipc` domain/image for the nRF5340 network core.

If flashing from VS Code, make sure the configuration is:

```txt
Board target: nrf7002dk/nrf5340/cpuapp
Sysbuild: enabled
Kconfig fragments: blank
Devicetree overlays: blank
Snippets: blank
```

## Required sysbuild config

`sysbuild.conf` should contain:

```conf
SB_CONFIG_WIFI_NRF70=y
SB_CONFIG_NETCORE_HCI_IPC=y
```

`SB_CONFIG_WIFI_NRF70` enables the nRF70 Wi-Fi system image support.

`SB_CONFIG_NETCORE_HCI_IPC` builds the nRF5340 network-core BLE controller image.

## Default radio mode

The app defaults to BLE-only mode:

```conf
CONFIG_WT02E40E_DEFAULT_BLE=y
```

This keeps Wi-Fi off until an antenna is installed or the host explicitly enables Wi-Fi.

Other boot modes are available in `Kconfig`:

```conf
CONFIG_WT02E40E_DEFAULT_IDLE=y
CONFIG_WT02E40E_DEFAULT_BLE=y
CONFIG_WT02E40E_DEFAULT_WIFI=y
CONFIG_WT02E40E_DEFAULT_BOTH=y
```

Only one should be selected.


## BLE self-disable behavior

BLE can be enabled or disabled from any command transport:

```text
wt ble on
wt ble off
wt mode ble
wt mode wifi
wt mode idle
wt mode both
```

From UART or RTT, `wt ble off` stops BLE immediately. From the BLE command characteristic, `ble off`, `mode idle`, and `mode wifi` first send a command response and then drop the BLE connection after `WT_BLE_SELF_STOP_DELAY_MS`. This makes it possible to turn BLE off from BLE without losing the acknowledgement packet first.

This means UART can be used as the local supervisory path for both radios:

```text
wt wifi on
wt wifi off
wt ble on
wt ble off
wt mode idle
wt mode ble
wt mode wifi
wt mode both
```

## Wi-Fi antenna note

Do not treat Wi-Fi results as meaningful without the Wi-Fi antenna connected. The nRF7002 may boot and may even scan nearby APs without an antenna, but association and DHCP can fail or be unstable.

Do not repeatedly transmit Wi-Fi traffic for long periods with no antenna or matched load attached.

## Wi-Fi credentials

Compiled fallback credentials live in `prj.conf`:

```conf
CONFIG_WIFI_CREDENTIALS_STATIC_SSID="Myssid"
CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="Mypassword"
```

Runtime credentials can be set from UART, RTT, or BLE command characteristic.

Typical first setup:

```txt
wt wifi cred set testwifi testpassword123
wt wifi on
```

Credential command behavior:

```txt
set      Clear stored runtime credentials and select this SSID
add      Add another stored SSID without clearing the others
open     Store an open network with no password
forget   Delete one stored SSID
clear    Delete all stored runtime credentials
list     Show stored runtime credentials
```

Security argument:

```txt
wt wifi cred set <ssid> <password> [wpa2|auto|wpa3]
```

Default security is WPA2 personal.

## UART/RTT command shell

All UART/RTT commands start with `wt`.

```txt
wt status
wt mode idle
wt mode ble
wt mode wifi
wt mode both

wt wifi on
wt wifi off
wt wifi status
wt wifi reconnect
wt wifi scan
wt wifi scan json
wt wifi scan last json
wt wifi cred list
wt wifi cred set <ssid> <password> [wpa2|auto|wpa3]
wt wifi cred add <ssid> <password> [wpa2|auto|wpa3]
wt wifi cred open <ssid>
wt wifi cred forget <ssid>
wt wifi cred clear

wt ble on
wt ble off
wt ble status

wt tx ble <message>
wt tx uart <message>
wt tx wifi <ipv4> <port> <message>
wt tx both <ipv4> <port> <message>
```

Examples:

```txt
wt status
wt mode both
wt wifi cred set testwifi testpassword123
wt wifi on
wt tx wifi 192.168.1.50 5000 hello over udp
wt tx ble hello over ble notify
wt tx uart hello printed from firmware
wt tx both 192.168.1.50 5000 hello everywhere-ish
```

## BLE service

Advertised name:

```txt
WT02E40E-CMD
```

Custom service:

```txt
Service UUID:             7f1c0001-2b5a-4f2d-9a31-d6a5f4e040e1
TX notify UUID:           7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
Status read/notify UUID:  7f1c0003-2b5a-4f2d-9a31-d6a5f4e040e1
Command write UUID:       7f1c0004-2b5a-4f2d-9a31-d6a5f4e040e1
Command response UUID:    7f1c0005-2b5a-4f2d-9a31-d6a5f4e040e1
```

### Testing with nRF Connect

1. Scan for `WT02E40E-CMD`.
2. Connect.
3. Open service `7f1c0001-2b5a-4f2d-9a31-d6a5f4e040e1`.
4. Read the status characteristic.
5. Enable notifications on the status characteristic for periodic status pings.
6. Enable notifications on the command response characteristic to see command replies.
7. Write UTF-8 text to the command write characteristic.
8. Enable notifications on the TX characteristic if using `tx ble`.

The status characteristic can be read immediately. Example payload:

```txt
WT02E40E status: mode=ble ble=on ready=on conn=on adv=off tx_notify=off status_notify=off cmd_rsp_notify=off wifi_req=off wifi_assoc=off ipv4=off wifi_cmd=on cmd_port=5001 uptime=12s
```

The periodic status ping only runs when:

```txt
BLE client connected
and status notifications enabled
```

No connected client or no status notification subscription means no periodic ping work is scheduled.

### BLE command interface

The BLE command write characteristic accepts the same basic command grammar as the UART/RTT shell, but the leading `wt` prefix is optional.

These are equivalent over BLE:

```txt
status
wt status
```

BLE command examples:

```txt
status
mode ble
mode wifi
mode both
wifi cred set testwifi testpassword123
wifi on
tx uart hello from ble
tx wifi 192.168.1.50 5000 hello over udp from ble
tx ble echo over the BLE TX notify characteristic
tx both 192.168.1.50 5000 send over wifi, uart, and BLE TX if enabled
```

The command response characteristic stores the last response so it can be read manually. Notifications are only sent when the BLE client enables command response notifications.

## Payload limits and parser behavior

Current local limits are in `src/wt_config.h`:

```c
#define WT_TX_PAYLOAD_MAX 255
#define WT_BLE_NOTIFY_APP_PAYLOAD_MAX 255
#define WT_BLE_STATUS_TEXT_MAX 192
#define WT_BLE_CMD_TEXT_MAX 255
#define WT_BLE_CMD_RSP_TEXT_MAX 255
#define WT_BLE_STATUS_PING_MS 5000
#define WT_BLE_ADV_RESTART_DELAY_MS 250
```

BLE TX, status notifications, and command responses use the negotiated ATT MTU dynamically. The app caps payload chunks at 255 bytes, so a 256-byte ATT MTU yields up to 253-byte ATT payload chunks. If a client stays at the default MTU, firmware falls back to 20-byte chunks.

The command parser now supports quoted arguments on the BLE command characteristic and Wi-Fi UDP command socket. UART/RTT shell parsing also accepts quoted arguments through the Zephyr shell. Use quotes for SSIDs, passwords, or positional arguments that contain spaces. Both double quotes and single quotes are supported, with backslash escapes inside quoted text.

Examples:

```txt
wifi cred set "My Home WiFi" "correct horse battery staple" wpa2
wifi cred add 'Lab WiFi' 'password with spaces' auto
wifi cred open "Coffee Shop WiFi"
wifi cred forget "My Home WiFi"
tx ble "hello with spaces"
tx uart "quote test: \"hello\""
tx wifi 192.168.1.50 5000 "payload with spaces"
```

The transmit commands still treat everything after the fixed arguments as payload, so quotes are not required for ordinary payload spaces. Quotes are mainly important for positional fields like SSID and password.

## Source layout

```txt
src/main.c        Boot/init only
src/wt_wifi.c     Wi-Fi prep, connect loop, DHCP events, credentials, UDP TX
src/wt_ble.c      BLE advertising, GATT TX/status/command characteristics
src/wt_shell.c    UART/RTT wt commands
src/wt_radio.c    Radio mode switching: idle, ble, wifi, both
src/wt_leds.c     LED0/LED1 LED status thread
src/wt_common.c   Shared parsing/helpers
src/wt_config.h   App constants and local limits
```

`main.c` should stay boring. New radio behavior should usually go in `wt_radio.c`, Wi-Fi behavior in `wt_wifi.c`, BLE behavior in `wt_ble.c`, and command syntax in `wt_shell.c` or the BLE command parser.

## Important project files

```txt
prj.conf              Main app Kconfig
sysbuild.conf         Wi-Fi and nRF5340 network-core image selection
app.overlay           LEDs, UART pin mapping, and local Wi-Fi MAC fallback
CMakeLists.txt        Source file list
Kconfig               App-specific boot mode settings
WT02E40E_COMMANDS.md  Command quick reference
```

## Troubleshooting

### `CONFIG_WIFI_NRF70 was assigned y, but got n`

The build is not seeing an enabled nRF7002 devicetree node.

Most common causes:

```txt
Wrong board target
Stale build directory
VS Code is building an old configuration
```

Fix:

```powershell
rmdir /s /q build
west build -b nrf7002dk/nrf5340/cpuapp . --sysbuild --pristine
```

### BLE does not advertise

Check that sysbuild created and flashed the network-core image:

```powershell
type build\domains.yaml
dir build\hci_ipc\zephyr
west flash -d build
```

The app should appear in nRF Connect mobile as:

```txt
WT02E40E-CMD
```

Use the nRF Connect scanner, not the phone OS Bluetooth pairing screen.

### LED0 blinks forever

That is normal when BLE is advertising and no BLE client is connected.

If you expected Wi-Fi, run:

```txt
wt mode wifi
wt wifi status
```

or over BLE:

```txt
mode wifi
wifi status
```

### LED1 never lights

LED1 is only for Wi-Fi state. With no Wi-Fi antenna, bad credentials, or Wi-Fi disabled, green may stay off forever.

Expected Wi-Fi LED states:

```txt
LED1 off       Wi-Fi off or not associated
LED1 blinking  Associated, waiting for IPv4
LED1 solid     IPv4 bound
```

### UART shell does not respond

Check:

```txt
115200 8N1
Common ground
Host RX connected to nRF TX P1.04
Host TX connected to nRF RX P1.06
```

RTT shell is available as a backup.

### Logs mention `__log_level` or `__log_current_dynamic_data`

`LOG_MODULE_REGISTER()` must appear before including any headers that use inline logging macros. This project keeps that fixed in `src/main.c` before including `net_private.h`.

If the terminal build succeeds but VS Code Problems still shows these, reset IntelliSense or ignore the stale cpptools view.

## Notes before productizing

- Add authentication or bonding before exposing BLE commands in a real deployment.
- Consider a structured binary/CBOR protocol instead of text commands for a host MCU.
- Verify nRF7002 antenna, regulatory config, MAC address handling, and production credential storage.
- Review power supply burst capacity before doing sustained Wi-Fi TX.


## BLE_Webapp

The project includes a local Node-hosted BLE/Wi-Fi console in `BLE_Webapp/`.

```powershell
cd BLE_Webapp
node server.js
```

Then open:

```text
http://localhost:8080
```

The page connects to the command GATT service over Web Bluetooth and also opens a UDP listener through Node so Wi-Fi `tx wifi <host> <port> <message>` packets can show up in the browser log.

## Three-way command bridge

This build exposes the same high-level control path over three transports:

```text
BLE command characteristic  -> command response characteristic
UART shell                  -> shell output
Wi-Fi UDP command socket     -> UDP response packet
```

Wi-Fi command receive is enabled by default and can be controlled with:

```text
wifi cmd status
wifi cmd on
wifi cmd off
wifi cmd port
wifi cmd port 5002
```

The board listens on UDP port `5001` once Wi-Fi has IPv4 by default. The command socket can be live-rebound at runtime with `wifi cmd port <port>`. The included `BLE_Webapp` Node server listens on UDP port `5000`, sends Wi-Fi command packets to the board, and streams board responses into the browser log.


## Device identity, BLE renaming, and discovery

This build adds small identity/config commands so the webapp and host MCU can identify the board without hardcoded notes.

```text
id
version
config
config json
status json
```

The advertised BLE name can be changed at runtime from any command transport:

```text
ble name
ble name set WT02E40E-CMD-01
name set WT02E40E-CMD-01
```

If the device is advertising, the firmware restarts advertising with the new name. If it is connected, the new name is used for the next advertising session. This is a runtime lab setting, not yet a saved production setting.

Wi-Fi discovery beacons can be enabled once Wi-Fi has IPv4:

```text
discovery status
discovery on
discovery off
```

When enabled, the board broadcasts a small JSON discovery packet to UDP port `5000` every few seconds. The included Node webapp listens on that port and auto-fills the board IP/command port when it sees the packet.

Example discovery payload:

```json
{"type":"wt02e40e_discovery","name":"WT02E40E-CMD-01","fw":"0.3.1-udp-rebind","cmd_port":5001,"udp_rx_port":5000,"uptime_s":42}
```

`config save` persists the runtime BLE name and discovery setting through the Zephyr settings/NVS backend. Wi-Fi credentials persist through the Zephyr Wi-Fi credentials/settings backend. Radio mode is still selected by Kconfig at boot unless changed at runtime.

## Full command feature pass

This build extends the command bridge so the device can be treated as a three-transport control node instead of only a BLE bring-up target.

### Identity and structured status

```text
id
version
config
config json
status
status json
ble status
ble status json
wifi status
wifi status json
fw status
```

`id` and `config` include the advertised BLE name, firmware version, current mode, saved boot mode, Wi-Fi command port, discovery state, bridge settings, Wi-Fi IP/MAC when available, buffer limits, and uptime.

### Saved app config

```text
boot status
boot mode idle
boot mode ble
boot mode wifi
boot mode both
config save
config reset
```

`config save` persists the BLE name, boot mode, discovery setting, Wi-Fi command-server enable state, and bridge settings through Zephyr settings/NVS. Wi-Fi credentials still use the Zephyr Wi-Fi credentials backend.

### BLE renaming

```text
ble name
ble name set "WT02E40E-CMD-01"
name set "WT02E40E-CMD-01"
config save
```

If the device is advertising, changing the BLE name restarts advertising with the new name. If it is connected, the new name is used the next time advertising starts.

### Wi-Fi discovery

```text
discovery status
discovery on
discovery off
```

With Wi-Fi online and IPv4 bound, discovery broadcasts JSON to UDP port `5000`:

```json
{"type":"wt02e40e_discovery","name":"WT02E40E-CMD-01","fw":"0.3.1-udp-rebind","ip":"192.168.1.123","cmd_port":5001,"udp_rx_port":5000,"uptime_s":42}
```

The included `BLE_Webapp` listens for these beacons and auto-fills the board IP and command port.

### Request IDs

Prefix a command with an ID token to make async responses easier to match:

```text
#42 status
#43 wifi status json
```

Responses preserve the prefix:

```text
#42 config name=...
#43 {"wifi_requested":true,...}
```

### Delayed radio safety rails

Commands that cut off their own transport can be delayed:

```text
ble off 5s
wifi off 5s
mode ble 5s
mode wifi 5s
mode idle 5s
```

From BLE, immediate `ble off`, `mode idle`, and `mode wifi` still answer first and then disconnect shortly after so the response can reach the browser/phone.

### Bridge forwarding rules

The bridge layer lets you configure common forwarding outputs once, then send payloads without spelling out every transport each time.

```text
bridge status
bridge status json
bridge target <ip> <port>
bridge ble on
bridge uart on
bridge wifi on
bridge all on
bridge send "hello through enabled bridge outputs"
bridge all "hello through enabled bridge outputs"
config save
```

Defaults:

```text
bridge ble=on
bridge uart=on
bridge wifi=off
bridge target=<unset>:5000
```

`bridge wifi on` requires a bridge target first, usually the Node server IP and UDP listener port.

### Ping and latency tests

```text
ping
ping uart
ping ble
ping wifi <ip> <port>
```

`ping ble` sends a BLE TX notification, so the BLE client must enable TX notifications first. `ping wifi` sends a UDP packet to the requested host/port.

### Firmware/reboot hooks

```text
fw status
fw reboot
fw reboot 5s
fw reboot bootloader 5s
reboot
reboot 5s
```

`fw reboot bootloader` is a placeholder hook in this bring-up build. It logs that bootloader reboot was requested and currently falls back to a cold reboot.

### Three-way command bridge summary

```text
BLE command characteristic  -> command response characteristic
UART/RTT shell              -> shell output
Wi-Fi UDP command socket     -> UDP response packet
```

All three command paths share the same high-level grammar for identity, config, boot mode, BLE, Wi-Fi, discovery, bridge, ping, transmit, and firmware commands.


## Live UDP command port rebind

The Wi-Fi command listener port can be changed at runtime from UART, BLE, or the existing Wi-Fi command path:

```text
wifi cmd port          # show current command port
wifi cmd port 5002     # rebind listener to UDP/5002
config save            # persist across reboot
```

When the port changes, the command thread closes the old UDP socket and binds the new one on the next poll cycle. If the command was sent over Wi-Fi, the response is sent from the old socket first, then the listener moves to the new port. Discovery packets report the current `cmd_port`, and the Node webapp updates its board command port field from discovery.


## Wi-Fi AP scanning

This build can scan nearby Wi-Fi access points from UART, BLE, or the Wi-Fi UDP command path. Zephyr exposes scan requests through `NET_REQUEST_WIFI_SCAN` and returns individual scan-result events, which this app stores as a small latest-results table.

```text
wifi scan                         # start scan, wait, return compact text
wifi scan json                    # start scan, wait, return JSON
wifi scan start                   # start scan asynchronously
wifi scan last                    # read previous scan as text
wifi scan last json               # read previous scan as JSON
wifi scan status                  # running/valid/count state
wifi scan clear                   # clear cached scan results
wifi scan connect <index> <password> [wpa2|auto|wpa3]
wifi scan open <index>
```

Example flow from BLE or UART:

```text
wifi scan json
wifi scan connect 1 "my password with spaces" wpa2
wifi on
wifi status
```

The included `BLE_Webapp` now has a scan helper that can populate the SSID field from scan results.


## BLE command execution note

BLE command writes are queued onto the Zephyr workqueue before execution. This keeps command handling and notifications out of the Bluetooth GATT write callback, which avoids lockups when a command immediately emits BLE TX notifications and command responses.


## TX command quoting note

TX payload examples in the webapp use quoted payloads now:

```text
tx ble "hello from the web console"
tx uart "hello from BLE command console"
tx wifi 192.168.1.50 5000 "hello over UDP"
```

The firmware joins remaining TX arguments into a payload, but quoting is still the safest form when sending through Web Bluetooth or copying commands between BLE, UART, and Wi-Fi.


## BLE TX write-path fix

Firmware version `0.3.3-ble-tx-writefix` debounces BLE command writes for a few milliseconds before dispatching. This keeps Web Bluetooth long/offset writes from executing a partial command like `tx ble` before the payload fragment arrives.


## Parser fix note

Firmware `0.3.4-parserfix` fixes the quoted-argument tokenizer so multi-word commands over BLE and Wi-Fi UDP parse correctly. This specifically fixes commands like:

```text
tx ble "hello from the web console"
wifi cred set "My Home WiFi" "password with spaces" wpa2
```

## Full command list summary

The command grammar is intentionally text-based so the same commands can be sent from UART/RTT, BLE, or Wi-Fi UDP.

Transport prefix rules:

```txt
UART/RTT shell:
  wt <command> [args...]

BLE command characteristic:
  <command> [args...]
  wt <command> [args...] is also accepted

Wi-Fi UDP command socket:
  <command> [args...]
  wt <command> [args...] is also accepted
```

Quoted arguments are supported on BLE and Wi-Fi command paths, and Zephyr shell quoting works on UART/RTT:

```txt
wifi cred set "My Home WiFi" "password with spaces" wpa2
tx ble "hello from uart"
tx wifi 192.168.1.50 5000 "payload with spaces"
```

Supported escape sequences inside quotes:

```txt
\"    literal quote
\\    literal backslash
\n    newline
\r    carriage return
\t    tab
```

### Status, identity, and config

```txt
status
status json

id
version

config
config json
config save
config reset
```

`status` gives the compact operational state. `status json` is meant for tools/web UIs. `config save` persists runtime configuration such as BLE name, discovery state, boot mode, and Wi-Fi command port. `config reset` restores runtime config defaults.

### Boot behavior

```txt
boot status
boot mode idle
boot mode ble
boot mode wifi
boot mode both
```

Boot mode controls which radio mode the app enters after reset. Use `config save` after changing boot mode if it should persist.

### Radio mode switching

```txt
mode idle
mode ble
mode wifi
mode both

mode idle 5s
mode ble 5s
mode wifi 5s
mode both 5s
```

The delayed forms are useful when the command would cut off the current control path. For example, `mode wifi 5s` sent from BLE gives the BLE response time to get back before BLE disconnects.

### BLE control

```txt
ble on
ble off
ble off 5s
ble status
ble status json

ble name
ble name get
ble name set <name>

name set <name>
```

`ble off` can be sent from BLE. When sent from BLE, the response is sent first, then BLE disconnect/stop is scheduled. The new BLE name is used on the next advertising cycle. Use `config save` to persist the name across reset.

### Wi-Fi control

```txt
wifi on
wifi off
wifi off 5s
wifi status
wifi status json
wifi reconnect
```

Wi-Fi stays off by default unless the selected boot mode or a command enables it.

### Wi-Fi credentials

```txt
wifi cred list
wifi cred set <ssid> <password> [wpa2|auto|wpa3]
wifi cred add <ssid> <password> [wpa2|auto|wpa3]
wifi cred open <ssid>
wifi cred forget <ssid>
wifi cred clear
```

`set` clears stored runtime credentials and selects a new secured network. `add` adds another network. `open` stores/selects an open network. `clear` removes runtime credentials but does not remove compiled fallback credentials from the firmware image.

### Wi-Fi AP scanning

```txt
wifi scan
wifi scan json
wifi scan start
wifi scan wait
wifi scan wait json
wifi scan last
wifi scan last json
wifi scan status
wifi scan clear
wifi scan connect <index> <password> [wpa2|auto|wpa3]
wifi scan open <index>
```

`wifi scan json` starts a scan and returns structured results. `wifi scan last` reports the last stored result list without starting a new scan. `wifi scan connect` uses an index from the last scan result list to set credentials.

### Wi-Fi UDP command receive

```txt
wifi cmd status
wifi cmd on
wifi cmd off
wifi cmd port
wifi cmd port <port>
```

The board command socket defaults to UDP port `5001`. `wifi cmd port <port>` live-rebinds the listener. If sent over Wi-Fi, the board responds from the old socket first, then rebinds on the next worker cycle. Use `config save` to persist the new port.

### Discovery beacon

```txt
discovery on
discovery off
discovery status
```

Discovery is LAN discovery, not Wi-Fi AP scanning. When enabled and Wi-Fi has IPv4, the board broadcasts a small JSON packet so the Node webapp can auto-fill board IP, command port, name, and firmware version.

### Transmit commands

```txt
tx ble <message>
tx uart <message>
tx wifi <ipv4> <port> <message>
tx both <ipv4> <port> <message>
```

`tx ble` sends to the BLE TX notify characteristic. A BLE client must be connected and subscribed to TX notifications. `tx uart` writes to the firmware UART TX path. `tx wifi` sends a UDP payload over Wi-Fi. `tx both` uses the combined transmit path.

### Bridge rules

```txt
bridge status
bridge status json

bridge target <ip> <port>

bridge ble on
bridge ble off
bridge uart on
bridge uart off
bridge wifi on
bridge wifi off
bridge all on
bridge all off

bridge send <message>
bridge all <message>
```

Bridge rules control which transports are used when forwarding generic bridge messages. `bridge target` sets the Wi-Fi destination used by bridge Wi-Fi output.

### Ping and latency tests

```txt
ping
ping uart
ping ble
ping wifi <ip> <port>
```

These are quick path checks. `ping ble` requires BLE TX notifications to be enabled. `ping wifi` sends a UDP ping payload to the selected IP/port.

### Firmware and reboot hooks

```txt
fw status
fw reboot
fw reboot 5s
fw reboot bootloader
fw reboot bootloader 5s

reboot
reboot 5s
```

Use delayed reboot forms over BLE/Wi-Fi so the response can leave before reset.

### Request IDs

Any command can optionally start with a request ID:

```txt
#42 status
#43 wifi status json
#44 tx ble "hello"
```

Responses preserve the ID:

```txt
#42 ok ...
#43 ok ...
#44 ok ble tx 5 bytes
```

This is useful for tools that may have multiple commands in flight or multiple transports active.

## Packet and transport structure

### BLE GATT structure

BLE uses one custom service with five characteristics:

```txt
Service UUID:             7f1c0001-2b5a-4f2d-9a31-d6a5f4e040e1

TX notify UUID:           7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
Status read/notify UUID:  7f1c0003-2b5a-4f2d-9a31-d6a5f4e040e1
Command write UUID:       7f1c0004-2b5a-4f2d-9a31-d6a5f4e040e1
Command response UUID:    7f1c0005-2b5a-4f2d-9a31-d6a5f4e040e1
```

BLE command packet:

```txt
Client -> Command write characteristic

UTF-8 text command
No binary framing required
Trailing newline optional
Example:
  tx ble "hello from ble"
```

BLE response packet:

```txt
Board -> Command response characteristic

UTF-8 text response
Readable as last-response value
Notify when command response notifications are enabled
Example:
  ok ble tx 14 bytes
```

BLE TX packet:

```txt
Board -> TX notify characteristic

UTF-8 payload
Notify only
Used by:
  tx ble <message>
  bridge ble output
  ping ble
```

BLE status packet:

```txt
Board -> Status read/notify characteristic

UTF-8 status line or status JSON
Read manually any time
Periodic notification only when status notifications are enabled
```

BLE payload sizing:

```txt
ATT payload = negotiated ATT MTU - 3
App-level BLE chunks are capped at 255 bytes
Default fallback payload is 20 bytes if MTU stays at 23 or notify rejects the larger packet
```

### UART/RTT shell structure

UART and RTT use Zephyr shell syntax:

```txt
Host -> board:
  wt <command> [args...]

Board -> host:
  shell/log output
```

UART parameters:

```txt
115200 baud
8 data bits
No parity
1 stop bit
Common ground required
```

UART shell commands require the `wt` root prefix:

```txt
wt status
wt tx ble "hello from uart"
wt wifi status
```

### Wi-Fi UDP command structure

Board command receive socket:

```txt
Node/webapp/host -> WT02E40E
UDP destination port: default 5001, configurable with wifi cmd port <port>
Payload: UTF-8 text command
```

Example command packet payload:

```txt
#42 status json
```

Board command response:

```txt
WT02E40E -> sender IP/port
UDP payload: UTF-8 text response
```

Example response packet payload:

```txt
#42 {"ok":true,"name":"WT02E40E-CMD","mode":"both",...}
```

Wi-Fi UDP command packets are intentionally simple and unauthenticated for bring-up. Do not expose this command port on an untrusted LAN without adding authentication or pairing logic.

### Wi-Fi UDP payload TX structure

The `tx wifi` command sends arbitrary text to a target UDP host/port:

```txt
Command:
  tx wifi <ipv4> <port> <message>

Packet:
  Board -> <ipv4>:<port>
  UDP payload = <message> as UTF-8 bytes
```

Example:

```txt
tx wifi 192.168.1.50 5000 "hello over udp"
```

### Discovery packet structure

When discovery is enabled, the board broadcasts a JSON packet to the Node/webapp UDP listener port.

Default destination:

```txt
UDP broadcast port: 5000
```

Example discovery payload:

```json
{
  "type": "wt02e40e_discovery",
  "name": "WT02E40E-CMD",
  "fw": "0.3.5-parserfix2",
  "cmd_port": 5001,
  "udp_rx_port": 5000,
  "uptime_s": 42
}
```

The sender IP from the UDP packet is the board IP. The webapp uses that sender IP plus `cmd_port` to auto-fill the Wi-Fi command panel.

### Wi-Fi scan JSON structure

The JSON scan commands return a list of recent APs. Shape may be compact to fit command buffers, but conceptually:

```json
{
  "ok": true,
  "scan": {
    "running": false,
    "count": 3,
    "results": [
      {
        "index": 1,
        "ssid": "My Home WiFi",
        "rssi": -44,
        "channel": 6,
        "security": "wpa2",
        "bssid": "aa:bb:cc:dd:ee:ff"
      }
    ]
  }
}
```

The `index` field is what `wifi scan connect <index> ...` and `wifi scan open <index>` use.

## Use case demo command flows

These are meant as quick bring-up demos for the README. Replace IPs, SSIDs, and passwords with local values.

### 1. First BLE sanity test

```txt
status
ble status
config
```

Expected:

```txt
BLE connected
mode=ble
wifi_req=off
```

### 2. UART to BLE TX test

This sends a message from UART/RTT and receives it on a connected BLE client.

On the BLE client, enable notifications on:

```txt
TX notify UUID:
7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
```

From UART/RTT:

```txt
wt tx ble "hello from uart"
```

Expected BLE/webapp log:

```txt
TX <= hello from uart
```

If the command returns `-13` or says TX notify is off, the BLE client is connected but has not enabled TX notifications.

### 3. BLE to UART test

From BLE command write:

```txt
tx uart "hello from ble"
```

Expected command response:

```txt
ok uart tx ...
```

Expected UART/log-side output:

```txt
hello from ble
```

### 4. BLE to Wi-Fi UDP test

Start the Node webapp:

```powershell
cd BLE_Webapp
node server.js
```

Open:

```txt
http://localhost:8080
```

Connect BLE, then send:

```txt
mode both
wifi cred set "Your SSID" "Your Password" wpa2
wifi on
wifi status
```

After IPv4 is bound, send a UDP packet to the Node listener:

```txt
tx wifi 192.168.1.50 5000 "hello from board over udp"
```

Expected webapp log:

```txt
UDP <= <board-ip>:<port> "hello from board over udp"
```

### 5. Wi-Fi command to BLE TX test

With BLE connected and TX notifications enabled:

```txt
mode both
wifi cmd on
wifi status
```

In the webapp Wi-Fi command panel, send to the board IP/command port:

```txt
tx ble "hello from wifi command path"
```

Expected BLE log:

```txt
TX <= hello from wifi command path
```

Expected Wi-Fi command response:

```txt
ok ble tx ...
```

### 6. Wi-Fi command to UART test

In the webapp Wi-Fi command panel:

```txt
tx uart "hello from wifi command path"
```

Expected UART/log-side output:

```txt
hello from wifi command path
```

### 7. Discover the board on the LAN

From BLE, UART, or Wi-Fi command:

```txt
discovery on
config save
```

Once Wi-Fi has IPv4, the board broadcasts discovery JSON. The webapp should auto-fill:

```txt
Board IP
Board command port
Device name
Firmware/version field
```

### 8. Scan Wi-Fi networks and connect by index

From BLE or UART:

```txt
wifi scan json
```

Pick an index from the result list, then connect:

```txt
wifi scan connect 1 "your password" wpa2
wifi on
wifi status
```

For an open AP:

```txt
wifi scan open 1
wifi on
wifi status
```

### 9. Rename the BLE device

```txt
ble name set WT02E40E-CMD-01
config save
```

If already connected, the current connection keeps working. The new name appears on the next advertising cycle.

### 10. Change Wi-Fi command port live

```txt
wifi cmd port
wifi cmd port 5002
config save
```

If the command was sent over Wi-Fi, the response is sent from the old port first, then the board rebinds to the new port.

### 11. Use request IDs

```txt
#42 status
#43 wifi status json
#44 tx ble "hello with an ID"
```

Expected response style:

```txt
#42 ok ...
#43 {"ok":true,...}
#44 ok ble tx ...
```

### 12. Safe delayed radio cutoff

From BLE:

```txt
ble off 5s
```

Expected:

```txt
ok ble off scheduled 5000 ms
```

Then the BLE link drops after the delay.

From Wi-Fi:

```txt
wifi off 5s
```

Expected:

```txt
ok wifi off scheduled 5000 ms
```

Then the Wi-Fi command path disappears after the delay.

### 13. Bridge all enabled outputs

Set a Wi-Fi bridge target:

```txt
bridge target 192.168.1.50 5000
bridge all on
bridge status
```

Send a bridge message:

```txt
bridge send "hello across enabled bridge outputs"
```

Expected:

```txt
BLE TX notification if subscribed
UART output
UDP packet to bridge target if Wi-Fi is connected
```

### 14. Reboot safely from BLE or Wi-Fi

```txt
fw status
fw reboot 5s
```

The delayed form gives the response time to reach the client before the board resets.
