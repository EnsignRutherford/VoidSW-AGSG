# VoidSW on the Atari GameStation Go (AGSG)

Build: `platform/gamestation-go-build.sh` (WSL Ubuntu; GCC 6.3 ARMv7 hard-float
cross toolchain, `-DEDUKE32_GAMESTATION_GO`, Debian stretch AGSG SDK). The
deployable package is `output/gamestation-go-armv7hf` (`voidsw`, `run-voidsw.sh`,
`gamecontrollerdb.txt`, `lib/`). Always copy the existing executable to a
timestamped `.bak` before replacing it.

## Platform-specific input handling

- **Volume buttons are ignored.** They live on the separate `zadc-keys` evdev
  device (EV_KEY 114 `KEY_VOLUMEDOWN`, 115 `KEY_VOLUMEUP`). In game mode the
  console driver also remaps them to ordinary game inputs (controller A,
  D-pad down, Down arrow, axis pushes), which is why volume down used to fire
  and volume up to crouch/move down. They must never produce a game, menu or exit
  action. This is the same minimal approach used in the NBlood, EDuke32,
  RedNukem and PCExhumed AGSG ports (`source/build/src/sdlayer.cpp`,
  `EDUKE32_GAMESTATION_GO`):
  - `SDL_SCANCODE_VOLUMEUP/DOWN` / `SDLK_VOLUMEUP/DOWN` key events are
    dropped in the key path, and evdev 114/115 are never turned into input by
    the direct keypad reader.
  - The `zadc-keys` node is read passively (no grab) only to timestamp real
    114/115 events. While a volume key is held (up to 1 s) or within 250 ms of
    its last event, new presses (controller/raw joystick buttons, strong axis
    pushes, SDL key-downs) are dropped. Releases always pass, so nothing can
    stick. Home (misc1) is never dropped. Number keys (SDL scancodes 1-0 and
    KP 1-0) are exempt from the key-down drop, and the direct keypad reader
    does not apply the window at all: the driver never remaps keypad digits,
    and dropping them made the wireless gamepad's number keys unusable.
  - The wireless gamepad's keypad (HID keyboard interface of the "LiteStar
    PC-GamePad" evdev nodes, e.g. `KEY_KP1`) is read passively by the same
    keypad reader and injected as number keys 1-0; its `*` (`KEY_9`) and `#`
    (`KEY_0`) become the left/right stick clicks, like the built-in keypad.
    No other key from those nodes is used. The reader keeps its `zadc-keys` /
    pad flags in step with its fd list when a node is removed.
  - No button allow-list, controller-database rewriting, joystick enumeration
    change or config remapping is used; `run-voidsw.sh`, `gamecontrollerdb.txt`,
    `config.cpp` and the ImGui backend are unchanged from the pre-volume build.
  - Home (misc1/b25), D-pad, triggers, keypad and trackball behavior is unchanged.
  - Earlier, more invasive attempts (agpio-keys button allow-list, ignoring the
    `zadc-keys` SDL joystick, `run-voidsw.sh` mapping sanitising) were
    reverted; that build is kept as `voidsw.broken-*.bak`.
- **Keys Setup capture.** Pressing A (controller A, trackball click or Enter)
  on a key entry in Keys Setup starts the "Press the key..." capture, but the
  same press is also injected as a Return key event (`gsg_injectMenuKey`) that
  arrives after the capture began, so Return was stored as the binding.
  `MNU_KeySetupCustom` (`source/sw/src/menus.cpp`) now discards scancodes in
  capture mode until Enter/controller A is released and 1/3 s has passed;
  Escape (Home) still cancels immediately and normal menu navigation is
  unchanged.
- `EDUKE32_GSG_INPUT_DEBUG=1` (launcher `--debug`) logs dropped inputs as
  `GSGDBG dropped phantom volume-remap ...`.