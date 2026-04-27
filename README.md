# linux-hi6250 (cldhd fork)

Linux 7.0 with **Huawei Honor 9 Lite (codename `leland`, SoC HiSilicon
Kirin 659 / Hi6250) USB-networking restoration patches**, on top of
patches inherited from `tildeknown/linux-hi6250`.

This fork's branch `hi6250-port-attempt` is the kernel that is currently
running on the test device, accessible via SSH over USB-Ethernet:

```
$ ssh root@172.16.42.1
Linux huawei-leland 7.0.0-gac38def65c79-dirty #38 SMP PREEMPT … aarch64 GNU/Linux
PRETTY_NAME="postmarketOS edge"
```

The original upstream Linux README is preserved at `README` in the
repository root.

## ⚠️ Authorship disclaimer

**This fork was not authored by a human.** I (cldhd / Vadim) am not a
programmer — I drive a phone-as-a-Linux-handheld project by orchestrating
neural-network coding agents (Claude Code, Codex). Every line of source
change in this fork that goes beyond what was inherited from upstream
parents was produced by those agents during interactive debugging
sessions on the Honor 9 Lite hardware. My own contribution is choosing
the goal, flashing the test images, and reporting back what the screen
and `dmesg` showed.

If you read code here, treat it as agent-authored: useful as a record of
what worked on this specific phone, but with no guarantee of style
consistency, upstream-readiness, or correctness on any other board.

## Provenance

There are three layers in this tree, from oldest to newest:

1. **Linux 7.0 upstream tarball import** (commit `Linux 7.0 upstream
   tarball import` on branch `master`). Identical to upstream
   `kernel.org` Linux 7.0.

2. **`tildeknown/linux-hi6250` Hi6250 SoC patches.** Copied here at fork
   time (Apr 26 2026) as `git am` of patches off that branch. These are
   the commits before `arm64: dts: hisilicon: add Huawei Honor 9 Lite
   (leland) support` on branch `hi6250-port-attempt` and include things
   like `huawei-bl-quirks.dtsi`, `psci-0.2`, MediaPad/P-Smart bindings,
   the "made it slightly similar to downstream" / "removes debug" /
   "debug again" / "sdfsdf" sequence, and so on. Their content and
   commit messages match the equivalent commits in
   `tildeknown/linux-hi6250` `hi6250-next` (different SHAs because the
   commits were rebased onto a 7.0 base instead of tildeknown's
   floating base). **Authorship of this layer belongs to tildeknown
   and prior contributors, not to this fork.**

3. **Neural-net-authored Honor-9-Lite-specific patches**, on top of (2).
   These are the commits unique to this fork. They are described in
   detail below.

## What this fork adds (the neural-net layer)

These commits are unique to this fork. They are what was needed beyond
tildeknown's tree to (a) make the Honor 9 Lite boot mainline Linux at
all, and (b) fix the USB-networking regression that was the focus of
this session.

### Hi6250-Honor-9-Lite hardware support (committed before this session)

These commits were authored by neural-net agents in earlier sessions and
were already in the local tree at the start of the USB debug session.
They are the foundation that the USB fix sits on top of:

- **`arm64: dts: hisilicon: add Huawei Honor 9 Lite (leland) support`** —
  device tree for the phone: pmu_ctrl, sys_ctrl, ao_ctrl, crg_ctrl
  syscons, the dwc2 USB OTG node at `usb@ff100000`, eMMC, simplefb
  framebuffer hand-off node, partition map.
- **`usb: dwc2: add Hi6250 (Kirin 659) USB2 OTG wrapper driver`** — new
  driver `drivers/usb/dwc2/dwc2-hi6250.c` that runs the SoC-specific
  power-on sequence (PCTRL clock mux, ABB 19.2 MHz reference clock via
  pmu_ctrl, AHBIF/PHY/controller reset release ordering, AHBIF CTRL0/2/3
  init, VBUS-valid override) before the standard dwc2 platform driver
  takes over the controller. **Compatible string `hisilicon,hi6250-usb`.**
- **`clk: hisilicon: hi6250: extend clock driver`** — adds the
  `HI6250_CLK_GATE_ABB_192` PMU gate so the dwc2 wrapper can request the
  USB2 PHY 19.2 MHz reference clock by name. (Note: the framework's
  `gate_flags=9` value here is wrong — it sets `CLK_GATE_SET_TO_DISABLE`
  unintentionally — and the dwc2 wrapper has a direct `regmap_update_bits`
  workaround.)
- **`video: fbdev: simplefb: Hi6250 Huawei bootloader quirks`** — work
  around format/mode differences between the Huawei BL handoff
  framebuffer and what simplefb auto-detects.
- **`arm64: Huawei bootloader handoff workarounds (Honor 9 Lite)`** —
  early head.S additions: SP805 watchdog disable at `0xe8a06000`
  (otherwise the BL-armed watchdog reboots the device a few seconds
  after handoff), appended-DTB copy past `_end` (so BSS clear doesn't
  destroy the DTB), Kconfig knob to keep KASLR off by default. Also
  adds setup.c / vmlinux.lds.S diagnostics to surface the initial
  register state for debugging the handoff.
- **`mmc: sdhci-of-arasan: Hi6250 orphan-clock bring-up diagnostics`** —
  adds logging around the eMMC clock plumbing during probe; needed
  because the BL leaves the SDHCI clock tree in a non-standard state.
- **`power: reset: add Hi6250 (Kirin 659) LPM3 reboot driver`** — new
  driver to issue the SoC-specific reset via the LPM3 mailbox.

### USB-networking restoration (committed in this session)

These are the commits that bring USB networking back. They are the work
of this session's agent run, on top of the tildeknown layer + the prior
agent commits above.

#### `drivers/usb/dwc2/dwc2-hi6250.c` — AHBIF CTRL0 bit-layout fix

**This is the most important fix.** Without it, the USB gadget never
enumerates from a cold boot.

The Hi6250 has an AHBIF interface block (separate from the dwc2 core
itself) at `0xff200000` that gates ID detection, ACA enable, and VBUS
validity for the dwc2 OTG controller. The downstream Huawei kernel
defines this block via a packed bit-field union
`union usbotg2_ctrl0` in
`drivers/usb/susb/hisi_usb_otg_type.h`:

```
bit 0   = idpullup_sel  (do not touch)
bit 1   = idpullup
bit 2   = acaenb_sel    -> set to 1 (register source)
bit 3   = acaenb        -> clear to 0 (ACA disabled)
bit 4-5 = id_sel        -> set to 01 (from PHY iddig)
bit 6   = id
```

The original wrapper in this fork had the bit positions confused:

```c
val |= BIT(0) | BIT(4);   /* id_sel | acaenb_sel */    ← WRONG
val &= ~BIT(5);           /* clear acaenb */            ← WRONG
```

The author had associated `BIT(0)` with `id_sel` (it isn't — `id_sel`
is bits 4-5) and `BIT(4)` with `acaenb_sel` (it isn't — `acaenb_sel`
is bit 2). The result was: never set `acaenb_sel`, never clear `acaenb`,
and incorrectly set `idpullup_sel`. The dwc2 controller saw garbage on
its ID/ACA lines and never came up cleanly. Symptom: USB device
descriptor reads return `-71 EPROTO`, no enumeration, host xhci retries
forever.

Corrected to match downstream:

```c
val |= BIT(2);            /* acaenb_sel = 1 */
val &= ~BIT(3);           /* acaenb = 0 */
val |= BIT(4);            /* id_sel low bit = 1 */
val &= ~BIT(5);           /* id_sel high bit = 0 → id_sel = 01 (PHY iddig) */
```

#### `arch/arm64/boot/dts/hisilicon/hi6250-huawei-leland.dts` — eye-diagram param

DT property `hisilicon,eye-diagram-param = <0x059066DB>` — this is the
HS PHY tuning value used by the downstream Huawei Linux fork's DTS for
the same SoC. With the CTRL0 bit-layout fix above in place, this is the
value that lets HS chirp succeed. The earlier `0x05cd06db` was a guess
based on what the BL leaves in the register at handoff time and
empirically produces FS-only enumeration with `-71 EPROTO`.

#### `arch/arm64/boot/dts/hisilicon/hi6250.dtsi` — `g-tx-fifo-size` padded

Padded `g-tx-fifo-size` to 15 entries (matching upstream Hi6220 DTS) to
suppress dwc2's auto-correct warnings.

#### `drivers/usb/dwc2/params.c` — force `utmi_phy_data_width = 8`

`dwc2_check_param_phy_utmi_width` in `params.c` overrides the 8-bit
`phy_utmi_width` set in `dwc2_set_his_params` to 16 if the HW
auto-detect (`GHWCFG4_UTMI_PHY_DATA_WIDTH`) reports `_8_OR_16` or `_16`,
even though Hi6250's PicoPHY is 8-bit. Workaround: in
`dwc2_set_his_params`, also set
`hsotg->hw_params.utmi_phy_data_width = GHWCFG4_UTMI_PHY_DATA_WIDTH_8`
so the validator accepts 8.

#### `drivers/usb/dwc2/gadget.c` — `lx_state` stuck-at-L2 workaround

`dwc2_hsotg_ep_queue` returns `-EAGAIN` whenever `lx_state != L0`. After
SET_ADDRESS on this PHY the bus briefly enters USBSUSP and `lx_state`
gets stuck at L2 even though the controller is fully active again
(DSTS.SUSPSTS clears). Symptom: `dwc2_hsotg_enqueue_setup: failed queue
(-11)` after `new address 1`. Workaround: read DSTS — if
`DSTS.SUSPSTS == 0`, controller is actually active, force
`hs->lx_state = DWC2_L0` and proceed.

### RTC support (committed in this session, unrelated to USB)

These are independent of the USB work but are in the same uncommitted
working set and so were committed together.

- **`drivers/rtc/rtc-hi6555v100.c`** (new file) — driver for the
  Hi6555v100 PMIC RTC (the wall-clock RTC, not the SoC's `pl031`).
- **`drivers/rtc/Kconfig` / `drivers/rtc/Makefile`** — register the new
  driver.
- **`drivers/rtc/rtc-pl031.c`** — small modifications to the upstream
  PL031 driver to coexist with `rtc-hi6555v100` as the wall-clock source.
- **`drivers/power/reset/hi6250-reboot.c`** — small modifications.

These pair with DT nodes in `arch/arm64/boot/dts/hisilicon/hi6250.dtsi`:

- `rtc@fff04000` — PL031 SoC RTC (rtc1).
- `rtc@fff34740` — PMIC RTC (rtc0; battery-backed; survives poweroff).
  The address overlaps the `pmu_ctrl@fff34000 / 0x1000` syscon region.
  This is intentional: the PMIC is on the SSI bus exposed by `pmu_ctrl`
  with 4-byte stride, and PMIC byte 0x1D0 (RTC data register 0) maps to
  MMIO offset `0x1D0 << 2 = 0x740`. The 0x40 sub-window is large enough
  to cover the data, load and control registers (offsets 0x00, 0x20,
  0x30 with 4-byte stride). The Hi6250 BL leaves `RTCCTRL` bit 0 = 1
  so the counter is already enabled at handoff — the driver only
  reads/writes the data + load registers.

`rtc-hi6555v100` registers first and so becomes `rtc0`, which is the
device the kernel uses for `CONFIG_RTC_HCTOSYS_DEVICE="rtc0"`. PL031
is registered as `rtc1` and remains volatile (it does not have a
battery-backed tick source on this board).

**Verified working 2026-04-27:** `since_epoch` increments by 1 per
second; `hwclock --systohc` writes the PMIC counter directly via
the driver; PMIC counter persists across reboot (battery-backed).

## Boot flow

The kernel is loaded by the U-Boot stage in the sibling repo
`cldhd/u-boot-hi6250`, which itself runs after the Huawei BL hands off
control. The kernel command line is set in the device tree:

```
console=tty0 loglevel=8 root=/dev/mmcblk0p51 rootfstype=ext4 rw rootwait
```

There is **no embedded initramfs** (`CONFIG_INITRAMFS_SOURCE=""`).
The kernel mounts p51 directly and runs `/sbin/init`. PostmarketOS's
standard `setup_usb_network_configfs` from
`/usr/share/initramfs/init_functions.sh` is therefore *not* called in
this boot path — instead the gadget is set up by a custom
`/etc/local.d/50-usb-network.start` on the rootfs, which uses CDC ECM
(not RNDIS) because RNDIS_INIT hits a separate dwc2 EP0 control-transfer
issue on this kernel.

## Building

```
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- huawei-leland_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image dtbs
```

Then assemble Image-dtb:

```
cat arch/arm64/boot/Image \
    arch/arm64/boot/dts/hisilicon/hi6250-huawei-leland.dtb \
    > flash/Image-dtb
```

The kernel `Image` is exactly 22 MiB after build. The DTB is appended
directly, landing at offset `0x1600000`, which is what the U-Boot
`bootcmd` expects.

## Known remaining issues

- **RNDIS gadget hits `RNDIS_INIT failed -71`** on the host. This is a
  control-transfer payload corruption on the dwc2 EP0 IN direction —
  likely an upstream dwc2 regression between Linux 6.19 and 7.0 (the
  6.19 backup kernel reaches the same RNDIS_INIT step). The CDC ECM
  workaround in `/etc/local.d/50-usb-network.start` avoids the issue
  because no class-specific control request returns a sized payload
  during ECM enumeration. A real fix needs an upstream dwc2 bisect.
- The kernel binary is built `7.0.0-gac38def65c79-dirty` because the
  USB-fix commit lands as a follow-up to `ac38def65c79`. After committing
  the patches in this README the `-dirty` suffix goes away and the
  version becomes `7.0.0-g<new-sha>`.

## License

All upstream Linux files retain their original SPDX license headers
(GPL-2.0 with the standard "Linus Torvalds" exception). Agent-authored
on-top patches are licensed GPL-2.0 to match the kernel core.
