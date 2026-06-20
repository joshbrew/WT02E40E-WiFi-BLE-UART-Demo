# WT02E40E Node BLE + Wi-Fi Console

This is a tiny Node-hosted console for the WT02E40E firmware.

It does two jobs:

```text
Browser <-> WT02E40E over BLE command characteristics
WT02E40E -> Node over Wi-Fi UDP packets
```

No npm dependencies are required. The server uses only built-in Node modules.

## Files

```text
server.js
    Serves the web UI, opens a UDP listener, streams UDP packets into the browser.

index.html
    Web Bluetooth command UI, command templates, and Wi-Fi UDP receive display.

README.md
    This file.
```

## Run

From this folder:

```powershell
node server.js
```

Or choose ports:

```powershell
node server.js --http 8080 --udp 5000
```

Then open:

```text
http://localhost:8080
```

Chrome/Edge allow Web Bluetooth on `localhost`, so this works without HTTPS during development.

## Normal flow

1. Connect to `WT02E40E-CMD` over BLE.
2. Enable command response notifications.
3. Use **Command templates** or send commands manually.
4. For Wi-Fi packets, connect the board to Wi-Fi, then use the page's UDP helper.

Common bring-up sequence:

```text
mode both
wifi cred set YourSSID YourPassword wpa2
wifi on
wifi status
```

Ask the board to send UDP to this Node page:

```text
tx wifi <your-computer-lan-ip> 5000 hello from WT02E40E over UDP
```

Node receives the UDP packet and streams it into the browser log.

## Command reference

The BLE command write characteristic accepts the same command strings as the UART shell. The `wt` prefix is optional for BLE command writes.

### Status

```text
status
wt status
ble status
wifi status
```

### Mode switching

```text
mode idle
mode ble
mode wifi
mode both
```

Default boot mode is `mode ble`.

### BLE control

```text
ble on
ble off
ble status
```

### Wi-Fi control

```text
wifi on
wifi off
wifi status
wifi reconnect
```

### Wi-Fi credentials

```text
wifi cred list
wifi cred set <ssid> <password> [wpa2|auto|wpa3]
wifi cred add <ssid> <password> [wpa2|auto|wpa3]
wifi cred open <ssid>
wifi cred forget <ssid>
wifi cred clear
```

`set` clears stored runtime credentials and selects one secured network.

`add` adds another secured network.

`open` stores/selects an open network.

`clear` clears runtime credentials. The compiled fallback credentials in firmware remain compiled into the image.

### Transmit

```text
tx ble <message>
tx uart <message>
tx wifi <ipv4> <port> <message>
tx both <ipv4> <port> <message>
```

`tx ble` emits over the BLE TX notify characteristic when a client is subscribed.

`tx uart` writes to the firmware UART transmit path.

`tx wifi` sends a UDP packet over Wi-Fi.

`tx both` uses the combined transmit path implemented in the firmware.

## BLE characteristic map

```text
Service:               7f1c0001-2b5a-4f2d-9a31-d6a5f4e040e1
TX notify:             7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
Status read/notify:    7f1c0003-2b5a-4f2d-9a31-d6a5f4e040e1
Command write:         7f1c0004-2b5a-4f2d-9a31-d6a5f4e040e1
Command response:      7f1c0005-2b5a-4f2d-9a31-d6a5f4e040e1
```

## Firewall note

Windows Firewall may block inbound UDP. Allow Node.js on private networks or the UDP packets will vanish into the tiny moat.

## Notes on the current UI fixes

The page now de-duplicates identical status/response lines that arrive within 500 ms. This avoids the common Web Bluetooth pattern where a manual read and a near-simultaneous notification print the same status twice.

The BLE TX test now auto-enables the TX notification characteristic before sending `tx ble` or `tx both`. Without TX notifications enabled, the firmware cannot deliver the BLE TX packet and will respond with:

```text
err ble tx notify off; enable TX notify first
```

If the page says the Node API returned HTML instead of JSON, the page is being served by the wrong server. Run either:

```powershell
cd BLE_Webapp
node server.js
```

or from the firmware project root:

```powershell
node BLE_Webapp\server.js
```

Then open:

```text
http://localhost:8080
```


## Fix notes for this build

This build fixes the manual **Read status once** double-print by treating `readValue()` as the only log source for manual reads and suppressing synthetic `characteristicvaluechanged` events unless status notifications are actually enabled.

It also adds no-cache headers and a version string to `/api/config`, which makes it easier to tell when the browser is accidentally showing an old cached page or a static-server copy instead of the Node server.


## Webapp version notes

`2026-06-20-command-log-order-v4` logs `CMD => ...` before awaiting the BLE write. That keeps command/response/TX output readable even when the device responds immediately and browser notification events arrive before `writeValue*()` resolves.
