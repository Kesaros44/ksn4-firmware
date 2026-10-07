/*
 * Drives status_led (physically on the peripheral/left half, see
 * ksn_4_left.overlay) to show whether the CENTRAL half (currently
 * ksn_4_right) is connected to a PC/host - NOT whether the two keyboard
 * halves are linked to each other (that was the previous behavior, via
 * zmk-poor-mans-led-indicator's CONFIG_INDICATOR_LED_SHOW_PERIPHERAL_BLE,
 * now disabled in ksn_4_left.conf).
 *
 * Solid on   = central has an active host connection (USB or BLE), and at
 *              least KSN1_PROFILE_MIN_CYCLES blink-cycles have already
 *              played since the last profile switch / disconnect (see
 *              below) - never jumps straight to solid without that.
 * Blinking   = no active host connection: blinks out (active profile
 *              index + 1) short pulses, pauses, then repeats for as long
 *              as disconnected. This doubles as "which profile am I on"
 *              feedback - switching to an already-paired profile that
 *              reconnects almost instantly still plays at least
 *              KSN1_PROFILE_MIN_CYCLES full cycles before settling solid,
 *              so a fast reconnect can't rob the user of the readout.
 *
 * WHY THIS NEEDS ITS OWN GATT SERVICE
 * ------------------------------------
 * ZMK's only built-in central->peripheral data channel is HID indicators
 * (CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS, already used by
 * ksn1_peripheral_indicators.c for the caps lock LED on this same board).
 * That channel is populated exclusively from real host Set_Report(Output)
 * data deep inside ZMK core (app/src/hid_indicators.c) and has no public
 * setter we could call from an out-of-tree file to inject a synthetic
 * "host connected" bit - so it can't be reused here. zmk-poor-mans-
 * led-indicator's SHOW_PERIPHERAL_BLE option only ever had access to the
 * peripheral<->central link state for the same underlying reason: from
 * the peripheral, central-only functions like zmk_endpoint_is_connected()
 * simply aren't linkable (see the comment at the top of
 * ksn1_peripheral_indicators.c).
 *
 * So instead, this file registers one small extra GATT service (its own
 * private UUIDs, see ksn1_conn_status_relay.h) directly on top of the SAME
 * BLE connection the two halves already use for ZMK's split protocol.
 * ksn1_conn_status_relay_central.c (built only on ksn_4_right) writes a
 * 2-byte payload ([0]=host connected, [1]=active BLE profile index) to
 * this characteristic whenever either value changes (plus a periodic
 * resend regardless, for self-healing - see KSN1_RESEND_TICKS there).
 * This file receives those bytes and drives the LED accordingly.
 *
 * Only builds on the peripheral (mirrors the guard used in
 * ksn1_peripheral_indicators.c) and only if the status_led alias exists.
 */

#include <zephyr/devicetree.h>

#if !defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && DT_NODE_EXISTS(DT_ALIAS(status_led))

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/led.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "ksn1_conn_status_relay.h"

LOG_MODULE_REGISTER(ksn1_conn_status_relay_peripheral, CONFIG_ZMK_LOG_LEVEL);

/* Faster blink used while we have never received a single byte from the
 * central. This is a deliberate diagnostic: it can't be confused with the
 * profile-count cycle below, so the board itself says "delivery path is
 * broken" with no serial log needed:
 *   fast (100ms) = nothing ever arrived from the central -> delivery path
 *   profile-count cycle (see below) = central is talking to us, this is
 *                  its actual reported state
 * Once the first byte arrives this is never used again for the rest of
 * the session. */
#define KSN1_CONN_STATUS_SILENT_MS 100

/* Profile-count blink cycle, played whenever there is no active host
 * connection: (active profile index + 1) pulses of ON_MS/OFF_MS, then a
 * CYCLE_PAUSE_MS gap, then repeat for as long as disconnected. The "+1"
 * is so profile 0 still blinks once instead of looking identical to "no
 * signal at all". MIN_CYCLES is the minimum number of full cycles played
 * after any profile switch (or a drop from a previously-solid state)
 * before "connected" is allowed to turn the LED solid - without this, an
 * already-paired profile that reconnects in well under a second would
 * flash the count too briefly to read, or not at all. */
#define KSN1_PROFILE_BLINK_ON_MS 150
#define KSN1_PROFILE_BLINK_OFF_MS 150
#define KSN1_PROFILE_CYCLE_PAUSE_MS 700
#define KSN1_PROFILE_MIN_CYCLES 5

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t status_led_idx = DT_NODE_CHILD_IDX(DT_ALIAS(status_led));

static bool host_connected;
static bool ever_heard_from_central;
static bool have_applied_once;
static bool led_phys_on;

/* Profile-count cycle state. */
static uint8_t current_profile;
static uint8_t blink_index;   /* which pulse within the current cycle (0-based) */
static bool blink_on_phase;   /* mid-pulse: currently in the "on" half? */
static bool in_pause;         /* between cycles, waiting out CYCLE_PAUSE_MS */
static uint8_t cycles_played; /* full cycles completed since the last reset */
static bool settled_solid;    /* true once MIN_CYCLES was met and we've gone solid */

static struct k_work_delayable blink_work;

static void set_led(bool on) {
    led_phys_on = on;
    if (on) {
        led_on(led_dev, status_led_idx);
    } else {
        led_off(led_dev, status_led_idx);
    }
}

static void start_cycle(void) {
    blink_index = 0;
    blink_on_phase = true;
    in_pause = false;
    set_led(true);
    k_work_reschedule(&blink_work, K_MSEC(KSN1_PROFILE_BLINK_ON_MS));
}

static void blink_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!ever_heard_from_central) {
        /* Delivery-path diagnostic flicker - see KSN1_CONN_STATUS_SILENT_MS. */
        set_led(!led_phys_on);
        k_work_reschedule(&blink_work, K_MSEC(KSN1_CONN_STATUS_SILENT_MS));
        return;
    }

    if (in_pause) {
        /* A cycle just finished. Only now do we check whether we're
         * allowed to settle solid - never mid-pulse, so a connect event
         * arriving mid-blink can't cut a pulse short and make it
         * unreadable. */
        if (host_connected && cycles_played >= KSN1_PROFILE_MIN_CYCLES) {
            settled_solid = true;
            set_led(true);
            return; /* steady on - nothing left to reschedule */
        }
        start_cycle();
        return;
    }

    if (blink_on_phase) {
        set_led(false);
        blink_on_phase = false;
        k_work_reschedule(&blink_work, K_MSEC(KSN1_PROFILE_BLINK_OFF_MS));
        return;
    }

    /* Finished one full pulse (on+off). One cycle = (current_profile + 1)
     * pulses, so profile 0 still blinks once instead of looking like "no
     * signal". */
    if (++blink_index >= (uint8_t)(current_profile + 1)) {
        cycles_played++;
        in_pause = true;
        set_led(false);
        k_work_reschedule(&blink_work, K_MSEC(KSN1_PROFILE_CYCLE_PAUSE_MS));
        return;
    }

    blink_on_phase = true;
    set_led(true);
    k_work_reschedule(&blink_work, K_MSEC(KSN1_PROFILE_BLINK_ON_MS));
}

static void apply_state(bool connected, uint8_t profile) {
    bool profile_changed = (profile != current_profile);
    /* A drop from an already-solid display is treated the same as a
     * profile switch: restart the count from zero so the next connect
     * (to the same or a different profile) always gets a full readout,
     * not just whatever cycle happened to be mid-flight. */
    bool fresh_drop = (settled_solid && !connected);
    bool force_restart = !have_applied_once || profile_changed || fresh_drop;

    have_applied_once = true;
    current_profile = profile;
    host_connected = connected;

    if (force_restart) {
        cycles_played = 0;
        settled_solid = false;
        k_work_cancel_delayable(&blink_work);
        start_cycle();
        return;
    }

    if (settled_solid) {
        set_led(true);
    }
    /* Otherwise a cycle is already running (or about to enter its pause) -
     * it picks up the latest host_connected/cycles_played on its own at
     * the next pause boundary, nothing to do here. */
}

static ssize_t on_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                        uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len < 2) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    ever_heard_from_central = true;

    const uint8_t *bytes = buf;
    apply_state(bytes[0] != 0, bytes[1]);
    return len;
}

/* Declared as plain struct instances (not passed inline) so that the
 * brace-init-list produced by KSN1_CONN_STATUS_*_UUID never has to flow
 * through another macro's argument list - the C preprocessor only tracks
 * parentheses (not braces) when splitting macro arguments, so passing the
 * brace-init form directly into BT_GATT_PRIMARY_SERVICE/BT_GATT_CHARACTERISTIC
 * causes the commas inside the UUID byte array to be misread as extra
 * arguments. Taking the address of a named variable sidesteps that. */
static const struct bt_uuid_128 ksn1_conn_status_svc_uuid = KSN1_CONN_STATUS_SERVICE_UUID;
static const struct bt_uuid_128 ksn1_conn_status_char_uuid = KSN1_CONN_STATUS_CHAR_UUID;

BT_GATT_SERVICE_DEFINE(ksn1_conn_status_svc,
                        BT_GATT_PRIMARY_SERVICE(&ksn1_conn_status_svc_uuid.uuid),
                        BT_GATT_CHARACTERISTIC(&ksn1_conn_status_char_uuid.uuid,
                                                BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                                BT_GATT_PERM_WRITE_ENCRYPT, NULL, on_write, NULL));

static int ksn1_conn_status_relay_peripheral_init(void) {
    k_work_init_delayable(&blink_work, blink_work_handler);

    /* Assume "not connected" (blinking) until the central actually tells
     * us otherwise - matches reality on cold boot, before the split link
     * and the host link are both up. */
    host_connected = false;
    /* Fast diagnostic flicker until the first byte ever arrives from
     * central - blink_work_handler's `!ever_heard_from_central` branch
     * keeps rescheduling at this same rate until then. */
    set_led(true);
    k_work_reschedule(&blink_work, K_MSEC(KSN1_CONN_STATUS_SILENT_MS));

    return 0;
}

SYS_INIT(ksn1_conn_status_relay_peripheral_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* !CONFIG_ZMK_SPLIT_ROLE_CENTRAL && status_led alias exists */
