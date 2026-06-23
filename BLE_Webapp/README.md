# WT02E40E BLE web console

Browser console for the WT02E40E command bridge. It talks to the board over Web Bluetooth and uses a small Node helper for UDP receive/send testing.

## Run

```powershell
node BLE_Webapp/server.js
```

Open:

```txt
http://localhost:8080
```

Use Chrome or Edge on `localhost` or HTTPS so Web Bluetooth is available.

## BLE flow

1. Click **Connect BLE**.
2. Select the board advertising as `WT02E40E-CMD` or another `WT02E40E*` name.
3. The page opens the custom service.
4. The page enables command response notifications.
5. Send commands from templates, quick buttons, or the command textarea.

Service UUIDs:

```txt
Service UUID:             7f1c0001-2b5a-4f2d-9a31-d6a5f4e040e1
TX notify UUID:           7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
Status read/notify UUID:  7f1c0003-2b5a-4f2d-9a31-d6a5f4e040e1
Command write UUID:       7f1c0004-2b5a-4f2d-9a31-d6a5f4e040e1
Command response UUID:    7f1c0005-2b5a-4f2d-9a31-d6a5f4e040e1
```

## Command response streams

Normal short command replies appear as a single response notification.

Long BLE replies use framed stream notifications on the same command response characteristic. Long Wi-Fi command replies use the same frame strings as UDP packets:

```txt
~S001
~C001000<payload>
~C001001<payload>
~E001002
```

The page reassembles stream chunks by stream id and chunk index, then logs the final payload as a normal response. This keeps every BLE notification under the negotiated BLE payload size and below the app-level 255-byte budget.


## Quick command deck

The quick command deck groups the high-use controls into Radio, Wi-Fi, Scan + credentials, and Indicator routines. The Wi-Fi on/off buttons also request `wifi status json` so the status pills update from the board response.

Indicator buttons call the firmware `led` command family:

```txt
led status
led test all
led pulse ble
led pulse wifi
led pulse activity
led pulse alert
```

## Wi-Fi scan helper

The scan helper uses the small default BLE-safe scan flow:

```txt
wifi scan json
wifi scan item 1 json
wifi scan item 2 json
...
```

The first command returns a summary and count. The page then fetches each AP item separately and fills the scanned SSID dropdown.

The full-stream scan command is also available from the command textarea or template list:

```txt
wifi scan full json
```

That returns one JSON document reassembled through the stream framing.

## UDP helper

The Node server listens for UDP packets on port `5000` by default. The page shows incoming discovery packets and board-originated UDP messages in the log.

The Wi-Fi command panel sends one-line UDP commands to the board command port, default `5001`.

Useful commands:

```txt
wifi cmd on
wifi cmd status
wifi cmd port 5001
discovery on
status
wifi status
```

## Common bring-up commands

```txt
status
ble status
wifi status
mode both
wifi scan json
wifi cred set "Your SSID" "Your Password" wpa2
wifi on
wifi cmd on
discovery on
tx wifi HOST_IP 5000 "hello from WT02E40E"
```

## Files

```txt
index.html   Web Bluetooth console and UDP event UI
server.js    Local HTTP server, UDP listener, UDP send endpoint, and Server-Sent Events
```
