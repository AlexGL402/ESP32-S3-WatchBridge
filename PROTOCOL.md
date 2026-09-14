# LJ737(D) / ZT Fit compatible BLE protocol notes

These notes are based on direct testing of an Ultra 2 smartwatch.

## Device information

```text
BLE name: Ultra 2
Model: LJ737(D)
Firmware: V27480
Hardware: LJ737_MB_V1.2
Companion app used by owner: ZT Fit
```

## GATT

Primary command channel:

```text
Service: 6e400f01-b5a3-f393-e0a9-e50e24dcca9d
Write:   6e400002-b5a3-f393-e0a9-e50e24dcca9d
Notify:  6e400003-b5a3-f393-e0a9-e50e24dcca9d
Extra:   6e400004-b5a3-f393-e0a9-e50e24dcca9d
```

Other observed services:

```text
1800 Generic Access
180A Device Information
180F Battery
FEE7
3802
AE00 / AE01 / AE02
```

`AE00 / AE01 / AE02` is not used by this project for notifications and should be treated as a separate OTA path.

## Packet framing

Host-to-watch packets tested here use:

```text
CD LL LL 12 01 CC PP PP [payload...]
```

Where:

- `CD` = frame start
- `LL LL` = frame length minus 3, big-endian
- `12` = protocol/module byte used by tested commands
- `01` = constant in tested requests
- `CC` = command
- `PP PP` = payload length, big-endian

Watch ACKs begin with `DC`.

## Find / vibrate

TX:

```text
CD 00 06 12 01 0B 00 01 01
```

Observed RX:

```text
DC 00 05 12 0B 00 09 01
```

Confirmed: watch vibrates / reacts.

## Enable notifications

TX:

```text
CD 00 11 12 01 07 00 0C 01 01 01 01 01 01 01 01 01 01 01 01
```

Observed RX:

```text
DC 00 05 12 07 00 14 01
```

## Push notification

Command: `0x12`

Payload starts with:

```text
01 00 00
```

Then ASCII/UTF-8 text in the form:

```text
sender:message
```

Verified payload:

```text
MeshCore:TEST FROM ESP32-S3
```

Verified full TX:

```text
CD 00 23 12 01 12 00 1E 01 00 00 4D 65 73 68 43 6F 72 65 3A 54 45 53 54 20 46 52 4F 4D 20 45 53 50 33 32 2D 53 33
```

Observed ACK:

```text
DC 00 05 12 12 00 26 01
```

The notification appeared on the watch.

## MTU / chunking

Negotiated MTU during the test was 200. The 38-byte notification above was accepted in one characteristic write, so 20-byte splitting was not required on this firmware.

## Watch-to-host traffic

After a notification was sent, the watch also emitted packets such as:

```text
CD 00 11 15 01 0C 00 0C 00 00 00 00 00 CF 00 00 00 7C 00 03
CD 00 11 15 01 0C 00 0C 00 00 00 00 00 D8 00 00 00 81 00 03
```

Their meaning is not decoded yet. The standalone firmware exposes the latest RX packet in the web UI so further events can be correlated with button presses, camera controls, media controls, or quick replies.
