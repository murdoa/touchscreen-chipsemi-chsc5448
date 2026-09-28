# Chipsemi CHSC5448 out-of-tree touchscreen driver

Experimental, hardware-unverified driver for the protocol implemented by the
local MediaTek `chsc5448_p785` vendor driver. Module name: `chsc5448`.

Uses the panel's existing firmware. **Does not flash firmware or configuration.**
Requires a working firmware image already on the controller. The vendor's
boot-time updater is deliberately not included.

## NixOS

Following the CST3240 module layout:

```nix
{
  imports = [ ./out-of-tree/touchscreen-chipsemi-chsc5448 ];
  hardware.out-of-tree.touchscreen-chipsemi-chsc5448.enable = true;
}
```

Build independently from this repository's root:

```sh
nix build --impure --no-link --builders '' --expr \
  '(import <nixpkgs> {}).linuxPackages.callPackage ./out-of-tree/touchscreen-chipsemi-chsc5448/driver.nix {}'
```

For deployment, use the target's `config.boot.kernelPackages`, not the build
host's default kernel. Requires I2C, REGMAP_I2C, INPUT, INPUT_TOUCHSCREEN and
GPIOLIB support. Targets Linux 6.12+ (including `<linux/unaligned.h>`);
compile-tested with Linux 6.12.78 on x86_64, not yet on the phone's kernel.

Without Nix, against a configured/prepared kernel build:

```sh
make -C out-of-tree/touchscreen-chipsemi-chsc5448/src KDIR=/path/to/kernel/build
# For cross-compilation, also pass ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

## Device tree

`chipsemi,chsc5448` is a provisional, local binding, not an upstream binding.
The driver does not scan buses. It binds to an explicitly described device.
Do not also bind the vendor driver to that same device.

Required:

- `compatible = "chipsemi,chsc5448"`
- `reg = <0x2e>` (address forced by this vendor driver)
- `interrupt-parent` and `interrupts`, configured for falling-edge IRQ
- `reset-gpios`, **GPIO_ACTIVE_LOW** (exclusive reset line)
- `vdd-supply` and `vccio-supply`, mapped to the actual board rails
- `touchscreen-size-x` and `touchscreen-size-y`, coordinate maxima plus one

Optional standard properties: `touchscreen-inverted-x`,
`touchscreen-inverted-y`, `touchscreen-swapped-x-y`.

Illustrative fragment only: replace GPIOs, supply references, bus and sizes
with verified board values. GPIO 0 and 480x854 below are placeholders.

```dts
#include <dt-bindings/gpio/gpio.h>
#include <dt-bindings/interrupt-controller/irq.h>

&i2c_touch {
    status = "okay";

    touchscreen@2e {
        compatible = "chipsemi,chsc5448";
        reg = <0x2e>;
        interrupt-parent = <&pio>;
        interrupts = <0 IRQ_TYPE_EDGE_FALLING>;
        reset-gpios = <&pio 0 GPIO_ACTIVE_LOW>;
        vdd-supply = <&touch_vdd>;
        vccio-supply = <&touch_vccio>;
        touchscreen-size-x = <480>;
        touchscreen-size-y = <854>;
    };
};
```

Supply voltages and sequencing belong to the board description; this driver
enables the supplied rails but does not guess or set their voltages. Linux can
substitute dummy regulators if supply properties are missing: do not treat that
as evidence that your wiring/DT is correct.

## Implemented protocol

- 32-bit big-endian register addresses, byte-oriented payloads.
- Reset asserted 30 ms, released, wait at least 5 ms, clear four bytes at
  `0x20000018`, then poll the firmware header at `0x20000014`.
- Require LE32 signature `0x43534843` at header offset 4. Fail explicitly on
  `0x4f525245` (firmware boot self-check failure).
- Read 56 bytes from `0x2000002c` per interrupt, no acknowledgement write.
- Ten five-byte records starting at offset 2:
  `x_lo, y_lo, major, y_hi:x_hi, event:id`.
- Event nibble 0 or 8 means active; 4 means released; ID 15 means unused.
- Contact count is the low nibble of byte 1. Zero releases all slots.
- Ignore proximity (`0xfc`), heartbeat (`0xfd`) and gesture (`0xfe`) packets;
  also ignore `0xff` as invalid.
- Reject invalid IDs, duplicate IDs, unsupported events and oversized counts
  before changing input state. Short I2C transfers are errors through regmap.
- Standard MT slots, coordinate transforms and pointer emulation.

The vendor iterates all ten records, not just the reported active count. This
driver likewise treats reports as full snapshots and drops absent slots. This
assumption and the event values need confirmation on hardware. It does not copy
the vendor's synthetic event-cycle rewriting. Checksum verification is omitted
because the vendor's active code path explicitly disables it; the report's
trailing bytes are not yet interpreted.

Suspend disables/drains the IRQ, releases touches and holds reset asserted.
Resume repeats boot validation. Rails remain enabled during suspend; no wake
gestures or low-power command protocol is implemented. On resume failure the
IRQ enable/disable balance is restored and the PM error is reported.

## Bring-up

1. Verify board rails, GPIO polarity, exclusive reset ownership and bus address.
2. Load `chsc5448` with the correct DT node and check the kernel log.
3. Enable dynamic debug for `chsc5448.c` if desired to log firmware/VID/PID.
4. Use `evtest` to check corners, drag, ten-finger tracking, lift/re-touch,
   simultaneous releases and suspend/resume.
5. Check IRQ counts in `/proc/interrupts` and watch for malformed-frame/I2C errors.

A successful module build is not a hardware compatibility test. If reset works
but firmware signature polling fails, capture the boot-header reads before
considering firmware changes. Do not try random Goodix-compatible strings or
run the vendor updater as a diagnostic.

Protocol references under
`android_kernel_xiaomi_dash_device_modules/drivers/input/touchscreen/chsc5448_p785/`:
`semi_touch_function.c`, `semi_touch_device.c`, `head_def.h`, `mtk_core.c`.
