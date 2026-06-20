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
wifi scan
wifi scan json
wifi scan last json
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


## Quoted arguments

The firmware command parser supports quoted arguments over BLE and Wi-Fi UDP commands, and the UART/RTT shell accepts quoted arguments through Zephyr shell parsing. Use quotes for SSIDs, passwords, and other positional arguments with spaces.

```text
wifi cred set "My Home WiFi" "correct horse battery staple" wpa2
wifi cred open "Coffee Shop WiFi"
tx ble "hello with spaces"
tx wifi <computer-ip> 5000 "payload with spaces"
```

The webapp credential helper automatically quotes SSID/password values when needed.

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


## Wi-Fi command channel

Firmware now listens for UDP commands on board port `5001` when Wi-Fi command receive is enabled. The Node webapp sends UDP packets from its listener socket, so replies come back into the same browser log.

Flow:

```text
Browser -> Node /api/udp/send -> UDP -> WT02E40E:5001
WT02E40E -> UDP response -> Node UDP listener -> browser event log
```

Useful commands:

```text
wifi cmd status
wifi cmd on
wifi cmd off
wifi cmd port
wifi cmd port 5002
status
wifi status
tx uart hello from wifi command
tx ble hello from wifi command
tx wifi <computer-ip> 5000 hello back to node
```


## BLE self-off note

The webapp can send `ble off`, `mode idle`, or `mode wifi` over the BLE command characteristic. The firmware responds first, then drops the BLE connection after a short delay. Use UART/RTT or the Wi-Fi UDP command panel to turn BLE back on after that.


## New identity/discovery helpers

The webapp includes templates for:

```text
id
version
config
config json
ble name
ble name set WT02E40E-CMD-01
discovery on
discovery status
```

`discovery on` makes the board broadcast a JSON packet to this Node server on UDP port `5000`. The page auto-fills the board IP and board command port when it receives that discovery packet.

## All-features command set

This webapp version includes templates and reference text for the full command feature pass:

```text
id / version / config / config json / status json
boot status / boot mode <idle|ble|wifi|both>
ble name set "WT02E40E-CMD-01"
discovery on/off/status
#42 status
ble off 5s / wifi off 5s / mode ble 5s
bridge status / bridge target <ip> <port> / bridge all on / bridge send <message>
ping / ping ble / ping wifi <ip> <port>
fw status / fw reboot 5s / fw reboot bootloader 5s
```

The built-in bring-up wizard in `index.html` gives the recommended first test sequence.


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
