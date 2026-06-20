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
wt mode idle
wt mode ble
wt mode wifi
wt mode both
wt wifi on
wt wifi off
wt wifi status
wt wifi reconnect
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

## Wi-Fi credential behavior

Compiled defaults are still present in `prj.conf`:

```conf
CONFIG_WIFI_CREDENTIALS_STATIC_SSID="Myssid"
CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD="Mypassword"
```

Runtime override examples over UART or RTT:

```txt
wt wifi cred set testwifi testpassword123
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
wifi cred set testwifi testpassword123
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
