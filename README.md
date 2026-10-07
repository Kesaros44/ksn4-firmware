# ksn4-firmware

ZMK firmware for the KSN-4 split keyboard — a [KSN-1](https://github.com/Kesaros44/ksn1-firmware) derivative with the left-half numpad removed.

* Keyboard Maintainer: [AJG](https://github.com/Kesaros44)
* Hardware Supported: KSN-4 split keyboard, nice!nano v2 (nRF52840), BLE

<!-- ![KSN-4 완성 사진](images/ksn4_board.webp) -->

## Hardware

- **MCU:** nice!nano v2 (nRF52840) per half, wireless (BLE)
- **Central:** right half
- **Matrix:** 6 rows × 18 columns combined (7 on the left + 11 on the right), `col2row`. The right half's `col-offset` is 7.
- **Difference from KSN-1:** the four numpad columns (and their switches) are removed from the left PCB. Every remaining GPIO is the same as KSN-1.
- **Encoder:** one EC11 encoder, right half only
- **LEDs (left half only):** Caps Lock LED, BLE connection-status LED
- **Backlight:** designed into the schematic, but parts aren't populated and the feature is disabled in firmware (`CONFIG_ZMK_BACKLIGHT=n`) — no backlight on the physical board
- **RGB underglow:** none
- **Battery:** reported from both halves

## Keymap (`config/ksn_4.keymap`)

Three layers, each 6 rows × 18 columns:

- **`default_layer`** — Windows base layer, same as KSN-1 without the numpad (the left half now starts at Esc / ` / Tab / Caps Lock / Shift / Ctrl). Encoder = volume.
- **`mac_layer`** — same layout with Mac modifier order and Mac media/brightness keys; the F-row has Mission Control, Spotlight and Dictation.
- **`func_layer`** (hold `&mo 2`; top layer, so FN always takes priority in both Windows and Mac modes) — Bluetooth profile select (0–4) and clear, output toggle, backlight inc/dec on the encoder (no-op — no backlight hardware), toggle (`&tog 1`) into `mac_layer`.

The numpad, the calculator macros, and the left-side Backspace/Delete keys (they sat in the numpad columns) are gone. Backspace and Delete remain on the right half.

## Building

GitHub Actions builds on every push — grab the `.uf2` files (`ksn_4_left`, `ksn_4_right`, `settings_reset`) from the workflow run's artifacts. Every successful build on `main` also replaces the `latest` release with the same files.

Local build with `west`:

```sh
west init -l config
west update
west build -p -b nice_nano_v2 -- -DSHIELD=ksn_4_left -DZMK_EXTRA_MODULES=$(pwd)/config
west build -p -b nice_nano_v2 -- -DSHIELD=ksn_4_right -DZMK_EXTRA_MODULES=$(pwd)/config
```

## Flashing

Double-tap reset on the nice!nano to enter the UF2 bootloader, then drag the matching `.uf2` onto the `NICENANO` drive (left firmware → left half, right → right half). Flash both halves — they run different images.

## Re-pairing / clearing Bluetooth bonds

Flash the `settings_reset` artifact to a half to wipe its BLE bonds, then reflash normal firmware and re-pair.

## Recent Changes

- Initial KSN-4 firmware, forked from KSN-1: removed the four left numpad columns from the matrix and keymap, set the right half's `col-offset` to 7, renamed the shield to `ksn_4`, and changed the BLE name and USB product string to KSN-4.
- Removed the Windows/macOS calculator macros (no numpad key to bind them to).
- Gave KSN-4 its own USB PID (`0x4B56`).

## Known Issues / TODO

- **Not yet verified on the physical KSN-4 board:** the firmware builds in GitHub Actions, but the matrix and keymap still need to be checked against the new PCB.
- **USB PID not registered:** `CONFIG_USB_DEVICE_PID=0x4B56` is a temporary placeholder. Needs a real PID from [pid.codes](https://pid.codes) before any commercial sale.
- **No right RCTRL/RGUI:** the Hangul/Hanja keys took over what used to be RCTRL and RGUI on the right half — those modifiers now live only on the left half.
- **mac_layer Hanja key may need Option+Return:** [KSN-2](https://github.com/Kesaros44/ksn2-firmware)'s `mac_layer` sends `LA(RET)` (Option+Return, macOS's actual Hanja shortcut) for its Hanja key because `LANG2` does nothing there; this board's `mac_layer` still sends plain `LANG2`, inherited from KSN-1.
- **word_flip's macOS delete may share a word-boundary bug found on KSN-3:** on 2026-09-16, [KSN-3](https://github.com/Kesaros44/ksn3-firmware) found that Option+Backspace doesn't respect word boundaries inside Hangul IME composition and switched to resending plain Backspace instead. This board still uses Option+Backspace for that step, inherited from KSN-1.
- **Modifier keys may still reset the word_flip buffer:** pressing Shift (or any other non-letter key) clears the tracked word, which could truncate a capital letter typed mid-word. Not yet confirmed fixed.
- **Source files keep the `ksn1_` prefix:** the custom C files under `config/src/` (LED, connection-status relay, `word_flip`) are unchanged from KSN-1 and still named `ksn1_*`.

---

# ksn4-firmware (한국어)

KSN-4 스플릿 키보드용 ZMK 펌웨어 설정입니다 — [KSN-1](https://github.com/Kesaros44/ksn1-firmware)에서 파생되었고, 왼쪽 half의 넘패드를 제거한 설계입니다.

* 키보드 관리자: [AJG](https://github.com/Kesaros44)
* 지원 하드웨어: KSN-4 스플릿 키보드, nice!nano v2 (nRF52840), BLE

<!-- ![KSN-4 완성 사진](images/ksn4_board.webp) -->

## 하드웨어

- **MCU:** half마다 nice!nano v2 (nRF52840), 무선(BLE)
- **Central:** 오른쪽 half
- **매트릭스:** 6행 × 18열 (왼쪽 7열 + 오른쪽 11열), `col2row`. 오른쪽 half의 `col-offset`은 7입니다.
- **KSN-1과의 차이:** 왼쪽 PCB에서 넘패드 4열과 해당 스위치를 제거했습니다. 남은 GPIO는 모두 KSN-1과 동일합니다.
- **인코더:** EC11 인코더 1개, 오른쪽 half에만 있음
- **LED (왼쪽 half 전용):** Caps Lock LED, BLE 연결상태 LED
- **백라이트:** 회로도에는 설계되어 있지만 부품이 실장되지 않았고 펌웨어 기능도 꺼져 있음(`CONFIG_ZMK_BACKLIGHT=n`) — 실제 보드에는 백라이트 없음
- **RGB 언더글로우:** 없음
- **배터리:** 양쪽 half 모두 보고

## 키맵 (`config/ksn_4.keymap`)

3개 레이어, 각각 6행 × 18열:

- **`default_layer`** — KSN-1에서 넘패드를 뺀 Windows 기본 레이어 (왼쪽 half가 Esc / ` / Tab / Caps Lock / Shift / Ctrl부터 시작). 인코더 = 볼륨.
- **`mac_layer`** — 동일한 배열에 Mac 모디파이어 순서와 Mac 미디어/밝기 키; F열에 Mission Control, Spotlight, 받아쓰기 키가 있음.
- **`func_layer`** (홀드 `&mo 2`, 맨 위 레이어 — Windows/Mac 모드와 관계없이 FN을 누르면 항상 우선) — 블루투스 프로필 선택(0–4) 및 clear, 출력 토글, 인코더로 백라이트 증감(백라이트 하드웨어 자체가 없어서 실제 동작은 없음), `mac_layer`로의 토글(`&tog 1`).

넘패드, 계산기 매크로, 그리고 넘패드 열에 있던 왼쪽 Backspace/Delete 키는 없어졌습니다. Backspace와 Delete는 오른쪽 half에 그대로 있습니다.

## 빌드

GitHub Actions가 push마다 자동으로 빌드합니다 — 워크플로우 실행의 아티팩트에서 `.uf2` 파일(`ksn_4_left`, `ksn_4_right`, `settings_reset`)을 받으면 됩니다. `main`에서 빌드가 성공할 때마다 `latest` 릴리즈도 같은 파일로 자동 교체됩니다.

`west`로 로컬 빌드:

```sh
west init -l config
west update
west build -p -b nice_nano_v2 -- -DSHIELD=ksn_4_left -DZMK_EXTRA_MODULES=$(pwd)/config
west build -p -b nice_nano_v2 -- -DSHIELD=ksn_4_right -DZMK_EXTRA_MODULES=$(pwd)/config
```

## 플래싱

nice!nano의 리셋 버튼을 더블탭해서 UF2 부트로더로 진입한 뒤, 마운트된 `NICENANO` 드라이브에 해당하는 `.uf2`(왼쪽 → 왼쪽 half, 오른쪽 → 오른쪽 half)를 드래그하면 됩니다. 양쪽 half 모두 플래시해야 합니다 — 서로 다른 이미지를 사용합니다.

## 재페어링 / 블루투스 본딩 초기화

`settings_reset` artifact를 해당 half에 플래시하면 BLE 본딩이 초기화됩니다. 그 다음 정상 펌웨어를 다시 플래시하고 재페어링하세요.

## 최근 변경 사항

- KSN-1에서 파생한 KSN-4 최초 펌웨어: 왼쪽 넘패드 4열을 매트릭스와 키맵에서 제거하고, 오른쪽 half의 `col-offset`을 7로 설정했으며, shield 이름을 `ksn_4`로 바꾸고 BLE 이름과 USB 제품명을 KSN-4로 변경.
- Windows/macOS 계산기 매크로 삭제 (붙일 넘패드 키가 없음).
- KSN-4 전용 USB PID(`0x4B56`) 지정.

## 알려진 이슈 / TODO

- **실제 KSN-4 보드에서 아직 검증 안 됨:** GitHub Actions 빌드는 통과했지만, 새 PCB에서 매트릭스와 키맵이 맞게 동작하는지 확인이 필요합니다.
- **USB PID 미등록:** `CONFIG_USB_DEVICE_PID=0x4B56`은 임시로 지정한 값입니다. 정식 판매 전 [pid.codes](https://pid.codes)에서 정식 PID를 할당받아야 합니다.
- **오른쪽 RCTRL/RGUI 부재:** 한/영·한자 키가 그 자리를 대체하면서, 해당 모디파이어는 왼쪽 half에만 남았습니다.
- **mac_layer 한자 키는 Option+Return이 필요할 수 있음:** [KSN-2](https://github.com/Kesaros44/ksn2-firmware)는 `mac_layer`의 한자 키를 `LA(RET)`(Option+Return, macOS의 실제 한자 변환 단축키)로 이미 바꿨습니다 — `LANG2`가 macOS에서 아무 동작도 안 하기 때문입니다. 이 저장소의 `mac_layer`는 KSN-1에서 물려받은 `LANG2` 그대로입니다.
- **word_flip의 macOS 삭제 방식이 KSN-3에서 발견된 단어 경계 버그를 공유할 수 있음:** 2026-09-16, [KSN-3](https://github.com/Kesaros44/ksn3-firmware)에서 Option+Backspace가 한글 IME 조합 중 단어 경계를 지키지 않는 문제가 발견되어 일반 Backspace 반복 전송 방식으로 교체했습니다. 이 저장소는 KSN-1에서 물려받은 Option+Backspace를 그대로 쓰고 있습니다.
- **수식키가 word_flip 버퍼를 여전히 리셋시킬 수 있음:** Shift 등 알파벳이 아닌 키를 누르면 인식 중이던 단어가 초기화되어, 단어 중간의 대문자가 잘릴 수 있습니다. 아직 수정 여부가 확인되지 않았습니다.
- **소스 파일명에 `ksn1_` 접두사가 남아 있음:** `config/src/` 아래 커스텀 C 파일(LED, 연결 상태 릴레이, `word_flip`)은 KSN-1에서 바뀐 것이 없어서 이름도 `ksn1_*` 그대로입니다.
