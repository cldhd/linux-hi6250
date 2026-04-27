// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi6250 (Kirin 659) reboot-reason marker.
 *
 * The Huawei BL1/BL2 on Kirin 659 phones reads PMIC register 0x18B
 * (PMIC HRST_REG0, memory-mapped through PMUCTRL at byte offset 0x62c
 * with x4 stride) to decide whether the previous boot ended cleanly.
 * Leaving the byte at the power-on default of 0xff is interpreted by
 * the BL as an abnormal reset; after a handful of these the BL's
 * failsafe counter trips and it force-boots into eRecovery instead
 * of system.
 *
 * This driver therefore writes COLDBOOT (0x10) into HRST_REG0 from
 * its restart_handler and returns NOTIFY_DONE. The actual SoC reset
 * is left to mainline's PSCI restart handler, which is registered at
 * priority 129 in drivers/firmware/psci/ and runs after this one
 * (this handler registers at priority 200).
 *
 * KNOWN LIMITATION: PSCI's SYSTEM_RESET only resets the SoC reliably
 * on the *first* `reboot` after a TWRP/eRecovery cold boot. From the
 * second reboot onwards the trustzone returns control to the kernel
 * without resetting and the kernel halts at "Requesting system
 * reboot". The fix would need either a working SP805 watchdog (this
 * SoC's WDT clock state across resets is not yet reverse-engineered)
 * or a working LPM3 mailbox path (downstream Huawei kernel uses a
 * complex multi-step IPC, not the simple NMI pulse the older version
 * of this driver tried). Workaround: power-cycle into TWRP via
 * Vol-Up + Power, then reboot from there.
 */
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

/*
 * PMIC HRST_REG0 lives inside the PMUCTRL block. PMIC regs are strided
 * x4 (val_bits=8, reg_stride=4), so reg 0x18B is at byte offset 0x62c.
 */
#define HI6250_PMIC_HRST_REG0_OFFSET	0x62c
#define HI6250_PMIC_HRST_REASON_MASK	0xff
#define HI6250_REBOOT_REASON_COLDBOOT	0x10

static struct regmap *pmuctrl;

static int hi6250_restart_handler(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	/*
	 * Tag the reset as "user-requested cold boot" in the PMIC HRST
	 * register so the Huawei BL does not count it as an abnormal
	 * reset and trip its eRecovery failsafe.
	 */
	if (pmuctrl)
		regmap_update_bits(pmuctrl, HI6250_PMIC_HRST_REG0_OFFSET,
				   HI6250_PMIC_HRST_REASON_MASK,
				   HI6250_REBOOT_REASON_COLDBOOT);

	return NOTIFY_DONE;
}

static struct notifier_block hi6250_restart_nb = {
	.notifier_call = hi6250_restart_handler,
	/*
	 * Mainline's PSCI restart handler in drivers/firmware/psci/ uses
	 * .priority = 129. Notifier chains are walked in decreasing
	 * priority order, so we must register at >129 to run BEFORE PSCI
	 * — otherwise PSCI either resets the SoC before we have a chance
	 * to set the BL marker (causing the BL to count this reboot as
	 * abnormal and eventually force-boot into eRecovery), or PSCI's
	 * SMC fails to reset and we end up writing the marker too late
	 * for any reset to happen at all (system hangs at
	 * "Requesting system reboot").
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

	err = register_restart_handler(&hi6250_restart_nb);
	if (err)
		dev_err(dev, "cannot register restart handler (err=%d)\n", err);

	return err;
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
