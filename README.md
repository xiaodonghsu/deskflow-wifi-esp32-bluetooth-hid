# Deskflow Wi-Fi → ESP32-S3 NimBLE HID

ESP32-S3 connects to Wi-Fi, opens multiple Deskflow/Synergy-compatible TCP
client sessions, and forwards each screen's keyboard/mouse events to its
corresponding paired computer, tablet, or phone over Bluetooth LE HID.

## Defaults

在 [app_config.h](main\app_config.h) 中配置.

- Wi-Fi SSID: `309_MeetingRoom`
- Wi-Fi password: `bestlink309`
- Deskflow server: `172.30.124.109:24800`
- Deskflow client screens: `esp32-hid-1`, `esp32-hid-2`, `esp32-hid-3`
- Advertised virtual screen size: `1920x1080`
- Bluetooth name: `Deskflow ESP32 HID`
- Maximum simultaneous HID hosts: `3`

Change these values in `main/app_config.h`.

## Build and flash

Requires ESP-IDF 6.x:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Set `APP_DESKFLOW_SERVER_IP` in `main/app_config.h` to the Deskflow server's
LAN address. Add all numbered screens to the Deskflow Server layout, then pair
`Deskflow ESP32 HID` from each destination device's Bluetooth settings. The
first paired host maps to screen 1, the second to screen 2, and the third to
screen 3:

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

## Implemented protocol subset

The bridge understands length-prefixed Deskflow/Synergy protocol frames and
handles keyboard down/up/repeat, absolute and relative pointer motion, mouse
buttons, vertical wheel, screen enter/leave, screen-information queries,
keep-alives, and the `Barrier` or `Synergy` handshake prefixes selected by
Deskflow Server. Protocol 1.8 language-aware keyboard-down (`DKDL`) frames are
also supported.

Clipboard, drag-and-drop, horizontal scrolling, dead keys, Unicode text input,
and consumer/media keys are not currently forwarded. Keyboard mapping covers
ASCII letters/numbers, common controls, navigation keys, and modifier masks.

## future feature

考虑同时支持多个HID设备
