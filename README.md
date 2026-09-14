# ESP32-S3 WatchBridge

Standalone ESP32-S3 BLE bridge/probe for cheap smartwatches that use the LJ737(D) / FitPro-style `0xCD` protocol.

The project was created from a real hardware test with an **Ultra 2** watch connected by the **ZT Fit** app.

## Verified watch

- Advertised name: `Ultra 2`
- Test MAC: `dd:63:d9:2c:b5:bb`
- Model: `LJ737(D)`
- Firmware: `V27480`
- Hardware: `LJ737_MB_V1.2`
- Companion app: ZT Fit
- BLE MTU observed: 200

## Verified BLE channel

- Service: `6e400f01-b5a3-f393-e0a9-e50e24dcca9d`
- Write: `6e400002-b5a3-f393-e0a9-e50e24dcca9d`
- Notify: `6e400003-b5a3-f393-e0a9-e50e24dcca9d`

Verified commands:

- Find/vibrate
- Enable notification categories
- Push text notification
- Watch ACK/RX capture

A 38-byte notification packet was accepted as one GATT write with MTU 200 and appeared on the watch as a `MeshCore` notification.

## Current features

The ESP32-S3 runs its own Wi-Fi AP and a tiny web UI:

- BLE scanner
- list device name / MAC / RSSI
- bind a watch and save its MAC in NVS
- connect / reconnect / unbind
- Find / Vibrate test
- Enable notifications test
- Send TEST notification
- send arbitrary sender + text
- display the last BLE packet received from the watch
- serial test commands remain available

This makes it easy to test a second watch without recompiling the firmware.

## Wi-Fi / Web UI

After boot connect to:

```text
SSID: WatchBridge-S3
Password: 12345678
```

Open:

```text
http://192.168.4.1
```

## Build

PlatformIO:

```powershell
pio run
pio run -t upload
pio device monitor
```

Default environment:

```text
esp32-s3-devkitc-1
```

## Serial commands

```text
F  Find/vibrate bound watch
E  Enable notifications
T  Send test notification
S  Run BLE scan
C  Connect bound watch
```

## Protocol status

See [PROTOCOL.md](PROTOCOL.md).

Important: service `AE00 / AE01 / AE02` was also observed on the tested watch and appears to be a JieLi OTA path. This project does **not** use it for normal notification transport.

## Reverse direction

The watch already sends BLE notifications back to the S3 on `6e400003`, so the project records those packets. ACK packets are confirmed. Whether buttons, camera controls, media actions, or quick replies can be used as a meaningful reverse channel still needs testing.

Arbitrary typed replies are unlikely unless the stock watch firmware exposes a reply UI. Preset quick replies may still be possible if the watch emits a command packet for them.

## Planned integration

Once the standalone bridge is stable, the useful part can be moved into ZephCore/ZephorS3 as an optional `WatchBridge` module:

```text
MeshCore RX -> ESP32-S3 -> BLE GATT -> smartwatch notification
smartwatch event -> BLE notify -> ESP32-S3 -> optional MeshCore action
```
