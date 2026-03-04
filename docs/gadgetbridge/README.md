# Kerfur Gadgetbridge Scaffold

This folder documents the BLE companion protocol that the firmware now exposes for Android/Gadgetbridge.

## Firmware Side (already implemented)

Service UUID (Kerfur Companion):
- `4a7b1001-1e1a-4d52-a2ef-14bb5f420001`

Characteristics:
- RX Write: `4a7b1002-1e1a-4d52-a2ef-14bb5f420001`
- TX Notify/Read: `4a7b1003-1e1a-4d52-a2ef-14bb5f420001`

Legacy debug service still exists in firmware for test event injection.

## Protocol v1

All packets are small (<= 20 bytes) and start with opcode byte.

Phone -> Kerfur (write to RX):
- `0x01` : Ping
- `0x10 <category>` : Android notification event
  - `<category>` is one byte (0..255)
  - optional bytes after category may carry short preview text

Kerfur -> Phone (notify on TX):
- `0x81 0x01` : Pong + protocol version (`1`)
- `0x82 <category>` : Notification ACK
- `0x90 <category>` : Test notification (triggered by shell command)
- `0xE0 <error> <opcode>` : Error response

Current firmware behavior:
- `0x10` packet publishes `APP_EVENT_PHONE_NOTIFICATION` in firmware.
- ANCS notifications from iPhone also publish `APP_EVENT_PHONE_NOTIFICATION`.

## Gadgetbridge Integration Outline

1. Add Kerfur device coordinator + support classes in your Gadgetbridge fork.
2. Scan filter should match either:
   - Companion service UUID above, or
   - Device name prefix (for fallback).
3. On connection:
   - Discover services.
   - Enable notifications on TX characteristic.
   - Send ping (`0x01`) and expect `0x81 0x01`.
4. On Android notification event in Gadgetbridge support class:
   - Convert to category byte.
   - Write `0x10 <category>` to RX.
5. Optionally include short text preview in same packet (truncate to fit ATT payload).

## Firmware Test Commands

From USB shell on device:
- `kerfur ble gb_status`
- `kerfur ble gb_test`
- `kerfur ble gb_test 3`

These commands verify that TX notifications are enabled and packet flow to phone app works.
