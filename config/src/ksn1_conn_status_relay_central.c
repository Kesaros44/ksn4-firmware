/*
 * Central-side counterpart to ksn1_conn_status_relay_peripheral.c.
 *
 * Every KSN1_POLL_MS, checks whether this half (currently ksn_4_right,
 * the split central) has an active connection to a PC/host, and which BLE
 * profile is currently selected (zmk_ble_active_profile_index()) - and
 * writes both as a 2-byte payload ([0]=connected, [1]=profile index) to
 * the custom GATT characteristic the peripheral (left half) exposes for
 * this, whenever either value changes (or periodically regardless, see
 * KSN1_RESEND_TICKS). The peripheral uses byte [1] to blink out the
 * active profile number on status_led while not connected. See
 * ksn1_conn_status_relay_peripheral.c for the full rationale on why a
 * dedicated GATT service is used instead of piggybacking on HID
 * indicators, and for the blink-pattern logic itself.
 *
 * FINDING THE RIGHT BLE CONNECTION
 * ---------------------------------
 * This half has up to two simultaneous BLE connections:
 *   1. To the PC/host - on that link we are the GATT SERVER (peripheral
 *      role), since we present as a HID keyboard to it.
 *   2. To the other keyboard half - on that link we are the GATT CLIENT
 *      (central role), since we're the one discovering the peripheral's
 *      services (ZMK's own split protocol works the same way).
 * bt_conn_get_info()->role tells these apart without touching any
 * ZMK-internal split state: BT_CONN_ROLE_CENTRAL only ever matches
 * connection #2.
 *
 * Only builds on the central (currently ksn_4_right).
 *
 * NOTE: as of the ZMK version this is built against, zmk_endpoint_is_connected()
 * takes no arguments - it reports the connection state of whichever
 * endpoint is currently selected internally. (Earlier drafts of this file
 * called it as zmk_endpoint_is_connected(zmk_endpoints_selected()), which
 * matched an older/incorrect signature and no longer builds.) If this
 * breaks again on a future ZMK update, check the current
 * zmk/app/include/zmk/endpoints.h for the exact signature and adjust -
 * the rest of this file doesn't need to change.
 */

#include <zephyr/devicetree.h>

#if defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include <zmk/endpoints.h>
#include <zmk/ble.h>

#include "ksn1_conn_status_relay.h"

LOG_MODULE_REGISTER(ksn1_conn_status_relay_central, CONFIG_ZMK_LOG_LEVEL);

#define KSN1_POLL_MS 250
/* Wait this long after (re)detecting the split link before starting our
 * own GATT discovery. ZMK's own split central code (central.c) also runs
 * its discovery right after a (re)connection - position state, HID
 * indicators, battery level, physical layout, etc. - and that's on the
 * critical path for the keyboard actually working again. Our discovery is
 * just for an LED and isn't urgent, but competing for the same
 * single-outstanding-request-per-connection ATT bearer at exactly the
 * same moment was slowing that down, which is why reconnects got
 * noticeably slower after this file was added. Deferring ours fixes it. */
#define KSN1_DISCOVERY_DELAY_MS 3000
/* If a discovery attempt finds nothing (e.g. the shared ATT bearer was busy
 * with ZMK's own split discovery at the same moment - see comment above),
 * retry after this delay instead of giving up until the bt_conn object
 * itself changes. Without this, a single failed attempt left status_led
 * stuck blinking indefinitely even though the split link was fine - this
 * was the cause of "LED keeps blinking while wired via USB". */
#define KSN1_DISCOVERY_RETRY_MS 2000
/* Watchdog: if the split link is up but we still have no characteristic
 * handle this long after the last attempt, start discovery again from the
 * poll tick. The two retry paths above only fire when they are actually
 * reached - a discovery that starts cleanly but whose callback never
 * arrives (link dropped mid-discovery, work item cancelled, ATT request
 * lost) leaves discovery_done false with nothing scheduled, and the LED
 * then blinks forever. This tick-driven check does not care why the
 * previous attempt stalled. */
#define KSN1_DISCOVERY_WATCHDOG_MS 5000
/* Re-send the current state every this many poll ticks even when nothing
 * changed. bt_gatt_write_without_response() is an ATT Write Command: there
 * is no response, so a write the peripheral silently discards (e.g. the
 * characteristic is BT_GATT_PERM_WRITE_ENCRYPT and link encryption hasn't
 * settled yet) still looks like a success here. Sending only on state
 * change meant one lost write left status_led blinking forever, since
 * have_sent is otherwise only cleared when the bt_conn object itself
 * changes. This showed up most on USB, where zmk_endpoint_is_connected()
 * goes true within a second of boot - so the first write lands at the
 * earliest, least settled moment. A periodic re-send makes the relay
 * self-healing regardless of why a write was lost; apply_state() on the
 * peripheral ignores a value that matches what it already has. */
#define KSN1_RESEND_TICKS 8 /* 8 * 250ms = every 2s */

static struct bt_conn *peripheral_conn;
static uint16_t char_value_handle;
static bool discovery_done;
static bool have_sent;
static bool last_sent_state;
static uint8_t last_sent_profile;
static uint8_t resend_ticks;

static struct bt_gatt_discover_params discover_params;
static struct bt_uuid_128 discover_svc_uuid = KSN1_CONN_STATUS_SERVICE_UUID;
static struct bt_uuid_128 discover_char_uuid = KSN1_CONN_STATUS_CHAR_UUID;

static struct k_work_delayable poll_work;
static struct k_work_delayable discovery_start_work;
static int64_t last_discovery_attempt;

static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params) {
    ARG_UNUSED(conn);

    if (!attr) {
        /* Nothing found this pass. refresh_peripheral_conn() only restarts
         * discovery when the bt_conn object itself changes, so without an
         * explicit retry here, a single failed lookup (e.g. ATT bearer busy)
         * would leave status_led stuck blinking forever even though the
         * split link never actually dropped. Retry from the top instead. */
        LOG_WRN("ksn1_conn_status: discovery step found nothing, retrying in %dms",
                KSN1_DISCOVERY_RETRY_MS);
        if (peripheral_conn) {
            k_work_reschedule(&discovery_start_work, K_MSEC(KSN1_DISCOVERY_RETRY_MS));
        }
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        discover_params.uuid = &discover_char_uuid.uuid;
        discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
        discover_params.start_handle = attr->handle + 1;
        discover_params.end_handle = 0xffff;
        bt_gatt_discover(conn, &discover_params);
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        char_value_handle = bt_gatt_attr_value_handle(attr);
        discovery_done = true;
        LOG_INF("ksn1_conn_status: found peripheral char, handle %d", char_value_handle);
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_STOP;
}

static void start_discovery(struct bt_conn *conn) {
    discovery_done = false;
    char_value_handle = 0;
    last_discovery_attempt = k_uptime_get();

    discover_params.uuid = &discover_svc_uuid.uuid;
    discover_params.func = discover_func;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(conn, &discover_params);
    if (err) {
        /* The callback path already retries when a discovery pass finds
         * nothing, but until this was added, an error returned by
         * bt_gatt_discover() itself (e.g. -EBUSY while ZMK's own split
         * discovery still holds the single-outstanding-request ATT bearer)
         * only logged a warning and scheduled nothing. discovery_done then
         * stayed false forever, send_state() early-returned on every poll
         * tick, and status_led blinked indefinitely. Retry from the top. */
        LOG_WRN("ksn1_conn_status: discovery start failed (%d), retrying in %dms", err,
                KSN1_DISCOVERY_RETRY_MS);
        if (peripheral_conn) {
            k_work_reschedule(&discovery_start_work, K_MSEC(KSN1_DISCOVERY_RETRY_MS));
        }
    }
}

static void discovery_start_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (peripheral_conn) {
        start_discovery(peripheral_conn);
    }
}

static bool conn_is_peripheral_link(struct bt_conn *conn) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0) {
        return false;
    }
    return info.type == BT_CONN_TYPE_LE && info.role == BT_CONN_ROLE_CENTRAL;
}

static void find_conn_cb(struct bt_conn *conn, void *data) {
    struct bt_conn **out = data;
    if (*out) {
        return; /* already found one */
    }
    if (conn_is_peripheral_link(conn)) {
        /* bt_conn_foreach() only guarantees `conn` is valid for the
         * duration of this callback - take our own ref so it's still
         * valid once we get back to refresh_peripheral_conn() and beyond.
         * refresh_peripheral_conn() is responsible for unref'ing this. */
        *out = bt_conn_ref(conn);
    }
}

/* Whether `a` and `b` represent the same underlying BLE link, by remote
 * address rather than raw pointer identity. Zephyr's connection pool is a
 * small fixed array of struct bt_conn slots that gets reused - after a
 * connection is freed (e.g. the peripheral was gone for a long time), a
 * later unrelated connection can be handed back the exact same pointer
 * value. A plain `found == peripheral_conn` check would then wrongly treat
 * a brand-new link as "unchanged" and skip re-discovery, leaving
 * char_value_handle pointing at a handle from the OLD connection - writes
 * to it fail silently (Write Without Response has no error path back to
 * us), so status_led gets stuck showing whatever state was last
 * successfully relayed before the long disconnect. Comparing by address
 * survives pointer reuse. */
static bool same_conn(struct bt_conn *a, struct bt_conn *b) {
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    struct bt_conn_info info_a, info_b;
    if (bt_conn_get_info(a, &info_a) != 0 || bt_conn_get_info(b, &info_b) != 0) {
        return false;
    }
    return bt_addr_le_cmp(info_a.le.dst, info_b.le.dst) == 0;
}

static void refresh_peripheral_conn(void) {
    struct bt_conn *found = NULL;
    bt_conn_foreach(BT_CONN_TYPE_LE, find_conn_cb, &found);

    /* bt_conn_foreach() only guarantees `found` is valid for the duration
     * of that call - it does NOT hand us a reference we can keep around
     * across poll ticks. Take our own reference before storing it, and
     * drop our old one whenever it's no longer the current link, or we'll
     * end up holding (and later dereferencing / writing to) a stale
     * connection object once it's freed elsewhere. This was causing
     * random disconnects. */

    if (same_conn(found, peripheral_conn)) {
        if (found) {
            bt_conn_unref(found); /* drop the extra ref bt_conn_foreach gave us */
        }
        return;
    }

    if (peripheral_conn) {
        bt_conn_unref(peripheral_conn);
    }

    peripheral_conn = found; /* already ref'd via bt_conn_foreach, if non-NULL */
    discovery_done = false;
    char_value_handle = 0;
    have_sent = false;

    if (peripheral_conn) {
        /* Don't discover right away - see KSN1_DISCOVERY_DELAY_MS comment. */
        k_work_reschedule(&discovery_start_work, K_MSEC(KSN1_DISCOVERY_DELAY_MS));
    } else {
        k_work_cancel_delayable(&discovery_start_work);
    }
}

static void send_state(bool connected, uint8_t profile) {
    if (!peripheral_conn || !discovery_done || char_value_handle == 0) {
        return;
    }

    uint8_t val[2] = {connected ? 1 : 0, profile};
    int err = bt_gatt_write_without_response(peripheral_conn, char_value_handle, val,
                                              sizeof(val), false);
    if (err) {
        LOG_WRN("ksn1_conn_status: write failed (%d)", err);
        return;
    }
    last_sent_state = connected;
    last_sent_profile = profile;
    have_sent = true;
}

static void poll_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    refresh_peripheral_conn();

    /* See KSN1_DISCOVERY_WATCHDOG_MS. Skipped while the initial delayed
     * start (or a scheduled retry) is still pending, so this only kicks in
     * when nothing else is going to. */
    if (peripheral_conn && !discovery_done && !k_work_delayable_is_pending(&discovery_start_work) &&
        (k_uptime_get() - last_discovery_attempt) > KSN1_DISCOVERY_WATCHDOG_MS) {
        LOG_WRN("ksn1_conn_status: still no handle after %dms, restarting discovery",
                KSN1_DISCOVERY_WATCHDOG_MS);
        start_discovery(peripheral_conn);
    }

    bool connected = zmk_endpoint_is_connected();
    uint8_t profile = (uint8_t)zmk_ble_active_profile_index();

    if (!have_sent || connected != last_sent_state || profile != last_sent_profile ||
        ++resend_ticks >= KSN1_RESEND_TICKS) {
        resend_ticks = 0;
        send_state(connected, profile);
    }

    k_work_reschedule(&poll_work, K_MSEC(KSN1_POLL_MS));
}

static int ksn1_conn_status_relay_central_init(void) {
    k_work_init_delayable(&poll_work, poll_work_handler);
    k_work_init_delayable(&discovery_start_work, discovery_start_work_handler);
    k_work_reschedule(&poll_work, K_MSEC(KSN1_POLL_MS));
    return 0;
}

SYS_INIT(ksn1_conn_status_relay_central_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
