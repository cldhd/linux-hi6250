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
 * This handler registers at priority 200 so it runs before PSCI's
 * handler. It tries PSCI itself first (so on a board that DOES
 * implement it, that path is preferred and the SoC is reset
 * immediately by the trustzone) and only pulses the LPM3 NMI when
 * PSCI returns. The LPM3 NMI does not return — the SoC resets
 * within ~500 ms of the pulse — so the post-NMI mdelay and the
 * NOTIFY_DONE return below are reached only if the LPM3 path also
 * fails, in which case the kernel halt path takes over and the
 * device must be force-power-cycled into TWRP for recovery.
 *
 * Verified working live on Honor 9 Lite 2026-04-27: PMIC RTC
 * survives the reset, BL boots back into PMOS in ~43 s, multiple
 * reboots in succession all succeed, no eRecovery failsafe trip.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
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
 */
#define HI6250_PMIC_HRST_REG0_OFFSET	0x62c
#define HI6250_PMIC_HRST_REASON_MASK	0xff
#define HI6250_REBOOT_REASON_COLDBOOT	0x10

/* Sysctrl SCLPMCUCTRL: bit 2 = nmi_in to LPM3 */
#define HI6250_SCTRL_SCLPMCUCTRL	0x510
#define HI6250_SCTRL_NMI_IN_BIT		BIT(2)

static struct regmap *pmuctrl;
static struct regmap *sysctrl;

static int hi6250_restart_handler(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	struct arm_smccc_res res = {};

	pr_emerg("hi6250-reboot: ENTER restart_handler mode=%lu cmd=%s\n",
		 mode, cmd ? (const char *)cmd : "(null)");

	/* 1. BL marker — keep the bootloader's eRecovery failsafe at rest. */
	if (pmuctrl)
		regmap_update_bits(pmuctrl, HI6250_PMIC_HRST_REG0_OFFSET,
				   HI6250_PMIC_HRST_REASON_MASK,
				   HI6250_REBOOT_REASON_COLDBOOT);

	/*
	 * 2. Try PSCI SYSTEM_RESET. On this board's trustzone this
	 * returns PSCI_RET_NOT_SUPPORTED (0xffffffff), but a more
	 * complete trustzone implementation might handle it and reset
	 * the SoC inside the SMC, in which case we never get past the
	 * arm_smccc_smc() call.
	 */
	arm_smccc_smc(PSCI_0_2_FN_SYSTEM_RESET, 0, 0, 0, 0, 0, 0, 0, &res);
	pr_emerg("hi6250-reboot: PSCI SYSTEM_RESET returned a0=0x%lx\n", res.a0);
	mdelay(100);

	/* 3. Pulse LPM3 NMI — the path that actually resets this SoC. */
	if (sysctrl) {
		pr_emerg("hi6250-reboot: pulsing SCLPMCUCTRL.nmi_in to LPM3\n");
		regmap_update_bits(sysctrl, HI6250_SCTRL_SCLPMCUCTRL,
				   HI6250_SCTRL_NMI_IN_BIT,
				   HI6250_SCTRL_NMI_IN_BIT);
	}
	mdelay(500);

	pr_emerg("hi6250-reboot: LPM3 NMI did not reset the SoC; halting\n");
	return NOTIFY_DONE;
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

	err = register_restart_handler(&hi6250_restart_nb);
	if (err)
		dev_err(dev, "cannot register restart handler (err=%d)\n", err);
	else
		dev_info(dev, "Hi6250 reboot handler registered (priority %d)\n",
			 hi6250_restart_nb.priority);

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
