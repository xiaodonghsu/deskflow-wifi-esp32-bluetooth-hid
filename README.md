# Deskflow Wi-Fi → ESP32-S3 NimBLE HID

ESP32-S3 connects to Wi-Fi, accepts a Deskflow/Synergy-compatible TCP connection,
and forwards keyboard/mouse events to a paired computer, tablet, or phone over
Bluetooth LE HID.

## Defaults

- Wi-Fi SSID: `GL-MT300N-V2-03d`
- Wi-Fi password: `goodlife`
- Deskflow server: `192.168.41.83:24800`
- Deskflow client screen name: `esp32-hid`
- Advertised virtual screen size: `1920x1080`
- Bluetooth name: `Deskflow ESP32 HID`

Change these values in `main/app_config.h`.

## Build and flash

Requires ESP-IDF 5.x:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Set `APP_DESKFLOW_SERVER_IP` in `main/app_config.h` to the Deskflow server's
LAN address. Pair `Deskflow ESP32 HID` from the destination device's Bluetooth
settings, then add a Deskflow screen named `esp32-hid`. The ESP32 reconnects to
the server automatically.

BLE bonding keys are persisted in NVS. After a HID host goes out of range, the
ESP32 first uses directed reconnect advertising and then falls back to fast
connectable advertising. The phone or tablet must keep Bluetooth enabled and
retain the device in its paired-device list.

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
