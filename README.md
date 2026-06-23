# WT02E40E BLE + Wi-Fi command bridge

Firmware for the WT02E40E module, using the nRF5340 application core with the nRF7002 Wi-Fi companion. The app boots into a command bridge that can be controlled over BLE, UART/RTT shell, and UDP once Wi-Fi is connected.

## Table of contents

- [Target](#target)
- [Build and flash](#build-and-flash)
- [Default boot behavior](#default-boot-behavior)
- [Debug hookup](#debug-hookup)
- [LED behavior](#led-behavior)
- [BLE service](#ble-service)
- [Command and bridge streaming](#command-and-bridge-streaming)
- [Wi-Fi scan flow](#wi-fi-scan-flow)
- [UART/RTT shell](#uartrtt-shell)
- [Wi-Fi UDP command path](#wi-fi-udp-command-path)
- [Web console](#web-console)
- [Core commands](#core-commands)
- [Demo command flows](#demo-command-flows)
- [Source layout](#source-layout)

## Target

Use the Nordic nRF7002 DK target because it provides the nRF70 Wi-Fi devicetree and sysbuild wiring used by this bring-up app.

```txt
Board target: nrf7002dk/nrf5340/cpuapp
Sysbuild:     enabled
```

Do not build this app as `nrf5340dk/nrf5340/cpuapp`; that target does not expose the nRF7002 path used by `CONFIG_WIFI_NRF70`.

## Build and flash

From the app directory:

```powershell
rmdir /s /q build
west build -b nrf7002dk/nrf5340/cpuapp . --sysbuild --pristine
west flash -d build
```

Useful post-build checks:

```powershell
type build\domains.yaml
dir build\hci_ipc\zephyr
```

The nRF5340 BLE controller runs on the network core, so a successful sysbuild includes the `hci_ipc` image.

## Default boot behavior

The app starts in BLE command mode by default.

```conf
CONFIG_WT02E40E_DEFAULT_BLE=y
```

Runtime modes:

```txt
mode idle
mode ble
mode wifi
mode both
```

From BLE, `mode wifi`, `mode idle`, and `ble off` send the response first, then disconnect BLE after a short delay.

## Debug hookup

Working debug-out hookup from the nRF5340 DK to the WT02E40E carrier:

```txt
DK VDD       ----+
                +---- DK VTG
DK VDD_nRF   -----------------> Custom board VDD_NRF
DK GND       -----------------> Custom board GND
DK SWDIO     -----------------> Custom board SWDIO_NRF
DK SWDCLK    -----------------> Custom board SWCLK_NRF
DK RESET     -----------------> Custom board RESET_NRF
DK SWO       -----------------> Custom board SWO_NRF optional
```

If the carrier is self-powered, use common ground and feed the target voltage into the debugger VTG/VTref sense pin instead of tying regulators together.

## LED behavior

The board uses the LED aliases exposed by the devicetree overlay. LED0 and LED1 are the primary indicators. LED2 and LED3 are optional activity/alert outputs when the board exposes those aliases.

```txt
LED0 / blue    BLE state
LED1 / green   Wi-Fi state
LED2 optional  bridge / TX / discovery activity pulse
LED3 optional  alert / error pulse
```

Runtime meanings:

```txt
BLE connected        LED0 solid
BLE report/TX notify LED0 short off dip while connected
BLE advertising      LED0 medium blink
BLE ready idle       LED0 short heartbeat pulse
BLE requested        LED0 slow blink
Wi-Fi IPv4 bound     LED1 solid
Wi-Fi scan running   LED1 fast blink
Wi-Fi associated     LED1 double pulse
Wi-Fi requested      LED1 slow blink
Wi-Fi off            LED1 off
```

Indicator commands:

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

## BLE service

The board advertises as `WT02E40E-CMD` unless renamed.

```txt
Service UUID:             7f1c0001-2b5a-4f2d-9a31-d6a5f4e040e1
TX notify UUID:           7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
Status read/notify UUID:  7f1c0003-2b5a-4f2d-9a31-d6a5f4e040e1
Command write UUID:       7f1c0004-2b5a-4f2d-9a31-d6a5f4e040e1
Command response UUID:    7f1c0005-2b5a-4f2d-9a31-d6a5f4e040e1
```

Enable command response notifications before sending commands from the web console or nRF Connect. The command response characteristic also stores the most recent short response for manual reads.

## Command and bridge streaming

Short command responses and bridge payloads are queued as normal single messages when they fit in the negotiated payload. BLE command responses, BLE TX notifications, browser BLE commands, browser Wi-Fi commands, and browser UDP sends each use a FIFO flush path so bursts stay ordered instead of overlapping the active GATT or UDP operation.

Longer payloads use framed text streaming. BLE command responses stream on the command response characteristic. BLE TX and bridge payloads stream on the TX notify characteristic. Wi-Fi UDP streams use the same frame strings as separate UDP packets:

```txt
~S001                 stream start, stream id 001
~C001000<payload>     chunk 0
~C001001<payload>     chunk 1
~E001002              stream end, 2 chunks sent
```

Frame properties:

```txt
Start frame: ~S + 3-digit stream id
Chunk frame: ~C + 3-digit stream id + 3-digit chunk index + payload bytes
End frame:   ~E + 3-digit stream id + 3-digit chunk count
```

Each frame is kept within the active BLE notification payload, so it works with a small MTU and also stays under the app-level 255-byte budget. Optional request IDs still work for command responses. A command such as `#42 status` returns a payload beginning with `#42 ` after reassembly. Bridge TX payload streams use the same frame grammar without a request ID prefix.

## Wi-Fi scan flow

Scanning turns on the Wi-Fi request state, the UDP command service, and discovery beacon support. The green LED fast-blinks while the scan is active and stays off when Wi-Fi is not requested.

Default BLE-safe scan flow:

```txt
wifi scan json
wifi scan item 1 json
wifi scan item 2 json
wifi scan item 3 json
```

`wifi scan json` returns a compact summary under the small response budget:

```json
{"type":"wifi_scan","running":false,"valid":true,"count":3,"status":0,"age_s":0,"results":[]}
```

Each item is fetched separately:

```json
{"type":"wifi_scan_item","i":1,"ssid":"Example","rssi":-48,"channel":6,"band":"2.4GHz","security":"wpa2","bssid":"aa:bb:cc:dd:ee:ff"}
```

Full streamed scan JSON is available when command response notifications are enabled:

```txt
wifi scan full json
```

That command returns one reassembled JSON document through the `~S/~C/~E` stream framing.

Connect from a scanned result:

```txt
wifi scan connect 1 "wifi password" wpa2
wifi on
wifi status
```

For open networks:

```txt
wifi scan open 1
wifi on
```

## UART/RTT shell

Carrier UART nets:

```txt
nRF P1.04 UART_TX -> host RX
nRF P1.06 UART_RX -> host TX
GND               -> host/debug UART GND
Baud              -> 115200 8N1
```

RTT shell is also enabled through J-Link. Use the `wt` prefix in shell mode:

```txt
wt status
wt wifi status
wt ble status
```

The BLE command parser accepts the same commands with or without the `wt` prefix.

## Wi-Fi UDP command path

When Wi-Fi has IPv4 and the command server is enabled, the board listens for one-line UDP commands.

```txt
Default board command port: 5001
Node/webapp receive port:  5000
```

Control commands:

```txt
wifi cmd status
wifi cmd on
wifi cmd off
wifi cmd port
wifi cmd port 5002
```

Discovery beacons are sent to UDP port `5000` when discovery is enabled.

## Web console

Run the local helper server:

```powershell
node BLE_Webapp/server.js
```

Open:

```txt
http://localhost:8080
```

Recommended browser flow:

```txt
Connect BLE
Enable command response notifications
Send status
Send wifi scan json
Select a scanned SSID
Set credentials or connect by scan index
```

The web console uses command response notifications as the primary response path. Manual response reads are only used when notifications are disabled, so BLE command writes, response reads, and scan item pulls stay serialized on browsers that allow only one active GATT operation at a time.

Chrome or Edge is recommended for Web Bluetooth.

## Core commands

```txt
status
status json
id
version
config
config json
config save
config reset
boot status
boot mode idle|ble|wifi|both
name get
name set <ble-name>
ble on
ble off [delay]
ble status [json]
ble name [get|set <name>]
mode idle|ble|wifi|both [delay]
wifi on
wifi off [delay]
wifi status [json]
wifi reconnect
wifi scan
wifi scan json
wifi scan full json
wifi scan last [json]
wifi scan item <index> [json]
wifi scan connect <index> <password> [wpa2|auto|wpa3]
wifi scan open <index>
wifi scan clear
wifi cred list
wifi cred set <ssid> <password> [wpa2|auto|wpa3]
wifi cred add <ssid> <password> [wpa2|auto|wpa3]
wifi cred open <ssid>
wifi cred forget <ssid>
wifi cred clear
wifi cmd status
wifi cmd on
wifi cmd off
wifi cmd port [port]
discovery status
discovery on
discovery off
bridge status
bridge target <ipv4> <port>
bridge ble|uart|wifi|all on|off
bridge send <message>
ping
ping ble
ping wifi <ipv4> <port>
tx ble <message>
tx uart <message>
tx wifi <ipv4> <port> <message>
tx both <ipv4> <port> <message>
fw status
fw reboot [delay]
```

Quoted arguments are supported for SSIDs, passwords, names, and payloads:

```txt
wifi cred set "My Home WiFi" "correct horse battery staple" wpa2
tx wifi 192.168.1.50 5000 "payload with spaces"
```

## Demo command flows

BLE sanity:

```txt
status
ble status
id
version
```

Enable Wi-Fi and UDP command service:

```txt
mode both
wifi cred set "My WiFi" "my password" wpa2
wifi on
wifi cmd on
wifi status
```

Scan and connect by index:

```txt
wifi scan json
wifi scan item 1 json
wifi scan connect 1 "my password" wpa2
wifi on
wifi status
```

Full streamed scan:

```txt
wifi scan full json
```

Send a UDP packet from the board to the web console host:

```txt
tx wifi 192.168.1.50 5000 "hello from WT02E40E"
```

Keep BLE and Wi-Fi active together:

```txt
mode both
wifi on
ble status
wifi status
```

Schedule a safe radio cutoff:

```txt
ble off 5s
wifi off 5s
mode ble 5s
```

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
BLE_Webapp/       Browser console and Node UDP helper
```
