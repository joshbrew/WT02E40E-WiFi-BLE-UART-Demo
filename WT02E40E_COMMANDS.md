# WT02E40E Wi-Fi/BLE command bring-up

Build target:

```txt
nrf7002dk/nrf5340/cpuapp
```

Use sysbuild.

```bash
west build -b nrf7002dk/nrf5340/cpuapp . --sysbuild --pristine
west flash
```

## Default mode

The app boots in BLE-only mode so the nRF7002 does not try to transmit while the Wi-Fi antenna is missing.

```conf
CONFIG_WT02E40E_DEFAULT_BLE=y
```

To change the hardcoded boot mode, set one of these in `prj.conf`:

```conf
CONFIG_WT02E40E_DEFAULT_IDLE=y
CONFIG_WT02E40E_DEFAULT_BLE=y
CONFIG_WT02E40E_DEFAULT_WIFI=y
CONFIG_WT02E40E_DEFAULT_BOTH=y
```

Only one should be selected.

## UART shell pins

From the carrier schematic:

```txt
nRF P1.04 UART_TX -> PA23/RX1 net
nRF P1.06 UART_RX -> PA22/TX1 net
GND               -> host/debug UART GND
Baud              -> 115200 8N1
```

RTT shell is also enabled, so the same commands are available through J-Link RTT if UART is not wired yet.

## Commands

```txt
wt status
wt status json
wt id
wt version
wt config
wt config json
wt config save
wt config reset
wt name get
wt name set <ble-name>
wt discovery on
wt discovery off
wt discovery status
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
wt ble name
wt ble name set <ble-name>
wt tx ble <message>
wt tx uart <message>
wt tx wifi <ipv4> <port> <message>
wt tx both <ipv4> <port> <message>
```


## Quoted arguments

The command parser supports quoted arguments for SSIDs, passwords, and payloads with spaces. This works over BLE commands and Wi-Fi UDP commands. UART/RTT shell commands use Zephyr shell parsing, which also handles quoted arguments.

```txt
wt wifi cred set "My Home WiFi" "correct horse battery staple" wpa2
wt wifi cred add 'Lab WiFi' 'password with spaces' auto
wt wifi cred open "Coffee Shop WiFi"
wt wifi cred forget "My Home WiFi"
wt tx ble "hello with spaces"
wt tx uart "quote test: \"hello\""
wt tx wifi 192.168.1.50 5000 "payload with spaces"
```

Backslash escapes are supported inside quoted text for quotes, backslashes, tabs, carriage returns, and newlines.

## Wi-Fi credential behavior

Compiled defaults are still present in `prj.conf`:

```conf
CONFIG_WIFI_CREDENTIALS_STATIC_SSID="Myssid"
CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="Mypassword"
```

Runtime override examples over UART or RTT:

```txt
wt wifi cred set "My WiFi" "test password 123"
wt wifi on
```

`set` clears the stored runtime credential list and makes the new SSID the favorite. If Wi-Fi is already running, the app restarts the Wi-Fi connection attempt using the new credentials.

`add` keeps other stored SSIDs and adds another favorite-capable entry.

`open` stores an open network without a password:

```txt
wt wifi cred open MyOpenNetwork
```

`clear` removes stored runtime credentials. After a reboot, the compiled static fallback credentials are still available from `prj.conf`.

Security modes:

```txt
wpa2   WPA2-PSK, default
wpa3   WPA3-SAE
auto   WPA/WPA2/WPA3 personal auto mode
```

## LED states

```txt
Blue blinking   firmware alive / BLE advertising / idle heartbeat
Blue solid      BLE connected
Green off       Wi-Fi not associated
Green blinking  Wi-Fi associated, waiting for IPv4
Green solid     Wi-Fi IPv4 bound
```

## BLE characteristics

The app boots in BLE mode by default. It advertises as `WT02E40E-CMD`. You do not need Wi-Fi or a Wi-Fi antenna to test this path.

Connect with nRF Connect and open the custom service:

```txt
Service UUID:             7f1c0001-2b5a-4f2d-9a31-d6a5f4e040e1
TX notify UUID:           7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
Status read/notify UUID:  7f1c0003-2b5a-4f2d-9a31-d6a5f4e040e1
Command write UUID:       7f1c0004-2b5a-4f2d-9a31-d6a5f4e040e1
Command response UUID:    7f1c0005-2b5a-4f2d-9a31-d6a5f4e040e1
```

The status characteristic can be read immediately after connecting. It returns a compact text payload like:

```txt
WT02E40E status: mode=ble ble=on adv=off tx_notify=off status_notify=off cmd_rsp_notify=off wifi_req=off wifi_assoc=off ipv4=off uptime=12s
```

Enable notifications on the status characteristic to get a periodic status ping every 5 seconds. The firmware only schedules that ping while a BLE client is connected and status notifications are enabled, so it does not run blindly when nobody is listening.

Enable notifications on the TX characteristic, then use this from the UART/RTT shell:

```txt
wt tx ble hello from uart
```

Enable notifications on the command response characteristic, then write UTF-8 text to the command characteristic. The `wt` prefix is optional on BLE, so these two are equivalent:

```txt
wt status
status
```

BLE command examples:

```txt
status
mode wifi
wifi cred set "My WiFi" "test password 123"
wifi on
tx uart hello from ble
tx wifi 192.168.1.50 5000 hello over udp from ble
tx ble echo back over the BLE TX characteristic
tx both 192.168.1.50 5000 send over wifi, uart, and BLE TX if enabled
```

The command response characteristic stores the last response so it can be read manually, and it notifies responses only when a BLE client has enabled response notifications.


## Build notes

Use `nrf7002dk/nrf5340/cpuapp` with sysbuild enabled. Delete the build folder after switching from `nrf5340dk/nrf5340/cpuapp`, otherwise Kconfig/devicetree warnings can stay cached.

The app uses shell backends for command/log access. Direct RTT and UART log backends are disabled to avoid backend/channel conflicts with the shell.


## Build note

Use `nrf7002dk/nrf5340/cpuapp` with sysbuild enabled. Delete the build directory before switching board targets.

## Source layout

The application is split into small files so the control flow is easier to follow:

```txt
src/main.c        Boot sequence only: init BLE, Wi-Fi callbacks/thread, LEDs, default mode.
src/wt_wifi.c     Wi-Fi interface prep, connect/disconnect loop, DHCP events, credentials, UDP TX.
src/wt_ble.c      BLE advertising, GATT service, TX/status/command characteristics.
src/wt_shell.c    `wt ...` UART/RTT shell commands.
src/wt_radio.c    Mode switching glue: idle, ble, wifi, both.
src/wt_leds.c     Blue/green indicator LED thread.
src/wt_common.c   Small shared parsers/helpers.
src/wt_config.h   Local app constants.
```

`main.c` should stay boring. If behavior changes, it probably belongs in the radio, Wi-Fi, BLE, shell, or LED module instead.

## Wi-Fi UDP command receive

The firmware now supports a third command path over Wi-Fi UDP.

```text
Default board command port: 5001
Node/webapp receive port:  5000
```

Once Wi-Fi is connected and IPv4 is bound, the board listens for one-line command packets on the configured UDP command port, default `5001`. It executes the same command grammar used by BLE command writes and replies to the sender address/port.

Control commands:

```text
wifi cmd status
wifi cmd on
wifi cmd off
wifi cmd port
wifi cmd port 5002
```

Examples from the Node webapp Wi-Fi command panel:

```text
status
wifi status
wifi cred list
tx uart hello from wifi command
tx ble hello from wifi command
tx wifi <computer-ip> 5000 hello back to Node
```

Recommended three-way bring-up:

```text
mode both
wifi cred set <ssid> <password> wpa2
wifi on
wifi cmd on
wifi status
```

Then send commands either over BLE, UART shell, or Wi-Fi UDP to `<board-ip>:5001`.


## BLE on/off notes

BLE can be controlled from UART/RTT, BLE commands, or Wi-Fi UDP commands:

```txt
wt ble on
wt ble off
wt mode ble
wt mode wifi
wt mode idle
wt mode both
```

BLE can be turned off from BLE too. In that case the firmware sends the command response first, waits `WT_BLE_SELF_STOP_DELAY_MS`, then disconnects and stops advertising.

UART/RTT remains the safest local supervisory interface when testing radio toggles because it can bring either radio back up:

```txt
wt wifi on
wt wifi off
wt ble on
wt ble off
```


## Identity, BLE name, and discovery

Identity/config commands are available over UART/RTT with the `wt` prefix and over BLE/Wi-Fi without the prefix.

```txt
wt id
wt version
wt config
wt config json
wt status json
```

Rename the BLE advertising name at runtime:

```txt
wt ble name
wt ble name set WT02E40E-CMD-01
wt name set WT02E40E-CMD-01
```

Enable discovery beacons so the Node webapp can find the board IP:

```txt
wt discovery on
wt discovery status
wt discovery off
```

Discovery broadcasts JSON to UDP port `5000`. The board command socket remains UDP port `5001`.

```json
{"type":"wt02e40e_discovery","name":"WT02E40E-CMD-01","fw":"0.3.1-udp-rebind","cmd_port":5001,"udp_rx_port":5000,"uptime_s":42}
```

## Full feature command additions

### Identity / status

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

### Saved config / boot behavior

```text
boot status
boot mode idle
boot mode ble
boot mode wifi
boot mode both
config save
config reset
```

### BLE name

```text
ble name
ble name set "WT02E40E-CMD-01"
name set "WT02E40E-CMD-01"
config save
```

### Discovery

```text
discovery status
discovery on
discovery off
```

### Request IDs

```text
#42 status
#43 wifi status json
```

### Delayed radio cutoff

```text
ble off 5s
wifi off 5s
mode ble 5s
mode wifi 5s
mode idle 5s
```

### Bridge forwarding

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
```

### Ping

```text
ping
ping uart
ping ble
ping wifi <ip> <port>
```

### Firmware / reboot

```text
fw status
fw reboot
fw reboot 5s
fw reboot bootloader 5s
reboot
reboot 5s
```


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
