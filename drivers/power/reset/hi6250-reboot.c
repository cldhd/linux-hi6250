// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi6250 (Kirin 659) SoC reset driver.
 *
 * Restart path on Honor 9 Lite (and assumed other Kirin 659 phones):
 *
 *   1. Write COLDBOOT (0x10) into PMIC HRST_REG0 so the Huawei
 *      bootloader counts the upcoming reset as a clean
 *      user-requested cold boot. PMIC byte 0x18B → MMIO 0x62c via
 *      the 4-byte stride PMU SSI proxy.
 *
 *   2. Pulse bit 2 of sysctrl + 0x510 (SCLPMCUCTRL.nmi_in). This
 *      raises an NMI on the LPM3 co-processor, whose firmware
 *      performs the actual SoC reset.
 *
 * Mainline's PSCI restart handler (drivers/firmware/psci/, priority
 * 129) issues PSCI_0_2_FN_SYSTEM_RESET / SYSTEM_OFF via SMC, but on
 * this board's trustzone implementation both calls return
 * 0xffffffff (PSCI_RET_NOT_SUPPORTED) — the trustzone simply does
 * not implement them. Without an in-kernel fallback the kernel
 * silently halts at "Reboot failed -- System halted".
 *
 * The restart handler registers at priority 200 so it runs before
 * PSCI's handler. It tries PSCI itself first (so on a board that
 * DOES implement it, that path is preferred and the SoC is reset
 * immediately by the trustzone) and only pulses the LPM3 NMI when
 * PSCI returns. The LPM3 NMI does not return — the SoC resets
 * within ~500 ms of the pulse — so the post-NMI mdelay and the
 * NOTIFY_DONE return below are reached only if the LPM3 path also
 * fails, in which case the kernel halt path takes over and the
 * device must be force-power-cycled into TWRP for recovery.
 *
 * The poweroff path replicates the official Huawei LLD-OREO
 * `hisi_pm_system_off()` from drivers/hisi/mntn/hisi_poweroff.c:
 *
 *   1. Write HRST_REG0 = AP_S_COLDBOOT (0x00) — the value the BL
 *      sees on a true fresh-from-off power-on.
 *   2. Drive the "powerhold" GPIO low — wired to the PMIC enable
 *      input. Referenced via a `huawei,powerhold-gpios` property
 *      on this driver's DT node (gpio25 pin 4 ACTIVE_LOW per the
 *      stock dts.img).
 *   3. mdelay(1000) — give the PMIC time to actually cut rails.
 *   4. Fall through to "chargereboot" via LPM3 NMI: write
 *      HRST_REG0 = CHARGEREBOOT (0x06), then NMI-reset. BL
 *      interprets 0x06 as "AP wants to be in charge-mode standby"
 *      and routes to the charging UI (which looks off to the
 *      user). 0x06 is intentional so BL does NOT count it as
 *      abnormal boot — earlier we used 0x10 (COLDBOOT, "user
 *      reboot") which together with the GPIO action confused the
 *      BFM/BFR counter into force-routing to eRecovery (writes
 *      "boot-erecovery" to both `misc` p20 and the `rrecord`
 *      partition; recovery requires clearing both from TWRP).
 *
 * Verified working live on Honor 9 Lite 2026-04-27: PMIC RTC
 * survives the reset, BL boots back into PMOS in ~43 s, multiple
 * reboots in succession all succeed, no eRecovery failsafe trip.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <uapi/linux/psci.h>

/*
 * PMIC HRST_REG0 lives inside the PMUCTRL block. PMIC regs are strided
 * x4 (val_bits=8, reg_stride=4), so reg 0x18B is at byte offset 0x62c.
 *
 * Reason byte values from the official Huawei LLD-OREO source
 * `drivers/hisi/ap/platform/hi6250/mntn_public_interface.h` enum:
 *   AP_S_COLDBOOT = 0x00 — fresh power-on from off (NOT a user reboot)
 *   CHARGEREBOOT  = 0x06 — boot to charging-UI standby (looks off)
 *   COLDBOOT      = 0x10 — user-requested reboot, boot kernel normally
 */
#define HI6250_PMIC_HRST_REG0_OFFSET		0x62c
#define HI6250_PMIC_HRST_REASON_MASK		0xff
#define HI6250_REBOOT_REASON_AP_S_COLDBOOT	0x00
#define HI6250_REBOOT_REASON_CHARGEREBOOT	0x06
#define HI6250_REBOOT_REASON_COLDBOOT		0x10

/*
 * BOOTUP_KEYPOINT byte tracks how far the kernel got. The Huawei BL
 * reads it on next boot to decide whether the previous boot
 * succeeded (250 = STAGE_BOOTUP_END) or got stuck partway. If the
 * latter, BL increments an internal failure counter and after a few
 * such "failed" boots in a row force-routes to eRecovery (passing
 * `erecovery_enter_reason=20xx` on the cmdline). Without writing
 * 250 here, mainline PMOS hits this trap on rapid reboots.
 *
 * Lives at PMIC byte 0x18C (PMIC_HRST_REG1) → MMIO 0x630.
 */
#define HI6250_PMIC_BOOTUP_KEYPOINT_OFFSET	0x630
#define HI6250_BOOTUP_KEYPOINT_MASK		0xff
#define HI6250_STAGE_BOOTUP_END			250

/*
 * PMIC LDO17 (touch vci, ~3.0 V) and LDO4 (touch vddio, ~1.8 V).
 * BL leaves LDO4 enabled but NOT LDO17, so the FocalTech touchscreen
 * never gets analog power and doesn't respond on i2c2. Enable both
 * here so the touch driver can probe.
 *
 * PMIC byte N at MMIO N*4 (4-byte SSI stride). Bit 0 of each
 * ONOFF byte = enable.
 *   PMIC_LDO4_ONOFF_ECO  byte 0x019 → MMIO 0x064
 *   PMIC_LDO17_ONOFF_ECO byte 0x025 → MMIO 0x094
 *
 * This is a temporary stand-in for a proper hi655x-pmic regulator
 * binding that we can't enable yet (the mainline driver crashes
 * Sxmo on this hi6555c chip — see project_leland_pmic_chip.md).
 */
#define HI6250_PMIC_LDO4_ONOFF_OFFSET		0x064
#define HI6250_PMIC_LDO17_ONOFF_OFFSET		0x094
#define HI6250_PMIC_LDO_ENABLE_BIT		0x01

/* Sysctrl SCLPMCUCTRL: bit 2 = nmi_in to LPM3 */
#define HI6250_SCTRL_SCLPMCUCTRL	0x510
#define HI6250_SCTRL_NMI_IN_BIT		BIT(2)

static struct regmap *pmuctrl;
static struct regmap *sysctrl;
static struct gpio_desc *powerhold_gpio;

static void hi6250_do_lpm3_reset(u32 psci_fn, const char *what, u8 reason)
{
	struct arm_smccc_res res = {};

	/* BL marker — keep the bootloader's eRecovery failsafe at rest. */
	if (pmuctrl)
		regmap_update_bits(pmuctrl, HI6250_PMIC_HRST_REG0_OFFSET,
				   HI6250_PMIC_HRST_REASON_MASK, reason);

	/*
	 * Try the PSCI call first. On this board's trustzone this
	 * returns PSCI_RET_NOT_SUPPORTED (0xffffffff), but a more
	 * complete trustzone implementation might handle it and act
	 * on it inside the SMC, in which case we never return.
	 */
	arm_smccc_smc(psci_fn, 0, 0, 0, 0, 0, 0, 0, &res);
	pr_emerg("hi6250-reboot: %s PSCI call returned a0=0x%lx\n",
		 what, res.a0);
	mdelay(100);

	/* Pulse LPM3 NMI — the path that actually resets this SoC. */
	if (sysctrl) {
		pr_emerg("hi6250-reboot: %s pulsing SCLPMCUCTRL.nmi_in to LPM3\n",
			 what);
		regmap_update_bits(sysctrl, HI6250_SCTRL_SCLPMCUCTRL,
				   HI6250_SCTRL_NMI_IN_BIT,
				   HI6250_SCTRL_NMI_IN_BIT);
	}
	mdelay(500);

	pr_emerg("hi6250-reboot: %s LPM3 NMI did not reset the SoC; halting\n",
		 what);
}

static int hi6250_restart_handler(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	pr_emerg("hi6250-reboot: ENTER restart_handler mode=%lu cmd=%s\n",
		 mode, cmd ? (const char *)cmd : "(null)");

	hi6250_do_lpm3_reset(PSCI_0_2_FN_SYSTEM_RESET, "restart",
			     HI6250_REBOOT_REASON_COLDBOOT);
	return NOTIFY_DONE;
}

static int hi6250_poweroff_handler(struct sys_off_data *data)
{
	/*
	 * Replicates official LLD-OREO `hisi_pm_system_off()`:
	 *   1. HRST_REG0 = AP_S_COLDBOOT (0x00) — what BL expects on a
	 *      true power-on from off.
	 *   2. Drive powerhold GPIO low — wired to PMIC enable input.
	 *      Cuts main rails and the device stays off until the user
	 *      either presses power for 10 s or plugs in a charger.
	 *   3. Fallback: "chargereboot" (HRST_REG0=0x06) + LPM3 NMI in
	 *      case the GPIO somehow fails to cut rails.
	 *
	 * BOOTUP_KEYPOINT=250 was set in probe, so even if step 2 causes
	 * a partial reset, the BL won't trip the eRecovery failsafe.
	 */
	pr_emerg("hi6250-reboot: poweroff handler entered\n");

	if (pmuctrl)
		regmap_update_bits(pmuctrl, HI6250_PMIC_HRST_REG0_OFFSET,
				   HI6250_PMIC_HRST_REASON_MASK,
				   HI6250_REBOOT_REASON_AP_S_COLDBOOT);

	if (powerhold_gpio) {
		pr_emerg("hi6250-reboot: pulling powerhold GPIO active (LOW)\n");
		gpiod_set_value_cansleep(powerhold_gpio, 1);
		mdelay(1000);
	}

	pr_emerg("hi6250-reboot: powerhold did not cut rails — falling through to chargereboot\n");
	hi6250_do_lpm3_reset(PSCI_0_2_FN_SYSTEM_OFF, "poweroff",
			     HI6250_REBOOT_REASON_CHARGEREBOOT);

	while (1)
		mdelay(1000);

	return NOTIFY_DONE;	/* unreachable */
}

static struct notifier_block hi6250_restart_nb = {
	.notifier_call = hi6250_restart_handler,
	/*
	 * Mainline PSCI restart handler is at priority 129; we MUST run
	 * before it because on this trustzone its PSCI_FN_SYSTEM_RESET
	 * call returns NOT_SUPPORTED and the kernel then silently halts
	 * without anyone trying the LPM3 NMI fallback.
	 */
	.priority = 200,
};

static int hi6250_reboot_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int err;

	pmuctrl = syscon_regmap_lookup_by_phandle(dev->of_node,
						  "hisilicon,pmu-ctrl");
	if (IS_ERR(pmuctrl))
		return dev_err_probe(dev, PTR_ERR(pmuctrl),
				     "failed to get pmu-ctrl regmap\n");

	sysctrl = syscon_regmap_lookup_by_phandle(dev->of_node,
						  "hisilicon,sys-ctrl");
	if (IS_ERR(sysctrl))
		return dev_err_probe(dev, PTR_ERR(sysctrl),
				     "failed to get sys-ctrl regmap\n");

	/*
	 * Optional: powerhold GPIO. We initialise it as output INACTIVE
	 * (logical 0 → physical HIGH for the active-low PMIC enable hold
	 * pin) so the PMIC stays enabled until poweroff toggles it active.
	 */
	powerhold_gpio = devm_gpiod_get_optional(dev, "huawei,powerhold",
						 GPIOD_OUT_LOW);
	if (IS_ERR(powerhold_gpio))
		return dev_err_probe(dev, PTR_ERR(powerhold_gpio),
				     "failed to get powerhold gpio\n");
	if (powerhold_gpio)
		dev_info(dev, "powerhold GPIO claimed for poweroff\n");

	err = register_restart_handler(&hi6250_restart_nb);
	if (err)
		return dev_err_probe(dev, err,
				     "cannot register restart handler\n");
	dev_info(dev, "Hi6250 reboot handler registered (priority %d)\n",
		 hi6250_restart_nb.priority);

	/*
	 * Register at FIRMWARE+1 so we run before PSCI's legacy
	 * pm_power_off. Our handler does the powerhold GPIO pulse
	 * itself (single LOW, no toggle) and falls through to LPM3 NMI
	 * if the GPIO didn't cut rails.
	 */
	err = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF,
					    SYS_OFF_PRIO_FIRMWARE + 1,
					    hi6250_poweroff_handler, NULL);
	if (err)
		return dev_err_probe(dev, err,
				     "cannot register poweroff handler\n");
	dev_info(dev, "Hi6250 poweroff handler registered (priority %d)\n",
		 SYS_OFF_PRIO_FIRMWARE + 1);

	/*
	 * Tell the BL "kernel reached boot end" so it doesn't count this
	 * boot toward the eRecovery failsafe. We do this from probe (very
	 * late_init equivalent for a platform driver) — by the time we
	 * probe, the kernel has already brought up enough subsystems that
	 * if anything was going to fail, it would have. Stock Android
	 * writes 250 from userspace much later (when SystemUI is ready),
	 * but our PMOS userspace doesn't have an equivalent hook.
	 */
	if (pmuctrl) {
		regmap_update_bits(pmuctrl,
				   HI6250_PMIC_BOOTUP_KEYPOINT_OFFSET,
				   HI6250_BOOTUP_KEYPOINT_MASK,
				   HI6250_STAGE_BOOTUP_END);
		dev_info(dev, "wrote BOOTUP_KEYPOINT = %d (STAGE_BOOTUP_END)\n",
			 HI6250_STAGE_BOOTUP_END);

		/* See LDO4/LDO17 comment above — temporary stand-in. */
		regmap_update_bits(pmuctrl, HI6250_PMIC_LDO4_ONOFF_OFFSET,
				   HI6250_PMIC_LDO_ENABLE_BIT,
				   HI6250_PMIC_LDO_ENABLE_BIT);
		regmap_update_bits(pmuctrl, HI6250_PMIC_LDO17_ONOFF_OFFSET,
				   HI6250_PMIC_LDO_ENABLE_BIT,
				   HI6250_PMIC_LDO_ENABLE_BIT);
		dev_info(dev, "enabled PMIC LDO4 (touch vddio) + LDO17 (touch vci)\n");
	}

	return 0;
}

static const struct of_device_id hi6250_reboot_of_match[] = {
	{ .compatible = "hisilicon,hi6250-reboot" },
	{}
};
MODULE_DEVICE_TABLE(of, hi6250_reboot_of_match);

static struct platform_driver hi6250_reboot_driver = {
	.probe = hi6250_reboot_probe,
	.driver = {
		.name = "hi6250-reboot",
		.of_match_table = hi6250_reboot_of_match,
	},
};
module_platform_driver(hi6250_reboot_driver);

MODULE_DESCRIPTION("HiSilicon Hi6250 (Kirin 659) SoC reset driver");
MODULE_LICENSE("GPL v2");
