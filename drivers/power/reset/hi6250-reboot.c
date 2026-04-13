// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi6250 (Kirin 659) SoC reset code.
 *
 * Unlike hi6220, the hi6250 does not reset via a simple magic-value
 * write to a sysctrl register. Instead the LPM3 co-processor handles
 * SoC reset: the AP pulses bit 2 of sysctrl + 0x510 (NMI_NOTIFY_LPM3)
 * and LPM3 firmware performs the actual reset.
 *
 * Mirrors the downstream `hisi_pm_system_reset` + `hisiap_nmi_notify_lpm3`
 * path used by Huawei's labyrinth_kernel_prague 4.4 fork, trimmed to
 * the essential mailbox pulse (no PMIC reboot-reason write, since LPM3
 * cold-boots fine with whatever reason is already there).
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

#define HI6250_NMI_NOTIFY_LPM3_OFFSET	0x510
#define HI6250_NMI_NOTIFY_LPM3_BIT	BIT(2)

static struct regmap *sysctrl;

static int hi6250_restart_handler(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	/* Rising edge on bit 2 of sysctrl + 0x510 NMIs the LPM3, which
	 * triggers the actual SoC reset.
	 */
	regmap_update_bits(sysctrl, HI6250_NMI_NOTIFY_LPM3_OFFSET,
			   HI6250_NMI_NOTIFY_LPM3_BIT,
			   HI6250_NMI_NOTIFY_LPM3_BIT);
	regmap_update_bits(sysctrl, HI6250_NMI_NOTIFY_LPM3_OFFSET,
			   HI6250_NMI_NOTIFY_LPM3_BIT, 0);

	mdelay(1000);
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
