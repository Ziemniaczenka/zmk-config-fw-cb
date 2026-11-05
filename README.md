# ZMK for the Framework Control Board

This is a minimal ZMK configuration for the Control Board from Framework's wireless keyboard, which uses an nRF54LM20A chip. It builds Framework's own board definition with ZMK's additions, and includes a shield for a key matrix wired to the board's header, a starter keymap, and support for USB, battery charging, the status LED and the wired/Bluetooth switch. It also has CI and local builds that sign the firmware so Framework's bootloader, MCUboot, will accept it.

Only part of the firmware has been tested, and only on the earlier Zephyr 4.4 build: it booted, typed over USB, paired with a Mac over Bluetooth, reported the battery level, let ZMK Studio edit the keymap over Bluetooth, and responded to the slide switch. The current Zephyr 4.5 build compiles but has not been run on hardware yet.

Framework publishes the board definition in its fork of Zephyr, [FrameworkComputer/zephyr#2](https://github.com/FrameworkComputer/zephyr/pull/2), which is based on Zephyr 4.5, and `config/west.yml` uses that fork. ZMK's official releases don't support Zephyr this new, so `config/west.yml` uses a draft version of ZMK, [zmkfirmware/zmk#3387](https://github.com/zmkfirmware/zmk/pull/3387), written for Zephyr 4.4. Both are pinned to exact commits.

## Building

The easiest way to build is to fork this repository, edit the keymap and push. GitHub Actions then builds the firmware and uploads `framework_cb_matrix-framework_wireless_tp_kb_nrf54lm20a_cpuapp_zmk.signed.bin`.

To build on your own computer, you need macOS or Linux, Python 3.10 or newer, and either `arm-none-eabi-gcc` or the Zephyr SDK 1.0. Then run:

```bash
scripts/build.sh --setup     # once: fetches about 4 GB into .zmk/
scripts/build.sh             # builds and signs
```

The finished image is saved as `build/framework_cb_matrix/zephyr/zmk.signed.bin`. Both ways of building apply the fixes in `patches/` before compiling.

## Patches

| Patch | What it does | Can go when |
|---|---|---|
| `zephyr/0001` | Defines `NRF_SOC_VALIDATE_HEADERS_DISABLED`, which ZMK's Kconfig sets. From ZMK's Zephyr fork. | ZMK supports Zephyr 4.5 |
| `zephyr/0002` | Adds the `post_boards_shields` CMake hook that ZMK loads the keymap through. From ZMK's Zephyr fork. | ZMK supports Zephyr 4.5 |
| `zephyr/0003` | Makes the nPM1300 charger driver stop the charger before configuring it. The charger is already running at 240 mA when the driver starts, most likely set by Framework's factory bootloader, and a new current only takes effect when charging is enabled. Without this the configured current is ignored and current readings are about twice too high. | Zephyr has the fix |
| `zmk/0001` | Lets ZMK use Zephyr's `device_next` USB stack, the only one that supports this board's USB controller. | ZMK supports `device_next` |
| `zmk/0002` | Lets the firmware turn Bluetooth off at runtime, for the wired/Bluetooth switch. | ZMK can do this itself |
| `zmk/0003` | Drops ZMK Studio's default for `BT_CONN_TX_MAX`. Zephyr 4.5 removes that option and Kconfig aborts on it. | ZMK supports Zephyr 4.5 |
| `zmk/0004` | Includes the generated syscall headers from `zephyr/syscalls/`, the path Zephyr 4.5 requires. | ZMK supports Zephyr 4.5 |
| `zmk/0005` | Adapts ZMK Studio's UART code, because `uart_irq_update()` no longer returns a value. | ZMK supports Zephyr 4.5 |
| `zmk/0006` | Keeps reporting the battery level while the keyboard is idle. ZMK stops 30 s after the last key press, so hosts kept an old level. | ZMK changes this |

## Flashing

Hold the pairing button on the back of the board while you plug it in or reset it. The bootloader then shows up on your computer as a USB serial port, and you can upload the image with these commands:

```bash
mcumgr --conntype serial --connstring dev=/dev/tty.usbmodem<N>,baud=115200 \
    image upload framework_cb_matrix-framework_wireless_tp_kb_nrf54lm20a_cpuapp_zmk.signed.bin
mcumgr --conntype serial --connstring dev=/dev/tty.usbmodem<N>,baud=115200 reset
```

After flashing, the keyboard appears as **Framework CB**. Fn+Q, Fn+W, Fn+E and Fn+R switch between Bluetooth profiles 0 to 3, and Fn+T clears the current profile.

If the board ran Framework's own firmware before, its old Bluetooth pairings are still stored on it. If a computer connects for a second and then drops, press Fn+T on that profile and pair again.

Images are signed with MCUboot's public development key. Framework's bootloader trusts this key, so it accepts the images.

## USB

Only Zephyr's newer USB stack, called device_next, supports this board's USB controller. ZMK's usual USB code uses the older stack. The patch `patches/zmk/0001` lets ZMK use device_next, and `src/usb_next.c` provides the keyboard support. USB logging and the USB boot protocol are not available.

## Battery

The charger on the board's power chip, the nPM1300, is already running at 240 mA when the firmware starts, most likely set by Framework's factory bootloader. The firmware restarts it with its own settings (`patches/zephyr/0003`): it charges the battery to 4.2 V at 450 mA and draws up to 500 mA from USB. Framework hasn't published details about its battery. 450 mA is meant for cells of 800 mAh or more, so lower it for a smaller cell. The battery's temperature sensor (thermistor) must be a 10 kOhm B3380 type, or the battery won't charge. Framework's board doesn't define a charger, so these settings are in the `npm1300_charger` node in `boards/framework/framework_wireless_tp_kb/framework_wireless_tp_kb_nrf54lm20a_cpuapp_zmk.dts`.

The battery level comes from Nordic's nRF Fuel Gauge library. It combines the chip's voltage, current and temperature readings with the charging state, instead of going by voltage alone. It takes a reading every second while USB is plugged in or the board draws 20 mA or more, and every 10 seconds otherwise. It also saves its progress every hour, so a reset doesn't make it lose track. For now it uses Nordic's example LiPo battery model instead of a model of the real battery. For better accuracy, make a model with Nordic's nPM PowerUP app and use it to replace `third_party/nrf_fuel_gauge/battery_model.inc`. The level is sent every 10 seconds when it changes, also while the keyboard is idle (`patches/zmk/0006`). With no battery connected, hosts keep the last level. The example model is for a cell of about 270 mAh, so with a larger cell the level changes too quickly.

## Wired/Bluetooth switch

When S1 is set to ON, keys are sent over Bluetooth, or over USB if no Bluetooth device is connected. When S1 is set to OFF, Bluetooth turns off and keys are sent over USB. USB stays connected in both positions, so the board can charge from a computer while you type over Bluetooth. The firmware reads S1 through the `protocol_switch` key in Framework's board. This feature needs `patches/zmk/0002`. To choose the output with ZMK's `&out` behavior instead, set `CONFIG_FRAMEWORK_CB_MODE_SWITCH=n`.

## ZMK Studio

ZMK Studio works over USB and over Bluetooth. Over USB, the board shows up as a serial port next to the keyboard. Set S1 to OFF so keys also go over USB, then connect from zmk.studio in Chrome or Edge, or from the ZMK Studio app. Over Bluetooth, set S1 to ON. On macOS you need the ZMK Studio app from zmk.studio/download for Bluetooth, because the web version only supports Bluetooth on Linux. Press Fn+Enter to unlock editing. Studio shows the keys as an 8 by 18 grid until you edit the `keys` list in the shield overlay to match your keyboard. Three spare layers are set aside for you to use in Studio. Changes made in Studio are saved on the board, and they replace the keymap file until you choose "Restore Stock Settings" in Studio. To build without Studio, remove `CONFIG_ZMK_STUDIO=y` from `config/framework_cb_matrix.conf`. The serial port then goes away too.

## LEDs

The LED turns green when the bootloader is in recovery mode and blue when a computer or phone is connected. To keep the blue light off, set `CONFIG_FRAMEWORK_CB_STATUS_LED=n`.

## Developing

Run `scripts/build.sh --setup` while you have internet access. After that, everything below is saved on your computer, including ZMK's documentation in `.zmk/zmk/docs/docs/`.

1. Start by getting the build, flash and test steps working, and practice recovering the board with the pairing button.
2. To change keys, edit `config/framework_cb_matrix.keymap`. To turn features on or off, edit `config/framework_cb_matrix.conf`. In ZMK's docs, the list of keycodes is in `keymaps/list-of-keycodes.mdx`, behaviors are explained in `keymaps/` and `features/`, and every option is listed in `config/`.
3. For a matrix of a different size, edit `row-gpios`, `col-gpios` and the transform in `boards/shields/framework_cb_matrix/framework_cb_matrix.overlay`, then update the keymap. For a different keyboard, copy the shield under a new name and add it to `build.yaml`. ZMK's `hardware-integration/` docs explain how shields work.
4. The board's pins, LEDs, switch and power rails come from Framework's board in `.zmk/zephyr/boards/framework/framework_wireless_tp_kb/`. This config's additions and overrides, such as the charger, are in `boards/framework/framework_wireless_tp_kb/`. Keep a copy of Framework's schematics with the repository. Devicetree properties are defined in `.zmk/zephyr/dts/bindings/` and `.zmk/zmk/app/dts/bindings/`. After a build, `build/framework_cb_matrix/zephyr/zephyr.dts` and `.config` show what was built. To browse every option, run `west build -d ../build/framework_cb_matrix -t menuconfig` from inside `.zmk/`, with `.zmk/.venv/bin` on your PATH.
5. `src/status_led.c` and `src/mode_switch.c` are short examples of code that reacts to ZMK events and to input events.
6. For logs, connect a 3.3 V serial adapter to the debug UART on the pin header (TX P2.02, RX P2.00, 115200 baud) and build with `SNIPPET=framework-cb-uart-log scripts/build.sh --pristine`. This keeps the UART's receiver on, which uses more battery, so use it only for debugging.

To use this board in a different zmk-config, add this repository to that config's `config/west.yml` and build for `framework_wireless_tp_kb/nrf54lm20a/cpuapp/zmk` with your own shield. You also need to apply `patches/`.

## Not covered

This config doesn't support the touchpad, the backlight, the white LEDs, the Caps Lock LED, split keyboards, the dongle or Framework's own keyboard layout. If you want to set up that layout, its key matrix map is `TouchpadKBMatrix.png` in Framework's [schematics](https://github.com/FrameworkComputer/Framework-Wireless-Touchpad-Keyboard/tree/main/ControlBoard/Schematic).

## License

This project uses the MIT license. `boards/framework/framework_wireless_tp_kb/mcuboot-dev-ed25519.pem` is MCUboot's public development key, from [mcu-tools/mcuboot](https://github.com/mcu-tools/mcuboot) under the Apache-2.0 license. Because the key is public, anyone can sign images with it, so treat images signed with it as unverified. The `third_party/nrf_fuel_gauge/` folder contains nRF Fuel Gauge 2.0.0 (from sdk-nrfxlib v3.4.1) and the example battery model (from sdk-nrf v3.4.1). They are under Nordic's 5-Clause license (`third_party/nrf_fuel_gauge/LICENSE`), which only allows their use with Nordic chips.
