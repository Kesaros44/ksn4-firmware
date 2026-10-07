/*
 * "단어 되돌리기" (word-flip) 트리거 behavior.
 *
 * 목적: 방금 입력한 단어가 한/영 오타(다른 언어 모드로 잘못 입력됨)일 때,
 * 사용자가 직접 판단하고 트리거 키를 눌러 그 단어를 지우고 반대 언어
 * 모드로 재입력한다. 호스트의 현재 IME 상태를 알 필요가 없다 - 무조건
 * "반대로 뒤집기"만 하므로 상태 추정이 틀릴 일이 없다 (사람이 트리거
 * 시점을 판단하기 때문).
 *
 * 동작:
 *   1. 이 파일의 리스너가 모든 A-Z 키 입력을 최근 순서대로 버퍼에 담아둔다.
 *      스페이스/엔터/탭/그 외 A-Z가 아닌 키가 눌리면 버퍼를 비운다(단어
 *      경계).
 *   2. 트리거 키(&word_flip <os>)를 누르면:
 *      a. 현재 버퍼를 로컬로 복사하고 전역 버퍼를 즉시 비운다.
 *      b. 한/영 전환 전송 - Windows(param1=0)는 LANG1, macOS(param1=1)는
 *         Caps Lock. 삭제보다 먼저 보내야 한다(아래 on_word_flip_binding_
 *         pressed의 순서 주석 참고).
 *      c. 단어 삭제 조합 전송 - Windows는 Ctrl+Backspace, macOS는
 *         Option+Backspace.
 *      d. 복사해둔 버퍼를 그대로 순서대로 재입력.
 *
 * 전부 zmk_behavior_queue_add()로 큐에 넣는다 - ZMK의 매크로 behavior가
 * 쓰는 것과 동일한 공식 비동기 큐 API라서, 이 파일이 어떤 스레드
 * 컨텍스트에서 호출되든 블로킹 없이 안전하게 순서대로 처리된다.
 *
 * 주의(2026-09-05): 이 세션에 west 빌드 툴체인이 없어 실제 컴파일 검증을
 * 못 했음. ZMK 공식 문서와 이 저장소에 이미 있는 검증된 코드(calc_macro,
 * ksn1_conn_status_relay 등)의 실제 API를 최대한 그대로 재사용했지만,
 * GitHub Actions 빌드 결과를 반드시 먼저 확인할 것.
 */

#define DT_DRV_COMPAT ksn_behavior_word_flip

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if (!defined(CONFIG_ZMK_SPLIT) || defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) && \
    DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define WORD_FLIP_MAX_LEN 24
#define WORD_FLIP_TAP_MS 40
#define WORD_FLIP_WAIT_MS 30
/* macOS는 한/영 전환에 Caps Lock을 쓰는데(시스템 설정 "Caps Lock 키로 ABC
 * 입력 소스 전환"), 이 전환은 즉시 반영되지 않고 수백 ms 지연이 있는 것으로
 * 잘 알려져 있다. Windows의 LANG1처럼 30ms 뒤에 바로 다음 키를 보내면 아직
 * 이전 입력 모드인 상태에서 삭제/재입력이 들어가 버린다. 그래서 맥에서는
 * 전환 키 뒤에만 넉넉히 기다린다. */
#define WORD_FLIP_MAC_TOGGLE_WAIT_MS 350

struct word_flip_key {
    uint32_t keycode;
    uint8_t explicit_modifiers;
};

static struct word_flip_key buffer[WORD_FLIP_MAX_LEN];
static size_t buffer_len;

/* 주의: 이벤트의 ev->keycode는 usage page가 빠진 순수 usage ID(A=0x04 ...
 * Z=0x1D)다. 반면 keys.h의 A/Z 매크로는 ZMK_HID_USAGE(page, id)로 page가
 * 상위 비트에 붙은 값(A=0x70004)이라 그대로 비교하면 절대 참이 되지 않는다.
 * page는 ev->usage_page로 따로 확인하고, 범위 비교는 ZMK_HID_USAGE_ID()로
 * 벗겨낸 ID끼리 해야 한다. */
static bool is_letter(uint16_t usage_page, uint32_t keycode) {
    return usage_page == HID_USAGE_KEY && keycode >= ZMK_HID_USAGE_ID(A) &&
           keycode <= ZMK_HID_USAGE_ID(Z);
}

static int word_flip_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE; /* release는 무시, press만 본다 */
    }

    if (is_letter(ev->usage_page, ev->keycode)) {
        if (buffer_len < WORD_FLIP_MAX_LEN) {
            buffer[buffer_len].keycode = ev->keycode;
            buffer[buffer_len].explicit_modifiers = ev->explicit_modifiers;
            buffer_len++;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* A-Z가 아닌 키(스페이스/엔터/백스페이스/화살표 등) = 단어 경계 */
    buffer_len = 0;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(word_flip_capture, word_flip_keycode_listener);
ZMK_SUBSCRIPTION(word_flip_capture, zmk_keycode_state_changed);

static void queue_kp_ex(struct zmk_behavior_binding_event *event, uint32_t param1,
                        uint32_t post_wait_ms) {
    struct zmk_behavior_binding binding = {
        /* "KP"는 devicetree 라벨일 뿐이고, zmk_behavior_get_binding()이 찾는
         * 디바이스 이름은 노드 이름인 "key_press"다 (ZMK app/dts/behaviors/
         * key_press.dtsi의 `kp: key_press { ... }`). "KP"로 두면 조회가
         * NULL이 되어 "No behavior assigned" 경고만 남기고 아무것도 안 나간다. */
        .behavior_dev = "key_press",
        .param1 = param1,
        .param2 = 0,
    };
    zmk_behavior_queue_add(event, binding, true, WORD_FLIP_TAP_MS);
    zmk_behavior_queue_add(event, binding, false, post_wait_ms);
}

static void queue_kp(struct zmk_behavior_binding_event *event, uint32_t param1) {
    queue_kp_ex(event, param1, WORD_FLIP_WAIT_MS);
}

static int on_word_flip_binding_pressed(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    struct word_flip_key snapshot[WORD_FLIP_MAX_LEN];
    size_t snapshot_len = buffer_len;

    if (snapshot_len == 0) {
        return ZMK_BEHAVIOR_OPAQUE; /* 되돌릴 게 없으면 아무것도 안 함 */
    }

    memcpy(snapshot, buffer, sizeof(struct word_flip_key) * snapshot_len);
    /* 아래에서 보낼 백스페이스가 이 파일의 리스너에도 잡혀서 버퍼를 지울
     * 것이므로, 재입력에 쓸 내용은 이미 snapshot에 복사해뒀으니 미리 비움 */
    buffer_len = 0;

    /* param1: 0 = Windows, 1 = macOS
     *
     * 단어 삭제:  Windows = Ctrl+Backspace,  macOS = Option+Backspace
     * 한/영 전환: Windows = LANG1(HID 0x90, 전용 한/영 키의 표준 코드)
     *             macOS   = Caps Lock
     *
     * macOS는 LANG1을 한/영 전환으로 처리하지 않는다 - 그 코드는 일본어 JIS
     * 가나 키(kVK_JIS_Kana)로 해석된다. Apple이 공식적으로 안내하는 전환
     * 방법은 Control+Space / Control+Option+Space / Caps Lock / Fn(지구본)
     * 뿐이고, 이 키보드 사용자는 Caps Lock 방식을 쓰므로 그쪽에 맞춘다.
     * (시스템 설정 > 키보드 > 입력 소스에서 "Caps Lock 키로 ABC 입력 소스
     * 전환"이 켜져 있어야 한다. 짧게 누르면 입력 소스 전환, 길게 누르면
     * 실제 Caps Lock이므로 여기서 보내는 40ms 탭은 전환으로 동작한다.) */
    bool is_mac = binding->param1 == 1;
    uint32_t delete_word = is_mac ? LA(BSPC) : LC(BSPC);
    uint32_t lang_toggle = is_mac ? CLCK : LANG1;

    /* 순서 주의: 한/영 전환을 반드시 먼저 보낸다. 한글 입력 중이면 마지막
     * 음절이 IME의 조합(composition) 상태로 물려 있어서, 이때 오는
     * Ctrl/Option+Backspace는 앱까지 가지 않고 IME가 가로채 조합 중인 음절만
     * 지운다(앞쪽 한글이 남는 증상). 전환 키를 먼저 보내면 그 시점에 조합이
     * 확정되고 IME가 빠지므로 뒤따르는 단어 삭제가 단어 전체에 적용된다. */
    queue_kp_ex(&event, lang_toggle, is_mac ? WORD_FLIP_MAC_TOGGLE_WAIT_MS : WORD_FLIP_WAIT_MS);
    queue_kp(&event, delete_word);

    for (size_t i = 0; i < snapshot_len; i++) {
        uint32_t param1 = ((uint32_t)snapshot[i].explicit_modifiers << 24) | snapshot[i].keycode;
        queue_kp(&event, param1);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_word_flip_binding_released(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api word_flip_driver_api = {
    .binding_pressed = on_word_flip_binding_pressed,
    .binding_released = on_word_flip_binding_released,
};

static int word_flip_init(const struct device *dev) {
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, word_flip_init, NULL, NULL, NULL, POST_KERNEL,
                         CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &word_flip_driver_api);

#endif /* central-only && DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
