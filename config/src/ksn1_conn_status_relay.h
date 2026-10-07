#pragma once

/*
 * Custom BLE UUIDs used to relay "is the central connected to a PC/host
 * right now?" from the central half (currently ksn_4_right) to the
 * peripheral half (currently ksn_4_left), so the peripheral can drive
 * status_led accordingly (solid = connected, blinking = not connected).
 *
 * These are private, randomly-generated UUIDs. They only need to:
 *   - match between ksn1_conn_status_relay_central.c and
 *     ksn1_conn_status_relay_peripheral.c (both include this header), and
 *   - not collide with any other GATT service on the same BLE connection.
 * ZMK's own split service (position state, HID indicators, battery, etc.)
 * uses entirely different UUIDs defined in
 * zmk/app/include/zmk/split/bluetooth/uuid.h, so there's no conflict -
 * this is just a second, independent GATT service layered on the same
 * physical link the two halves already use to talk to each other.
 */

#define KSN1_CONN_STATUS_SERVICE_UUID                                                            \
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x8f2a1c00, 0x8b3d, 0x4a1e, 0x9c7a, 0x1d4b6e2f7a01))

#define KSN1_CONN_STATUS_CHAR_UUID                                                               \
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x8f2a1c01, 0x8b3d, 0x4a1e, 0x9c7a, 0x1d4b6e2f7a01))
