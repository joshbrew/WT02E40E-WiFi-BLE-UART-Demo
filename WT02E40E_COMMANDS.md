# WT02E40E command reference

The same command grammar is available over BLE command write, UART/RTT shell, and Wi-Fi UDP command packets. Use the `wt` prefix in UART/RTT shell. The BLE and UDP command parsers accept commands with or without the `wt` prefix.

BLE command responses and BLE TX notifications use FIFO flush queues. Short messages are sent as one queued notification. Longer messages are framed into `~S/~C/~E` chunks and each frame is queued in order. The web console also queues outgoing BLE commands, Wi-Fi command UDP packets, and raw UDP sends so scan item pulls and bridge tests do not overlap.

## Build target

```txt
nrf7002dk/nrf5340/cpuapp
```

Build with sysbuild:

```powershell
west build -b nrf7002dk/nrf5340/cpuapp . --sysbuild --pristine
west flash -d build
```

## Response and bridge streams

Command responses and bridge payloads are kept under the 255-byte app budget for normal replies. When a response or payload does not fit in one BLE notification payload, it uses framed stream messages. BLE command responses stream on the command response characteristic, BLE TX/bridge payloads stream on the TX notify characteristic, and Wi-Fi UDP streams use the same frame strings as separate UDP packets:

```txt
~S001
~C001000<payload>
~C001001<payload>
~E001002
```

The web console reassembles command response, BLE TX, and UDP streams before logging or parsing them. Request IDs are preserved inside reassembled command response payloads.

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
wt boot status
wt boot mode idle|ble|wifi|both
wt name get
wt name set <ble-name>
wt mode idle [delay]
wt mode ble [delay]
wt mode wifi [delay]
wt mode both [delay]
wt ble on
wt ble off [delay]
wt ble status [json]
wt ble name
wt ble name get
wt ble name set <ble-name>
wt wifi on
wt wifi off [delay]
wt wifi status [json]
wt wifi reconnect
wt wifi scan
wt wifi scan json
wt wifi scan full json
wt wifi scan last [json]
wt wifi scan item <index> [json]
wt wifi scan connect <index> <password> [wpa2|auto|wpa3]
wt wifi scan open <index>
wt wifi scan clear
wt wifi cred list
wt wifi cred set <ssid> <password> [wpa2|auto|wpa3]
wt wifi cred add <ssid> <password> [wpa2|auto|wpa3]
wt wifi cred open <ssid>
wt wifi cred forget <ssid>
wt wifi cred clear
wt wifi cmd status
wt wifi cmd on
wt wifi cmd off
wt wifi cmd port [port]
wt discovery status
wt discovery on
wt discovery off
wt bridge status
wt bridge target <ipv4> <port>
wt bridge ble on|off
wt bridge uart on|off
wt bridge wifi on|off
wt bridge all on|off
wt bridge send <message>
wt led status
wt led test [all|ble|wifi|activity|alert]
wt led pulse [all|ble|wifi|activity|alert]
wt ping
wt ping ble
wt ping wifi <ipv4> <port>
wt tx ble <message>
wt tx uart <message>
wt tx wifi <ipv4> <port> <message>
wt tx both <ipv4> <port> <message>
wt fw status
wt fw reboot [delay]
wt fw reboot bootloader [delay]
```

## Quoted arguments

Use quotes for SSIDs, passwords, names, and payloads with spaces.

```txt
wt wifi cred set "My Home WiFi" "correct horse battery staple" wpa2
wt wifi cred add "Lab WiFi" "password with spaces" auto
wt wifi cred open "Coffee Shop WiFi"
wt wifi cred forget "My Home WiFi"
wt tx ble "hello with spaces"
wt tx uart "quote test: \"hello\""
wt tx wifi 192.168.1.50 5000 "payload with spaces"
```

Backslash escapes are supported inside quoted BLE and UDP command arguments for quotes, backslashes, tabs, carriage returns, and newlines.

## Wi-Fi scan

BLE-safe scan. The scan path waits for Wi-Fi radio readiness and retries briefly if the radio is busy. When Wi-Fi is off, scan uses temporary radio power and turns it back off after the scan. Use `wt wifi reconnect` to associate with stored credentials.

```txt
wt wifi scan json
wt wifi scan item 1 json
wt wifi scan item 2 json
```

Full streamed scan:

```txt
wt wifi scan full json
```

Connect from a scanned AP:

```txt
wt wifi scan connect 1 "wifi password" wpa2
wt wifi status
```

Open AP by scan index:

```txt
wt wifi scan open 1
wt wifi status
```

## LED states

```txt
LED0 solid         BLE client connected
LED0 short off dip  BLE command response or TX notify queued to host
LED0 medium blink  BLE advertising
LED0 heartbeat     BLE ready, idle, not connected
LED0 slow blink    BLE requested before ready
LED1 solid         Wi-Fi associated and IPv4 bound
LED1 fast blink    Wi-Fi scan running
LED1 double pulse  Wi-Fi associated, waiting for IPv4
LED1 slow blink    Wi-Fi requested or associating
LED1 off           Wi-Fi not requested
LED2 pulse         Bridge, UDP TX, discovery, or command activity when led2 exists
LED3 pulse         Alert/error activity when led3 exists
```

Indicator commands:

```txt
wt led status
wt led test all
wt led pulse ble
wt led pulse wifi
wt led pulse activity
wt led pulse alert
```


## Demo flows

BLE sanity:

```txt
wt status
wt ble status
wt config
```

UART to BLE TX:

```txt
wt tx ble "hello from uart"
```

BLE to UART:

```txt
tx uart "hello from ble"
```

BLE-safe scan and item pull:

```txt
wifi scan json
wifi scan last json
wifi scan item 1 json
wifi scan item 2 json
wifi scan connect 1 "wifi password" wpa2
wifi status
```

Full streamed scan:

```txt
wifi scan full json
```

Wi-Fi command listener:

```txt
mode both
wifi reconnect
wifi cmd on
wifi status
```

Wi-Fi UDP command to BLE TX:

```txt
tx ble "hello from wifi command path"
```

Bridge all outputs:

```txt
bridge target 192.168.1.50 5000
bridge all on
bridge send "hello across enabled bridge outputs"
```

Safe delayed cutoff:

```txt
ble off 5s
wifi off 5s
mode ble 5s
```

## BLE characteristics

```txt
Service UUID:             7f1c0001-2b5a-4f2d-9a31-d6a5f4e040e1
TX notify UUID:           7f1c0002-2b5a-4f2d-9a31-d6a5f4e040e1
Status read/notify UUID:  7f1c0003-2b5a-4f2d-9a31-d6a5f4e040e1
Command write UUID:       7f1c0004-2b5a-4f2d-9a31-d6a5f4e040e1
Command response UUID:    7f1c0005-2b5a-4f2d-9a31-d6a5f4e040e1
```

## Wi-Fi UDP command path

```txt
Default board command port: 5001
Node/webapp receive port:  5000
```

Enable and check the UDP command listener:

```txt
wt wifi cmd on
wt wifi cmd status
```

Change the board command port:

```txt
wt wifi cmd port 5002
```
