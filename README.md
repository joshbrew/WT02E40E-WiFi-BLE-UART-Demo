# WT02E40E Wi-Fi + BLE + UART Command Bridge

This project is a bring-up and command bridge application for the WT02E40E module, which combines an nRF5340 host MCU with an nRF7002 Wi-Fi companion.

The firmware starts in BLE command mode by default. Wi-Fi stays off until a command asks for it, or until a scan temporarily powers it for the scan. The same command system is exposed through BLE, UART/RTT shell, and UDP once Wi-Fi has an IPv4 address.

The repository also includes `BLE_Webapp/`, a Chrome/Edge Web Bluetooth console with a small Node helper for UDP testing. It is useful for quick checks after flashing, scan testing, BLE TX notification testing, and Wi-Fi command bridge testing.

<img width="500" alt="WT02E40E bring-up board" src="https://github.com/user-attachments/assets/7c44a742-f9b0-41f6-8764-3b026d386278" />
<img width="500" alt="WT02E40E web console" src="https://github.com/user-attachments/assets/a4b94aed-e316-48fd-9c9b-f96997f97339" />

## Table of contents

- [What this firmware does](#what-this-firmware-does)
- [Hardware assumptions](#hardware-assumptions)
- [Debug/programming hookup used during bring-up](#debugprogramming-hookup-used-during-bring-up)
- [UART shell pins](#uart-shell-pins)
- [Build and flash](#build-and-flash)
- [Required sysbuild config](#required-sysbuild-config)
- [Default radio behavior](#default-radio-behavior)
- [LED behavior](#led-behavior)
- [Wi-Fi antenna note](#wi-fi-antenna-note)
- [BLE service](#ble-service)
- [BLE_Webapp](#blewebapp)
- [Command transports](#command-transports)
- [Payload size and stream framing](#payload-size-and-stream-framing)
- [Outbound queues](#outbound-queues)
- [Wi-Fi behavior](#wi-fi-behavior)
- [Wi-Fi credentials](#wi-fi-credentials)
- [Wi-Fi AP scanning](#wi-fi-ap-scanning)
- [Wi-Fi UDP command path](#wi-fi-udp-command-path)
- [Discovery beacons](#discovery-beacons)
- [Bridge forwarding](#bridge-forwarding)
- [Device identity, BLE name, and saved config](#device-identity-ble-name-and-saved-config)
- [Command list summary](#command-list-summary)
- [Packet and transport structure](#packet-and-transport-structure)
- [Use case demo command flows](#use-case-demo-command-flows)
- [Source layout](#source-layout)
- [Important project files](#important-project-files)
- [Troubleshooting](#troubleshooting)
- [Notes before productizing](#notes-before-productizing)

## What this firmware does

- Boots into BLE command mode by default.
- Advertises as `WT02E40E-CMD`, unless renamed.
- Keeps Wi-Fi off at boot unless the saved boot mode says otherwise.
- Provides a custom BLE service with:
  - TX notify characteristic.
  - Status read/notify characteristic.
  - Command write characteristic.
  - Command response read/notify characteristic.
- Provides the same command system over UART and J-Link RTT shell.
- Provides the same command system over UDP when Wi-Fi is online.
- Lets a host switch radio state at runtime: idle, BLE, Wi-Fi, or both.
- Lets a host add, remove, list, and select Wi-Fi credentials at runtime.
- Scans nearby Wi-Fi APs and lets a host connect by scan-result index.
- Sends UDP payloads over Wi-Fi when connected.
- Sends payloads over BLE TX notifications when a BLE client has subscribed.
- Forwards command traffic across BLE, UART, and Wi-Fi when bridge outputs are enabled.
- Uses LED indicators for BLE state, Wi-Fi state, bridge activity, and alert pulses.

This is lab bring-up firmware. The BLE command characteristic and Wi-Fi UDP command socket are intentionally easy to use. Add authentication, bonding, or a signed command protocol before using this on an untrusted network.

## Hardware assumptions

Target module:

```txt
WT02E40E = nRF5340 + nRF7002
```

Build target:

```txt
nrf7002dk/nrf5340/cpuapp
```

The WT02E40E nRF5340-to-nRF7002 wiring is close enough to the nRF7002 DK path for this bring-up, so the app uses the nRF7002 DK board target instead of the plain nRF5340 DK target.

Use this target:

```txt
nrf7002dk/nrf5340/cpuapp
```

Avoid this target for this app:

```txt
nrf5340dk/nrf5340/cpuapp
```

The plain nRF5340 DK target does not expose the nRF7002 devicetree node used by the Wi-Fi driver, so `CONFIG_WIFI_NRF70` will not come up correctly.

## Debug/programming hookup used during bring-up

The working debug-out hookup from an nRF5340 DK to the WT02E40E carrier was:

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

The DK still appears as the J-Link programmer. The important part is that SWD is routed to the external WT02E40E target instead of the onboard DK nRF5340.

If the custom board is self-powered, use common ground and feed the custom board 3.3 V rail into the debugger VTG/VTref sense pin. Avoid tying two active regulators together.

## UART shell pins

The app enables a UART shell on the carrier UART nets:

```txt
nRF P1.04 UART_TX -> host RX net
nRF P1.06 UART_RX -> host TX net
GND               -> host/debug UART GND
Baud              -> 115200 8N1
```

RTT shell is also enabled through J-Link, so the same commands are available over RTT when UART is not wired.

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

If flashing from VS Code, use:

```txt
Board target: nrf7002dk/nrf5340/cpuapp
Sysbuild: enabled
Kconfig fragments: blank, unless you intentionally added one
Devicetree overlays: blank, unless you intentionally added one
Snippets: blank
```

A VS Code Problems warning like “Bluetooth subsystem must be enabled by CONFIG_BT” can appear when cpptools inspects `wt_ble.c` outside the real sysbuild context. Trust the terminal build. If `west build` succeeds, the editor warning is stale analysis.

## Required sysbuild config

`sysbuild.conf` should contain:

```conf
SB_CONFIG_WIFI_NRF70=y
SB_CONFIG_NETCORE_HCI_IPC=y
```

`SB_CONFIG_WIFI_NRF70` enables the nRF70 Wi-Fi support.

`SB_CONFIG_NETCORE_HCI_IPC` builds the nRF5340 network-core BLE controller image.

`prj.conf` selects BLE boot mode by default:

```conf
CONFIG_WT02E40E_DEFAULT_BLE=y
```

## Default radio behavior

Default first boot behavior:

```txt
BLE on
Wi-Fi off
UDP command service flag off until enabled or scan/connection path uses it
Discovery off unless saved config enables it
```

The default mode comes from Kconfig:

```conf
CONFIG_WT02E40E_DEFAULT_BLE=y
```

Other boot modes are available in `Kconfig`:

```conf
CONFIG_WT02E40E_DEFAULT_IDLE=y
CONFIG_WT02E40E_DEFAULT_BLE=y
CONFIG_WT02E40E_DEFAULT_WIFI=y
CONFIG_WT02E40E_DEFAULT_BOTH=y
```

Only one should be selected.

Runtime modes:

```txt
mode idle
mode ble
mode wifi
mode both
```

Radio commands that cut off their own transport answer first, then perform the cutoff after a short delay. For example, `ble off` sent from BLE sends the response before the BLE link drops.

### `wifi on` and `wifi reconnect`

`wifi on` means “prepare/enable the Wi-Fi interface.” It does not immediately start an association attempt using stored credentials.

`wifi reconnect` means “connect or reconnect using stored credentials.”

This split keeps scan behavior cleaner. You can power the interface without accidentally starting a connection attempt while scan is trying to run.

Common patterns:

```txt
wifi on
wifi status
```

```txt
wifi on
wifi reconnect
wifi status
```

```txt
wifi scan json
wifi scan connect 1 "wifi password" wpa2
wifi reconnect
wifi status
```

## LED behavior

The carrier overlay maps the primary indicators as:

```txt
LED0   P1.07   BLE state
LED1   P0.10   Wi-Fi state
```

Optional devicetree LED aliases may also be used:

```txt
LED2   bridge / TX / discovery activity pulse
LED3   alert / error pulse
```

Runtime meanings:

```txt
LED0 solid          BLE client connected
LED0 short off dip  BLE command response or BLE TX notify queued to host
LED0 medium blink   BLE advertising, no connected client
LED0 heartbeat      BLE ready and idle
LED0 slow blink     BLE requested before ready

LED1 off            Wi-Fi not requested
LED1 slow blink     Wi-Fi requested or interface preparing
LED1 fast blink     Wi-Fi scan pending/running
LED1 double pulse   Wi-Fi associated, waiting for IPv4/DHCP
LED1 solid          Wi-Fi associated and IPv4 bound

LED2 pulse          Bridge, UDP TX, discovery, or command activity if LED2 exists
LED3 pulse          Alert/error activity if LED3 exists
```

Manual indicator commands:

```txt
led status
led test all
led test ble
led test wifi
led pulse ble
led pulse wifi
led pulse activity
led pulse alert
```

The `led test ...` commands are simple user-input checks. They prove the firmware can drive the mapped LED pins from the command system. The runtime LED behavior still follows the radio and transport state machine.

## Wi-Fi antenna note

Do not treat Wi-Fi results as meaningful without the Wi-Fi antenna connected. The nRF7002 may boot and may even scan nearby APs without an antenna, but association and DHCP can fail or look unstable.

Avoid sustained Wi-Fi transmission with no antenna or matched load attached.

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
5. Enable notifications on the command response characteristic.
6. Enable notifications on the TX characteristic if using `tx ble`, `bridge ble`, or `ping ble`.
7. Enable notifications on the status characteristic only when you want periodic status pings.
8. Write UTF-8 text commands to the command write characteristic.

The command response characteristic stores the last short response so it can be read manually. Notifications are the preferred path for command responses because streaming responses are delivered as notifications.

The periodic status ping only runs when:

```txt
BLE client connected
and status notifications enabled
```

Status pings do not trigger the BLE connected LED off-dip. Command responses and TX notifications do, so UART-driven commands such as `tx ble j18_blink` produce a visible report indicator when BLE actually sends to the connected host.

## BLE_Webapp

The project includes a local Web Bluetooth console in `BLE_Webapp/`.

Run the Node helper:

```powershell
cd BLE_Webapp
node server.js
```

Then open:

```txt
http://localhost:8080
```

Use Chrome or Edge for Web Bluetooth support.

The webapp provides:

```txt
BLE connect/disconnect
Command entry
Grouped quick commands
Wi-Fi on/off/reconnect controls
Wi-Fi scan and scan-result item pulls
Credential helpers
Status pills for BLE, Wi-Fi, IPv4, UDP command, discovery, and scan state
Indicator test buttons
UDP listener for board-to-host packets
Wi-Fi UDP command sender through the Node helper
```

The webapp queues outgoing BLE commands, Wi-Fi command packets, and raw UDP sends. This matters because browsers usually allow only one active GATT operation at a time. The UI sends scan item requests one at a time instead of blasting them into the BLE stack.

## Command transports

The same high-level commands are available from three places:

```txt
BLE command characteristic  -> command response characteristic
UART/RTT shell              -> shell output
Wi-Fi UDP command socket     -> UDP response packet
```

Command prefix rules:

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

Examples:

```txt
wt status
wt wifi scan json
wt tx ble "hello from uart"
```

```txt
status
wifi scan json
tx uart "hello from ble"
```

Quoted arguments are supported on BLE and Wi-Fi command paths. Zephyr shell quoting works on UART/RTT.

Use quotes for SSIDs, passwords, names, and payloads with spaces:

```txt
wifi cred set "My Home WiFi" "correct horse battery staple" wpa2
wifi cred add "Lab WiFi" "password with spaces" auto
wifi cred open "Coffee Shop WiFi"
wifi cred forget "My Home WiFi"
tx ble "hello with spaces"
tx uart "quote test: \"hello\""
tx wifi 192.168.1.50 5000 "payload with spaces"
```

Supported escapes inside quoted BLE and UDP command arguments:

```txt
\"    literal quote
\\    literal backslash
\n    newline
\r    carriage return
\t    tab
```

## Payload size and stream framing

The firmware keeps normal command messages small because BLE packets and microcontroller RAM are both limited.

Current local limits are in `src/wt_config.h`:

```c
#define WT_TX_PAYLOAD_MAX 244
#define WT_BLE_NOTIFY_APP_PAYLOAD_MAX 244
#define WT_BLE_STATUS_TEXT_MAX 250
#define WT_BLE_CMD_TEXT_MAX 255
#define WT_BLE_CMD_RSP_TEXT_MAX 250
#define WT_WIFI_CMD_RX_TEXT_MAX 255
#define WT_WIFI_CMD_RSP_TEXT_MAX 250
#define WT_WIFI_DISCOVERY_PAYLOAD_MAX 250
#define WT_WIFI_SCAN_MAX_RESULTS 10
#define WT_WIFI_SCAN_TIMEOUT_MS 12000
```

Plain-language version:

```txt
Commands can be up to about 255 bytes.
Normal responses are kept under about 250 bytes.
BLE notification payloads are capped below 255 bytes.
Big responses are split into framed chunks instead of requiring one big buffer.
```

BLE ATT also has its own MTU. The usable ATT payload is:

```txt
ATT payload = negotiated ATT MTU - 3 bytes
```

If the client negotiates a large MTU, this app still caps its own frame payload to stay under the local 244 byte BLE payload budget. If the client stays at the default MTU, the BLE flusher sends smaller pieces that fit the active connection.

### Stream frame format

Long command responses and bridge payloads use text frames:

```txt
~S001
~C001000<payload>
~C001001<payload>
~E001002
```

Frame meaning:

```txt
~S001                 start stream id 001
~C001000<payload>     chunk for stream 001, chunk index 000
~C001001<payload>     chunk for stream 001, chunk index 001
~E001002              end stream 001, total chunks 002
```

Field sizes:

```txt
~S, ~C, ~E   frame marker
001          3-digit stream id
000          3-digit chunk index
002          3-digit final chunk count
```

The receiver reassembles all `~C` payload pieces between `~S` and `~E`. The webapp does this for:

```txt
BLE command responses
BLE TX notifications
Wi-Fi UDP command responses
raw UDP stream packets
```

Short replies skip this framing and arrive as normal text.

Example short response:

```txt
ok wifi on
```

Example streamed command response after reassembly:

```json
{"type":"wifi_scan_full","count":6,"results":[...]}
```

Request IDs still work. If a command starts with `#42`, the reassembled command response keeps that ID in the payload:

```txt
#42 status json
```

Response shape:

```txt
#42 {"ok":true,...}
```

## Outbound queues

Each transport has a flush queue. Code that wants to send data pushes into the queue, and one flusher owns the actual radio or characteristic send.

Firmware-side queues:

```txt
BLE command response queue -> command response characteristic
BLE TX queue               -> TX notify characteristic
Wi-Fi UDP response path     -> UDP response packets
Wi-Fi UDP TX path           -> UDP payload packets
```

Browser-side queues:

```txt
BLE command write queue
Wi-Fi command UDP send queue
raw UDP send queue
```

This avoids overlapping GATT operations and keeps bursts ordered:

```txt
producer -> transport queue -> one flusher -> radio/characteristic
```

The queue also carries stream frames, so a long streamed response is sent as ordered `~S`, `~C`, and `~E` frames.

## Wi-Fi behavior

Wi-Fi is off by default in BLE boot mode. The key commands are:

```txt
wifi on          prepare/enable the Wi-Fi interface
wifi reconnect   associate using stored credentials
wifi off         release Wi-Fi request and bring the Wi-Fi side down
wifi status      show Wi-Fi state
```

Scan can run without manually pressing `wifi on` first:

```txt
wifi scan json
```

If Wi-Fi is off, scan temporarily powers Wi-Fi, waits for the interface to settle, scans, then releases the temporary Wi-Fi request after the scan finishes. The Wi-Fi LED fast-blinks during scan and returns to off afterward.

If Wi-Fi was already explicitly on, scan leaves Wi-Fi on afterward.

This is the recommended simple scan path from BLE boot mode:

```txt
wifi scan json
wifi scan last json
wifi scan item 1 json
wifi scan item 2 json
```

## Wi-Fi credentials

Compiled fallback credentials can live in `prj.conf`:

```conf
CONFIG_WIFI_CREDENTIALS_STATIC_SSID="Myssid"
CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="Mypassword"
```

Runtime credential commands:

```txt
wifi cred list
wifi cred set <ssid> <password> [wpa2|auto|wpa3]
wifi cred add <ssid> <password> [wpa2|auto|wpa3]
wifi cred open <ssid>
wifi cred forget <ssid>
wifi cred clear
```

Behavior:

```txt
set      Clear stored runtime credentials and select this SSID
add      Add another stored SSID without clearing the others
open     Store/select an open network with no password
forget   Delete one stored SSID
clear    Delete all stored runtime credentials
list     Show stored runtime credentials
```

Default security argument is WPA2 personal.

Examples:

```txt
wifi cred set "My Home WiFi" "password with spaces" wpa2
wifi reconnect
wifi status
```

```txt
wifi cred open "Open Test AP"
wifi reconnect
wifi status
```

## Wi-Fi AP scanning

Scan commands:

```txt
wifi scan
wifi scan json
wifi scan full json
wifi scan start
wifi scan wait
wifi scan wait json
wifi scan last
wifi scan last json
wifi scan item <index>
wifi scan item <index> json
wifi scan status
wifi scan clear
wifi scan connect <index> <password> [wpa2|auto|wpa3]
wifi scan open <index>
```

### BLE-safe scan

`wifi scan json` returns a compact summary that stays under the small BLE command response budget:

```json
{"type":"wifi_scan","running":false,"valid":true,"count":4,"status":0,"age_s":1,"results":[]}
```

Then each AP is fetched separately:

```txt
wifi scan item 1 json
wifi scan item 2 json
wifi scan item 3 json
```

Example item:

```json
{"type":"wifi_scan_item","i":1,"ssid":"ExampleAP","rssi":-48,"channel":6,"band":"2.4GHz","security":"WPA2-PSK","bssid":"aa:bb:cc:dd:ee:ff"}
```

The browser uses this pattern by default because every response stays below the small BLE packet budget.

### Full streamed scan

`wifi scan full json` returns the full list as one logical JSON document using `~S/~C/~E` stream frames:

```txt
wifi scan full json
```

Use this when command response notifications are enabled and the receiver supports stream reassembly.

### Async scan status

The scan driver is asynchronous. A scan can legitimately return a running state first:

```json
{"type":"wifi_scan","running":true,"valid":false,"count":0,"status":-119,"age_s":0,"results":[]}
```

That means the scan was accepted and is still running. The normal follow-up is:

```txt
wifi scan last json
```

### Connect from scan result

Pick a result index, then connect:

```txt
wifi scan connect 1 "wifi password" wpa2
wifi reconnect
wifi status
```

For an open AP:

```txt
wifi scan open 1
wifi reconnect
wifi status
```

## Wi-Fi UDP command path

When Wi-Fi has IPv4 and the command server is enabled, the board listens for UDP command packets.

Defaults:

```txt
Board command port: 5001
Node/webapp receive port: 5000
```

Control commands:

```txt
wifi cmd status
wifi cmd on
wifi cmd off
wifi cmd port
wifi cmd port 5002
```

If the port changes while using the Wi-Fi command path, the board responds from the old socket first, then rebinds on the next worker cycle.

Example UDP command payload:

```txt
#42 status json
```

Example UDP response payload:

```txt
#42 {"ok":true,"name":"WT02E40E-CMD",...}
```

## Discovery beacons

Discovery is LAN discovery, separate from Wi-Fi AP scanning.

Commands:

```txt
discovery status
discovery on
discovery off
```

When discovery is enabled and Wi-Fi has IPv4, the board broadcasts JSON to UDP port `5000` every few seconds.

Example discovery payload:

```json
{
  "type": "wt02e40e_discovery",
  "name": "WT02E40E-CMD",
  "fw": "0.3.9-wifi-led-idle",
  "ip": "192.168.1.123",
  "cmd_port": 5001,
  "udp_rx_port": 5000,
  "uptime_s": 42
}
```

The webapp uses the packet sender IP plus the advertised `cmd_port` to auto-fill the Wi-Fi command panel.

## Bridge forwarding

Bridge rules let you configure common forwarding outputs once, then send payloads without spelling out every transport every time.

Commands:

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

Defaults:

```txt
bridge ble=on
bridge uart=on
bridge wifi=off
bridge target=<unset>:5000
```

`bridge wifi on` needs a target first:

```txt
bridge target 192.168.1.50 5000
bridge wifi on
bridge send "hello over bridge"
```

Bridge outputs use the same queued transmit paths and the same `~S/~C/~E` stream framing when a payload is too large for one packet.

## Device identity, BLE name, and saved config

Identity/status commands:

```txt
id
version
status
status json
config
config json
```

BLE name commands:

```txt
ble name
ble name get
ble name set WT02E40E-CMD-01
name set WT02E40E-CMD-01
```

If the device is advertising, changing the name restarts advertising with the new name. If a BLE client is already connected, the new name appears on the next advertising cycle.

Saved config commands:

```txt
config save
config reset
boot status
boot mode idle
boot mode ble
boot mode wifi
boot mode both
```

`config save` persists runtime app settings such as BLE name, boot mode, discovery setting, Wi-Fi command-port setting, and bridge settings through Zephyr settings/NVS.

Wi-Fi credentials use the Zephyr Wi-Fi credentials/settings backend.

## Command list summary

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

### Boot behavior

```txt
boot status
boot mode idle
boot mode ble
boot mode wifi
boot mode both
```

Use `config save` after changing boot mode if it should persist.

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

### Wi-Fi control

```txt
wifi on
wifi off
wifi off 5s
wifi status
wifi status json
wifi reconnect
```

### Wi-Fi credentials

```txt
wifi cred list
wifi cred set <ssid> <password> [wpa2|auto|wpa3]
wifi cred add <ssid> <password> [wpa2|auto|wpa3]
wifi cred open <ssid>
wifi cred forget <ssid>
wifi cred clear
```

### Wi-Fi AP scanning

```txt
wifi scan
wifi scan json
wifi scan full json
wifi scan start
wifi scan wait
wifi scan wait json
wifi scan last
wifi scan last json
wifi scan item <index>
wifi scan item <index> json
wifi scan status
wifi scan clear
wifi scan connect <index> <password> [wpa2|auto|wpa3]
wifi scan open <index>
```

### Wi-Fi UDP command receive

```txt
wifi cmd status
wifi cmd on
wifi cmd off
wifi cmd port
wifi cmd port <port>
```

### Discovery beacon

```txt
discovery on
discovery off
discovery status
```

### Indicator commands

```txt
led status
led test all
led test ble
led test wifi
led pulse ble
led pulse wifi
led pulse activity
led pulse alert
```

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

### Ping and latency tests

```txt
ping
ping uart
ping ble
ping wifi <ip> <port>
```

`ping ble` requires BLE TX notifications to be enabled.

### Transmit commands

```txt
tx ble <message>
tx uart <message>
tx wifi <ipv4> <port> <message>
tx both <ipv4> <port> <message>
```

`tx ble` sends to the BLE TX notify characteristic. A BLE client must be connected and subscribed to TX notifications.

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

Use delayed forms over BLE/Wi-Fi so the response can leave before reset.

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
#43 {"ok":true,...}
#44 ok ble tx ...
```

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
Payload: UTF-8 text command
Trailing newline: optional
Example: tx ble "hello from ble"
```

BLE response packet:

```txt
Board -> Command response characteristic
Payload: UTF-8 text response or stream frames
Readable as last-response value for short responses
Notified when command response notifications are enabled
```

BLE TX packet:

```txt
Board -> TX notify characteristic
Payload: UTF-8 text payload or stream frames
Notify only
Used by: tx ble, bridge ble output, ping ble
```

BLE status packet:

```txt
Board -> Status read/notify characteristic
Payload: UTF-8 status line or status JSON
Read manually any time
Periodic notification only when status notifications are enabled
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

### Wi-Fi UDP command structure

Board command receive socket:

```txt
Node/webapp/host -> WT02E40E
UDP destination port: default 5001, configurable with wifi cmd port <port>
Payload: UTF-8 text command
```

Board command response:

```txt
WT02E40E -> sender IP/port
UDP payload: UTF-8 text response or stream frame
```

The UDP command path is convenient for LAN testing. Add authentication before using it on an untrusted LAN.

### Wi-Fi UDP payload TX structure

The `tx wifi` command sends arbitrary text to a target UDP host/port:

```txt
Command:
  tx wifi <ipv4> <port> <message>

Packet:
  Board -> <ipv4>:<port>
  UDP payload = <message> as UTF-8 bytes or stream frames if needed
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
  "fw": "0.3.9-wifi-led-idle",
  "cmd_port": 5001,
  "udp_rx_port": 5000,
  "uptime_s": 42
}
```

### Wi-Fi scan JSON structure

Summary scan response:

```json
{
  "type": "wifi_scan",
  "running": false,
  "valid": true,
  "count": 3,
  "status": 0,
  "age_s": 1,
  "results": []
}
```

Single scan item response:

```json
{
  "type": "wifi_scan_item",
  "i": 1,
  "ssid": "My Home WiFi",
  "rssi": -44,
  "channel": 6,
  "band": "2.4GHz",
  "security": "WPA2-PSK",
  "bssid": "aa:bb:cc:dd:ee:ff"
}
```

Full streamed scan response:

```json
{
  "type": "wifi_scan_full",
  "running": false,
  "valid": true,
  "count": 3,
  "status": 0,
  "age_s": 1,
  "results": [
    {
      "i": 1,
      "ssid": "My Home WiFi",
      "rssi": -44,
      "channel": 6,
      "band": "2.4GHz",
      "security": "WPA2-PSK",
      "bssid": "aa:bb:cc:dd:ee:ff"
    }
  ]
}
```

## Use case demo command flows

These are quick bring-up paths. Replace IPs, SSIDs, and passwords with local values.

### 1. First BLE sanity test

Connect from the web console or nRF Connect. Enable command response notifications. Send:

```txt
status
ble status
config
id
version
```

Expected shape:

```txt
BLE connected
mode=ble
wifi_req=off
```

### 2. UART to BLE TX test

Enable notifications on the BLE TX characteristic:

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

If the command says TX notify is off, the BLE client is connected but has not enabled TX notifications.

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

### 4. Streamed command response test

Enable command response notifications, then request a full streamed scan:

```txt
wifi scan full json
```

The web console should reassemble the frames and show one JSON response. Raw frame shape:

```txt
~S001
~C001000<payload>
~C001001<payload>
~E001002
```

### 5. BLE-safe Wi-Fi scan and item pull

This flow keeps every response small:

```txt
wifi scan json
wifi scan last json
wifi scan item 1 json
wifi scan item 2 json
wifi scan item 3 json
```

Connect using a selected AP index:

```txt
wifi scan connect 1 "your password" wpa2
wifi reconnect
wifi status
```

For an open AP:

```txt
wifi scan open 1
wifi reconnect
wifi status
```

### 6. Manual Wi-Fi power and reconnect

Prepare Wi-Fi without connecting:

```txt
wifi on
wifi status
```

Connect using stored credentials:

```txt
wifi reconnect
wifi status
```

Turn Wi-Fi off:

```txt
wifi off
wifi status
```

### 7. BLE to Wi-Fi UDP packet test

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
wifi reconnect
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

### 8. Wi-Fi UDP command to BLE TX test

With BLE connected and TX notifications enabled:

```txt
mode both
wifi on
wifi cmd on
wifi status
```

In the webapp Wi-Fi command panel, send to the board IP and command port:

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

### 9. Wi-Fi UDP command to UART test

In the webapp Wi-Fi command panel:

```txt
tx uart "hello from wifi command path"
```

Expected UART/log-side output:

```txt
hello from wifi command path
```

### 10. Discover the board on the LAN

Enable discovery and save it if desired:

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

### 11. Rename the BLE device

```txt
ble name set WT02E40E-CMD-01
config save
```

The current connection keeps working. The new name appears on the next advertising cycle.

### 12. Change Wi-Fi command port live

```txt
wifi cmd port
wifi cmd port 5002
config save
```

If the command was sent over Wi-Fi, the response is sent from the old port first, then the board rebinds to the new port.

### 13. Use request IDs

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

### 14. Safe delayed radio cutoff

From BLE:

```txt
ble off 5s
```

Expected:

```txt
ok ble off scheduled 5000 ms
```

The BLE link drops after the delay.

From Wi-Fi:

```txt
wifi off 5s
```

Expected:

```txt
ok wifi off scheduled 5000 ms
```

The Wi-Fi command path disappears after the delay.

### 15. Bridge all enabled outputs

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

Expected outputs when available:

```txt
BLE TX notification if subscribed
UART output
UDP packet to bridge target if Wi-Fi is connected
```

### 16. Indicator checks

Manual LED sanity checks:

```txt
led status
led test all
led test ble
led test wifi
led pulse activity
led pulse alert
```

Runtime traffic indication:

```txt
BLE connected                  LED0 solid
BLE command response/TX sent    LED0 short off dip
Wi-Fi scan running              LED1 fast blink
Wi-Fi IPv4 bound                LED1 solid
```

### 17. J18 UART-to-BLE blink test

With the SAMD21J18 sending UART commands into the nRF bridge:

```txt
tx ble j18_blink
```

Expected BLE/webapp log when TX notifications are enabled:

```txt
TX <= j18_blink
```

### 18. Reboot safely from BLE or Wi-Fi

```txt
fw status
fw reboot 5s
```

The delayed form gives the response time to reach the client before the board resets.

## Source layout

```txt
src/main.c        Boot/init sequence
src/wt_app.c      App status, config, bridge, boot, and reboot commands
src/wt_ble.c      BLE advertising, GATT service, command write, response streams, and TX streams
src/wt_wifi.c     Wi-Fi prep, connect loop, scan, credentials, UDP command service, and UDP streams
src/wt_shell.c    UART/RTT shell commands
src/wt_radio.c    Radio mode switching glue
src/wt_leds.c     LED state machine
src/wt_common.c   Shared parsers and small helpers
src/wt_config.h   App constants and payload budgets
src/wt_stream.c   Shared stream framing helper
src/wt_stream.h   Shared stream framing API
BLE_Webapp/       Browser console and Node UDP helper
```

`main.c` should stay boring. New radio behavior usually belongs in `wt_radio.c`, Wi-Fi behavior in `wt_wifi.c`, BLE behavior in `wt_ble.c`, and command syntax in `wt_shell.c` or the app command parser.

## Important project files

```txt
prj.conf              Main app Kconfig
sysbuild.conf         Wi-Fi and nRF5340 network-core image selection
app.overlay           LEDs, UART pin mapping, and local Wi-Fi MAC fallback
CMakeLists.txt        Source file list
Kconfig               App-specific boot mode settings
WT02E40E_COMMANDS.md  Command quick reference
BLE_Webapp/index.html Web Bluetooth console
BLE_Webapp/server.js  Local Node HTTP/UDP helper
```

## Troubleshooting

### `CONFIG_WIFI_NRF70 was assigned y, but got n`

The build is not seeing an enabled nRF7002 devicetree node.

Common causes:

```txt
Wrong board target
Stale build directory
VS Code building an old configuration
```

Use a clean sysbuild:

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

Use the nRF Connect scanner or the web console, not the phone OS Bluetooth pairing screen.

### VS Code says `CONFIG_BT` is missing

If the terminal `west build` works, the warning is likely from cpptools looking at files without the sysbuild Kconfig context. Reset IntelliSense or ignore the Problems entry.

### LED0 blinks forever

That is normal when BLE is advertising and no BLE client is connected.

Connect from the web console or nRF Connect. LED0 should become solid after connection.

### LED1 blinks on boot

In normal BLE-only boot, LED1 should be off. LED1 blinking means the firmware thinks Wi-Fi is requested, preparing, or scanning.

Check saved config:

```txt
config
boot status
wifi status
```

Return to BLE-only boot if needed:

```txt
boot mode ble
config save
reboot
```

or clear saved runtime config:

```txt
config reset
reboot
```

### Wi-Fi scan returns `running=true` first

That is normal. Scan is asynchronous.

Use:

```txt
wifi scan last json
```

a moment later, then pull items:

```txt
wifi scan item 1 json
```

### Wi-Fi scan returns busy once after `wifi on`

The scan command has retry/settle behavior, but Wi-Fi radio startup can still take a moment. The recommended flow from BLE boot mode is:

```txt
wifi scan json
```

without pressing `wifi on` first. Scan will temporarily power Wi-Fi by itself when needed.

### UART shell does not respond

Check:

```txt
115200 8N1
Common ground
Host RX connected to nRF TX P1.04
Host TX connected to nRF RX P1.06
```

RTT shell is available as a backup.

### `tx ble` says notifications are off

A BLE client is connected but has not enabled notifications on the TX characteristic.

Enable TX notify UUID:

```txt
7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
```

Command responses use a different characteristic:

```txt
7f1c0005-2b5a-4f2d-9a31-d6a5f4e040e1
```

### Webapp says a GATT operation is already in progress

The webapp has queues for command writes and scan item pulls, but browser BLE state can still get wedged after disconnects or refreshes.

Use:

```txt
Disconnect
Reconnect
Enable command response notifications
Run status
```

If it persists, reload the page and reconnect.

### Logs mention `__log_level` or `__log_current_dynamic_data`

`LOG_MODULE_REGISTER()` must appear before headers that use inline logging macros. This project keeps module registration in the C files that need it.

If the terminal build succeeds but VS Code Problems still shows these, reset IntelliSense or ignore the stale cpptools view.

## Notes before productizing

- Add BLE pairing/bonding or application-level authentication before exposing commands in deployment.
- Add authentication to the Wi-Fi UDP command socket before using it on an untrusted LAN.
- Consider CBOR, protobuf, or a compact binary protocol if this becomes a production host-MCU interface.
- Verify nRF7002 antenna design, regulatory configuration, MAC address handling, and production credential storage.
- Review power supply burst capacity before sustained Wi-Fi TX.
- Add watchdog and fault recovery policies for unattended operation.
