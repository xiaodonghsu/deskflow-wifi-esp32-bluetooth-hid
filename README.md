# Deskflow Wi-Fi → ESP32-S3 NimBLE HID

ESP32-S3 connects to Wi-Fi and opens a numbered Deskflow/Synergy-compatible TCP
client session only when the corresponding Bluetooth HID host is connected.
Each screen's keyboard/mouse events are forwarded to its paired computer,
tablet, or phone.

## Defaults

在 [app_config.h](main\app_config.h) 中配置.

- Wi-Fi SSID: `GL-MT300N-V2-03d`
- Wi-Fi password: `goodlife`
- Deskflow server: `192.168.41.83:24800`
- Configuration SoftAP: `esp32-hid-config` (no password)
- Deskflow client screens: `esp32-hid-1`, `esp32-hid-2`, `esp32-hid-3`
- Advertised virtual screen size: `1920x1080`
- Bluetooth name: `Deskflow ESP32 HID`
- Maximum simultaneous HID hosts: `3`

Change these values in `main/app_config.h`.

## Wi-Fi configuration portal

At boot the ESP32 runs a SoftAP alongside its normal Wi-Fi station connection.
The default SoftAP is:

```text
esp32-hid-config
```

Connect to that network and open the configuration page:

```text
http://192.168.1.100/
```

The page contains communication settings (Wi-Fi credentials, Deskflow server
IPv4 address and port, SoftAP credentials, and BLE device name) plus an
individual Deskflow screen name, width, and height for every HID slot. Press
**Save and restart device** to validate and persist the form in NVS. Saved
values override the compile-time defaults in `main/app_config.h`.

The SoftAP password is optional. If it is non-empty it must contain 8–63
characters; leaving it empty creates an open configuration network.

## Build and flash

Requires ESP-IDF 6.x:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

The project uses ESP-IDF's 1.5 MiB single-factory-app partition table. OTA
slots are not enabled; NVS remains available for Wi-Fi parameters, BLE bonds,
and HID target mappings.

Set compile-time defaults in `main/app_config.h`, or change them on the
configuration page. Add the configured HID screen names to the Deskflow Server
layout, then pair the configured BLE device name from each destination device's
Bluetooth settings. The first paired host maps to HID slot 1, the second to
slot 2, and the third to slot 3:

```text
esp32-hid-1 -> first BLE host
esp32-hid-2 -> second BLE host
esp32-hid-3 -> third BLE host
```

The screen-to-peer address mapping and BLE bonding keys are persisted in NVS.
The ESP32 continues fast connectable advertising until all three slots are
connected, and resumes advertising whenever any target disconnects. The phone
or tablet must keep Bluetooth enabled and retain the device in its paired-device
list.

No Deskflow sessions are created at boot while all HID targets are offline.
Connecting target 1 creates only `esp32-hid-1`; connecting target 2 then creates
`esp32-hid-2`. When a HID target disconnects, its Deskflow TCP session is closed
within approximately one second and is recreated with the same numbered name
when that target reconnects.

## Implemented protocol subset

The bridge understands length-prefixed Deskflow/Synergy protocol frames and
handles keyboard down/up/repeat, absolute and relative pointer motion, mouse
buttons, vertical wheel, screen enter/leave, screen-information queries,
keep-alives, and the `Barrier` or `Synergy` handshake prefixes selected by
Deskflow Server. Protocol 1.8 language-aware keyboard-down (`DKDL`) frames are
also supported. After each handled server message, the client sends a `CNOP`
reply with `TCP_NODELAY` enabled to avoid interactive latency from delayed TCP
acknowledgments.

Clipboard, drag-and-drop, horizontal scrolling, dead keys, Unicode text input,
and consumer/media keys are not currently forwarded. Keyboard mapping covers
ASCII letters/numbers, common controls, navigation keys, and modifier masks.
