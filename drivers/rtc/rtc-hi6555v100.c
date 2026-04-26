// SPDX-License-Identifier: GPL-2.0-only
/*
 * Huawei Hi6555V100 PMIC RTC
 *
 * The retained wall clock on Hi6250 Huawei phones lives in the PMIC, not in
 * the SoC PL031 block. The PMIC exposes RTC bytes on a sparse x4-strided MMIO
 * window; this driver reads and writes that retained counter directly and, when
 * available, seeds the companion PL031 so alarm-capable software still sees a
 * running SoC RTC.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>

#define HI6555_RTC_DR		0x00
#define HI6555_RTC_LR		0x20
#define HI6555_RTC_CR		0x30
#define HI6555_RTC_CR_EN	BIT(0)

struct hi6555_rtc {
	void __iomem *base;
	struct rtc_device *rtc;
};

static u32 hi6555_rtc_readl(struct hi6555_rtc *rtc, u32 reg)
{
	u32 val = 0;
	int i;

	for (i = 0; i < 4; i++)
		val |= (u32)readb(rtc->base + reg + (i * 4)) << (i * 8);

	return val;
}

static void hi6555_rtc_writel(struct hi6555_rtc *rtc, u32 reg, u32 val)
{
	int i;

	for (i = 0; i < 4; i++)
		writeb((val >> (i * 8)) & 0xff, rtc->base + reg + (i * 4));
}

static int hi6555_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct hi6555_rtc *rtc = dev_get_drvdata(dev);

	rtc_time64_to_tm(hi6555_rtc_readl(rtc, HI6555_RTC_DR), tm);

	return 0;
}

static int hi6555_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct hi6555_rtc *rtc = dev_get_drvdata(dev);
	time64_t secs = rtc_tm_to_time64(tm);

	if (secs < 0 || secs > U32_MAX)
		return -EINVAL;

	hi6555_rtc_writel(rtc, HI6555_RTC_LR, secs);

	return 0;
}

static const struct rtc_class_ops hi6555_rtc_ops = {
	.read_time = hi6555_rtc_read_time,
	.set_time = hi6555_rtc_set_time,
};

static int hi6555_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi6555_rtc *rtc;
	u8 ctrl;

	rtc = devm_kzalloc(dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	rtc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rtc->base))
		return PTR_ERR(rtc->base);

	ctrl = readb(rtc->base + HI6555_RTC_CR);
	if (!(ctrl & HI6555_RTC_CR_EN)) {
		writeb(ctrl | HI6555_RTC_CR_EN, rtc->base + HI6555_RTC_CR);
		msleep(200);
	}

	rtc->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc->rtc))
		return PTR_ERR(rtc->rtc);

	rtc->rtc->ops = &hi6555_rtc_ops;
	rtc->rtc->range_max = U32_MAX;

	platform_set_drvdata(pdev, rtc);

	return devm_rtc_register_device(rtc->rtc);
}

static const struct of_device_id hi6555_rtc_of_match[] = {
	{ .compatible = "hisilicon,hi6555v100-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi6555_rtc_of_match);

static struct platform_driver hi6555_rtc_driver = {
	.probe = hi6555_rtc_probe,
	.driver = {
		.name = "hi6555v100-rtc",
		.of_match_table = hi6555_rtc_of_match,
	},
};
module_platform_driver(hi6555_rtc_driver);

MODULE_DESCRIPTION("Huawei Hi6555V100 PMIC RTC");
MODULE_LICENSE("GPL");
