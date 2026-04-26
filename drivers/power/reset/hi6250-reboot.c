// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi6250 (Kirin 659) SoC reset code.
 *
 * Unlike hi6220, the hi6250 does not reset via a simple magic-value
 * write to a sysctrl register. Instead the LPM3 co-processor handles
 * SoC reset: the AP writes a "reboot reason" byte into the PMIC HRST
 * register (memory-mapped through PMUCTRL) and pulses bit 2 of
 * sysctrl + 0x510 (NMI_NOTIFY_LPM3). LPM3 firmware then performs the
 * actual reset, and the Huawei BL1/BL2 bootloader reads the reason
 * byte to decide whether the previous boot ended cleanly.
 *
 * Skipping the reason write lets the byte sit at its power-on default
 * of 0xff, which the bootloader interprets as an abnormal reset; after
 * a handful of such resets the bootloader's failsafe counter trips and
 * it force-boots into eRecovery. Writing COLDBOOT (0x10) tells the
 * bootloader "this was a user-requested reboot" and keeps the counter
 * at rest.
 *
 * Mirrors the downstream `hisi_pm_system_reset` + `set_reboot_reason`
 * + `hisiap_nmi_notify_lpm3` path from Huawei's labyrinth_kernel_prague
 * 4.4 fork.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

#include <asm/proc-fns.h>

#define HI6250_NMI_NOTIFY_LPM3_OFFSET	0x510
#define HI6250_NMI_NOTIFY_LPM3_BIT	BIT(2)

/* PMIC HRST_REG0 lives inside the PMUCTRL block. PMIC regs are strided
 * x4 (val_bits=8, reg_stride=4), so reg 0x18B is at byte offset 0x62c.
 */
#define HI6250_PMIC_HRST_REG0_OFFSET	0x62c
#define HI6250_PMIC_HRST_REASON_MASK	0xff
#define HI6250_REBOOT_REASON_COLDBOOT	0x10

static struct regmap *sysctrl;
static struct regmap *pmuctrl;

static int hi6250_restart_handler(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	/* 1. Tag the reset as "user-requested cold boot" in the PMIC
	 * HRST register so the bootloader does not count it as an
	 * abnormal reset and trip its eRecovery failsafe.
	 */
	if (pmuctrl)
		regmap_update_bits(pmuctrl, HI6250_PMIC_HRST_REG0_OFFSET,
				   HI6250_PMIC_HRST_REASON_MASK,
				   HI6250_REBOOT_REASON_COLDBOOT);

	/* 2. Rising edge on bit 2 of sysctrl + 0x510 NMIs the LPM3,
	 * which triggers the actual SoC reset.
	 */
	regmap_update_bits(sysctrl, HI6250_NMI_NOTIFY_LPM3_OFFSET,
			   HI6250_NMI_NOTIFY_LPM3_BIT,
			   HI6250_NMI_NOTIFY_LPM3_BIT);
	regmap_update_bits(sysctrl, HI6250_NMI_NOTIFY_LPM3_OFFSET,
			   HI6250_NMI_NOTIFY_LPM3_BIT, 0);

	/* LPM3 reset latency can exceed several seconds. Spin here so the
	 * kernel does not fall through to machine_restart() cleanup (which
	 * tears down fbcon/DRM and briefly restores the underlying
	 * framebuffer image) and then loop in "Reboot failed -- System
	 * halted" while waiting for LPM3.
	 */
	while (1)
		cpu_do_idle();

	return NOTIFY_DONE;
}

static struct notifier_block hi6250_restart_nb = {
	.notifier_call = hi6250_restart_handler,
	.priority = 128,
};

static int hi6250_reboot_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int err;

	sysctrl = syscon_regmap_lookup_by_phandle(dev->of_node,
						  "hisilicon,sys-ctrl");
	if (IS_ERR(sysctrl))
		return dev_err_probe(dev, PTR_ERR(sysctrl),
				     "failed to get sys-ctrl regmap\n");

	/* Optional: PMUCTRL is used to tag the reset reason for the
	 * bootloader. If missing we still reboot via LPM3, but the
	 * bootloader's failsafe counter may trip after repeated resets.
	 */
	pmuctrl = syscon_regmap_lookup_by_phandle(dev->of_node,
						  "hisilicon,pmu-ctrl");
	if (IS_ERR(pmuctrl)) {
		dev_warn(dev, "no pmu-ctrl regmap (%ld); skipping reboot-reason tag\n",
			 PTR_ERR(pmuctrl));
		pmuctrl = NULL;
	}

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
