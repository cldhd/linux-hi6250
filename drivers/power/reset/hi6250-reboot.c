// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi6250 (Kirin 659) reboot-reason marker.
 *
 * The Huawei BL1/BL2 bootloader on Kirin 659 phones reads a "reboot
 * reason" byte from PMIC register 0x18B (PMIC HRST_REG0, memory-mapped
 * through PMUCTRL at byte offset 0x62c with x4 stride) to decide
 * whether the previous boot ended cleanly. Leaving the byte at its
 * power-on default of 0xff is interpreted by the BL as an abnormal
 * reset; after a handful of these the BL's failsafe counter trips and
 * it force-boots into eRecovery instead of the system kernel.
 *
 * This driver registers a restart_handler that simply writes
 * COLDBOOT (0x10) to that byte and returns. The actual SoC reset is
 * left to mainline's PSCI restart handler (drivers/firmware/psci/),
 * which runs after this one and issues PSCI_0_2_FN_SYSTEM_RESET via
 * SMC. The trustzone implementation drives LPM3 internally and the
 * SoC resets reliably.
 *
 * An earlier version of this driver pulsed bit 2 of sysctrl + 0x510
 * (SCLPMCUCTRL.nmi_in) to NMI the LPM3 co-processor directly and then
 * spun in `cpu_do_idle()` waiting for LPM3 to act. In practice on this
 * board that NMI did not always trigger a reset and the spin loop
 * never returned, leaving the kernel hung at the end of the reboot
 * sequence until the device was forcibly power-cycled. PSCI is the
 * reliable path; this driver only stays around to set the BL marker.
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
	 * register so the Huawei bootloader does not count it as an
	 * abnormal reset and trip its eRecovery failsafe (after a few
	 * "abnormal" resets the BL force-boots into eRecovery).
	 *
	 * That is the ONLY thing this handler does. The actual SoC reset
	 * is left to mainline's PSCI restart handler (priority 0, runs
	 * after this one). The trustzone implementation of
	 * PSCI_0_2_FN_SYSTEM_RESET drives LPM3 internally and resets the
	 * SoC reliably; the custom NMI-LPM3 path that used to live here
	 * had a tendency to hang indefinitely waiting for LPM3 to act.
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
	 * Higher priority than mainline PSCI's restart handler (which
	 * registers at priority 0 in drivers/firmware/psci/), so this
	 * runs first and gets the marker into the PMIC before PSCI
	 * triggers the actual SoC reset.
	 */
	.priority = 128,
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
