// SPDX-License-Identifier: GPL-2.0
/*
 * Hi6250 (Kirin 659) USB2 OTG wrapper driver
 *
 * Performs the SoC-specific power-on sequence for the DWC2 OTG controller.
 * Sequence derived from downstream dwc_otg_hi6250.c (Huawei kernel source).
 *
 *   1. Enable ABB reference clock (via PCTRL + PMU clock gate)
 *   2. Enable HCLK for USB2 OTG (single CRG gate)
 *   3. Deassert AHB infrastructure resets
 *   4. Configure AHBIF (ctrl0, eye diagram, VBUS valid)
 *   5. Deassert PHY POR, PHY, and controller resets
 *   6. Spawn DWC2 child platform device
 *
 * IMPORTANT: Downstream only enables BIT_HCLK_USB2OTG (bit 1) in CRG.
 * It does NOT enable CLK_USB2OTG_REF (bit 2), CLK_USB2PHY_PLL (bit 6),
 * or HCLK_USB2OTG_PMU (bit 0). It also never writes ctrl4 or ctrl5.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

/* CRG register offsets */
#define PERI_CRG_CLK_EN4	0x40
#define PERI_CRG_CLK_DIS4	0x44
#define PERI_CRG_CLKSTAT4	0x48
#define PERI_CRG_RSTEN4		0x90
#define PERI_CRG_RSTDIS4	0x94
#define PERI_CRG_RSTSTAT4	0x98

/* Clock bits in EN4/DIS4 -- downstream only uses BIT_HCLK_USB2OTG */
#define BIT_HCLK_USB2OTG	BIT(1)

/* Reset bits in RSTEN4/RSTDIS4 */
#define BIT_RST_USB2OTG		BIT(9)
#define BIT_RST_USB2OTG_MUX	BIT(10)
#define BIT_RST_USB2OTG_AHBIF	BIT(11)
#define BIT_RST_USB2OTGPHY	BIT(12)
#define BIT_RST_USB2OTGPHYPOR	BIT(13)
#define BIT_RST_USB2OTG_32K	BIT(14)
#define BIT_RST_USB2OTG_ADP	BIT(27)

/* Infrastructure resets -- deassert first to make AHBIF registers accessible */
#define USB2_RST_AHBIF		(BIT_RST_USB2OTG_ADP | BIT_RST_USB2OTG_32K | \
				 BIT_RST_USB2OTG_MUX | BIT_RST_USB2OTG_AHBIF)

/* PCTRL offset for ABB clock config */
#define PCTRL_PERI_CTRL24	0x64

/* PMU register for ABB 192 clock gate */
#define PMU_ABB_192_OFFSET	0x43c

/* AHBIF register offsets (base ff200000) */
#define AHBIF_CTRL0		0x00
#define AHBIF_CTRL2		0x08
#define AHBIF_CTRL3_EYE	0x0C
#define AHBIF_CTRL4		0x10
#define AHBIF_CTRL5		0x14

struct hi6250_usb2 {
	struct device		*dev;
	struct regmap		*pericrg;
	struct regmap		*pctrl;
	struct regmap		*pmuctrl;
	void __iomem		*ahbif;
	struct clk		*clk_abb;
	u32			eye_diagram_param;
};

static int hi6250_usb2_poweron(struct hi6250_usb2 *usb)
{
	struct device *dev = usb->dev;
	u32 val;
	int ret;

	/* Dump initial state (dev_dbg — visible with dyndbg, kept for if
	 * USB power-on ever regresses) */
	regmap_read(usb->pericrg, PERI_CRG_RSTSTAT4, &val);
	dev_dbg(dev, "initial RSTSTAT4=0x%08x\n", val);
	regmap_read(usb->pericrg, PERI_CRG_CLKSTAT4, &val);
	dev_dbg(dev, "initial CLKSTAT4=0x%08x\n", val);

	/* Step 1: Configure ABB clock mux via PCTRL_PERI_CTRL24 */
	ret = regmap_read(usb->pctrl, PCTRL_PERI_CTRL24, &val);
	if (ret)
		return ret;
	dev_dbg(dev, "PCTRL24 before=0x%08x\n", val);
	val &= ~(7 << 24);
	val |= (5 << 24);
	regmap_write(usb->pctrl, PCTRL_PERI_CTRL24, val);

	/* Step 2: Enable ABB 19.2MHz reference clock via PMU gate */
	if (usb->clk_abb) {
		ret = clk_prepare_enable(usb->clk_abb);
		if (ret) {
			dev_err(dev, "failed to enable clk_abb: %d\n", ret);
			return ret;
		}
		dev_info(dev, "clk_abb enabled (rate=%lu)\n",
			 clk_get_rate(usb->clk_abb));
	} else {
		dev_warn(dev, "clk_abb is NULL! ABB ref clock not managed\n");
	}

	/* Check framework's view of the register */
	if (usb->pmuctrl) {
		regmap_read(usb->pmuctrl, PMU_ABB_192_OFFSET, &val);
		dev_dbg(dev, "PMU ABB_192 before direct write=0x%08x\n", val);

		/*
		 * The clock framework is NOT physically writing this register
		 * (gate_flags=9 in clk-hi6250.c appears to be broken for this
		 * gate). Force bit 0 set directly — standard HiSilicon PMU gate
		 * semantics: 1=enabled.
		 */
		regmap_update_bits(usb->pmuctrl, PMU_ABB_192_OFFSET, BIT(0), BIT(0));
		udelay(10);
		regmap_read(usb->pmuctrl, PMU_ABB_192_OFFSET, &val);
		dev_dbg(dev, "PMU ABB_192 after bit0=1 write=0x%08x\n", val);

		/* If bit 0 didn't stick as 1, framework gate polarity inverted */
		if (!(val & BIT(0)))
			dev_warn(dev, "PMU ABB_192 bit0 didn't latch — PHY ref clk may be off (check PMU gate polarity)\n");
	}

	/* Step 3: Enable HCLK_USB2OTG only (matches downstream exactly) */
	regmap_write(usb->pericrg, PERI_CRG_CLK_EN4, BIT_HCLK_USB2OTG);
	udelay(100);

	regmap_read(usb->pericrg, PERI_CRG_CLKSTAT4, &val);
	dev_dbg(dev, "CLKSTAT4 after clk_en=0x%08x\n", val);

	/* Step 4: Deassert AHB infrastructure resets */
	regmap_write(usb->pericrg, PERI_CRG_RSTDIS4, USB2_RST_AHBIF);
	udelay(100);

	/* Dump AHBIF state */
	dev_dbg(dev, "AHBIF: [00]=0x%08x [08]=0x%08x [0c]=0x%08x [10]=0x%08x [14]=0x%08x\n",
		 readl(usb->ahbif + 0x00),
		 readl(usb->ahbif + 0x08), readl(usb->ahbif + 0x0c),
		 readl(usb->ahbif + 0x10), readl(usb->ahbif + 0x14));

	/*
	 * Step 5: Configure AHBIF ctrl0 (exactly as downstream).
	 *
	 * Bit layout from downstream union usbotg2_ctrl0 in
	 * drivers/usb/susb/hisi_usb_otg_type.h:
	 *   bit 0   = idpullup_sel  (not touched here — leave as default)
	 *   bit 1   = idpullup
	 *   bit 2   = acaenb_sel    -> set to 1 (register source)
	 *   bit 3   = acaenb        -> clear to 0 (ACA disabled)
	 *   bit 4-5 = id_sel        -> set to 01 (from PHY iddig)
	 *   bit 6   = id
	 *
	 * Downstream init_usb_otg_phy_hi6250 sets:
	 *   ctrl0.bits.id_sel     = 1
	 *   ctrl0.bits.acaenb_sel = 1
	 *   ctrl0.bits.acaenb     = 0
	 */
	val = readl(usb->ahbif + AHBIF_CTRL0);
	val |= BIT(2);            /* acaenb_sel = 1 */
	val &= ~BIT(3);           /* acaenb = 0 */
	val |= BIT(4);            /* id_sel low bit = 1 */
	val &= ~BIT(5);           /* id_sel high bit = 0  → id_sel = 01 (PHY iddig) */
	writel(val, usb->ahbif + AHBIF_CTRL0);

	/* Step 6: Write eye diagram parameter to ctrl3 */
	writel(usb->eye_diagram_param, usb->ahbif + AHBIF_CTRL3_EYE);

	/* Step 7: Deassert PHY POR */
	regmap_write(usb->pericrg, PERI_CRG_RSTDIS4, BIT_RST_USB2OTGPHYPOR);
	udelay(50);

	/* Step 8: Deassert PHY digital */
	regmap_write(usb->pericrg, PERI_CRG_RSTDIS4, BIT_RST_USB2OTGPHY);
	udelay(100);

	/* Step 9: Deassert controller */
	regmap_write(usb->pericrg, PERI_CRG_RSTDIS4, BIT_RST_USB2OTG);

	/* Step 10: Set VBUS valid (exactly as downstream) */
	val = readl(usb->ahbif + AHBIF_CTRL2);
	val |= BIT(2) | BIT(3);   /* vbusvldsel | vbusvldext */
	writel(val, usb->ahbif + AHBIF_CTRL2);

	/* Downstream waits 1ms here */
	msleep(1);

	/*
	 * Diagnostic probe of the DWC2 controller before the main dwc2
	 * driver attaches. The TX-FIFO-flush self-clear bit
	 * (GRSTCTL.TxFFlsh, bit 10) can only complete when the PHY ref
	 * clock is presented to the controller — so this is an indirect
	 * probe of PHY-clock readiness.
	 *
	 * On Hi6250 this point in init is BEFORE the controller has
	 * synced to the PHY clock (that happens later in dwc2 core
	 * probe via PCGCTL writes), so seeing TxFFlsh still set here is
	 * the *expected* state, not a failure. We keep the diagnostic
	 * (dev_dbg) so an actual PHY-clock regression — bit10 stuck on
	 * a later boot when we ALSO see USB enumeration fail — has a
	 * traceable signature.
	 */
	{
		void __iomem *dwc = ioremap(0xff100000, 0x1000);
		if (dwc) {
			u32 grstctl;

			dev_dbg(dev, "DWC2: GSNPSID=0x%08x GUSBCFG=0x%08x PCGCTL=0x%08x\n",
				readl(dwc + 0x40), readl(dwc + 0x0c),
				readl(dwc + 0xe00));

			grstctl = readl(dwc + 0x10);
			dev_dbg(dev, "GRSTCTL=0x%08x\n", grstctl);

			writel(0x80000410, dwc + 0x10);
			udelay(200);
			grstctl = readl(dwc + 0x10);
			dev_dbg(dev,
				"GRSTCTL after TxFFlsh=0x%08x (%s; main dwc2 probe will complete PHY sync)\n",
				grstctl,
				(grstctl & BIT(10)) ?
					"TxFFlsh pending — PHY clock not yet synced (expected at this stage)" :
					"TxFFlsh cleared — PHY clock already synced");

			iounmap(dwc);
		}
	}

	/* Final state dumps (dev_dbg — kept for regression triage) */
	dev_dbg(dev, "AHBIF final: [00]=0x%08x [08]=0x%08x [10]=0x%08x [14]=0x%08x\n",
		readl(usb->ahbif + 0x00),
		readl(usb->ahbif + 0x08),
		readl(usb->ahbif + 0x10), readl(usb->ahbif + 0x14));
	regmap_read(usb->pericrg, PERI_CRG_RSTSTAT4, &val);
	dev_dbg(dev, "final RSTSTAT4=0x%08x\n", val);
	regmap_read(usb->pericrg, PERI_CRG_CLKSTAT4, &val);
	dev_dbg(dev, "final CLKSTAT4=0x%08x\n", val);

	dev_info(dev, "USB2 OTG power-on complete\n");
	return 0;
}

static void hi6250_usb2_poweroff(struct hi6250_usb2 *usb)
{
	/* Exactly as downstream close_usb_otg_phy_hi6250 */
	regmap_write(usb->pericrg, PERI_CRG_RSTEN4, BIT_RST_USB2OTG);
	udelay(1);
	regmap_write(usb->pericrg, PERI_CRG_RSTEN4, BIT_RST_USB2OTGPHY);
	regmap_write(usb->pericrg, PERI_CRG_RSTEN4, BIT_RST_USB2OTGPHYPOR);
	regmap_write(usb->pericrg, PERI_CRG_RSTEN4, USB2_RST_AHBIF);
	regmap_write(usb->pericrg, PERI_CRG_CLK_DIS4, BIT_HCLK_USB2OTG);
	msleep(1);
	if (usb->clk_abb)
		clk_disable_unprepare(usb->clk_abb);
}

static int hi6250_usb2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi6250_usb2 *usb;
	struct resource *res;
	int ret;

	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;

	usb->dev = dev;
	platform_set_drvdata(pdev, usb);

	/* Get CRG syscon */
	usb->pericrg = syscon_regmap_lookup_by_phandle(dev->of_node,
					"hisilicon,pericrg-syscon");
	if (IS_ERR(usb->pericrg)) {
		dev_err(dev, "no hisilicon,pericrg-syscon\n");
		return PTR_ERR(usb->pericrg);
	}

	/* Get PCTRL syscon */
	usb->pctrl = syscon_regmap_lookup_by_phandle(dev->of_node,
					"hisilicon,pctrl-syscon");
	if (IS_ERR(usb->pctrl)) {
		dev_err(dev, "no hisilicon,pctrl-syscon\n");
		return PTR_ERR(usb->pctrl);
	}

	/* Get PMU syscon (optional, for diagnostics) */
	usb->pmuctrl = syscon_regmap_lookup_by_phandle(dev->of_node,
					"hisilicon,pmuctrl-syscon");
	if (IS_ERR(usb->pmuctrl)) {
		dev_info(dev, "no hisilicon,pmuctrl-syscon (diag only)\n");
		usb->pmuctrl = NULL;
	}

	/* Map AHBIF registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	usb->ahbif = devm_ioremap_resource(dev, res);
	if (IS_ERR(usb->ahbif))
		return PTR_ERR(usb->ahbif);

	/* Get ABB 19.2MHz clock (USB PHY reference) */
	usb->clk_abb = devm_clk_get_optional(dev, "clk_abb_192");
	if (IS_ERR(usb->clk_abb))
		return dev_err_probe(dev, PTR_ERR(usb->clk_abb),
				     "failed to get clk_abb_192\n");

	/* Get eye diagram parameter from DT */
	if (of_property_read_u32(dev->of_node, "hisilicon,eye-diagram-param",
				 &usb->eye_diagram_param))
		usb->eye_diagram_param = 0x059066DB;

	/* Power on USB2 subsystem */
	ret = hi6250_usb2_poweron(usb);
	if (ret) {
		dev_err(dev, "USB2 power-on failed: %d\n", ret);
		return ret;
	}

	/* Spawn child DWC2 device */
	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret) {
		dev_err(dev, "failed to populate children: %d\n", ret);
		hi6250_usb2_poweroff(usb);
		return ret;
	}

	dev_info(dev, "Hi6250 USB2 OTG initialized\n");
	return 0;
}

static void hi6250_usb2_remove(struct platform_device *pdev)
{
	struct hi6250_usb2 *usb = platform_get_drvdata(pdev);

	of_platform_depopulate(usb->dev);
	hi6250_usb2_poweroff(usb);
}

static const struct of_device_id hi6250_usb2_of_match[] = {
	{ .compatible = "hisilicon,hi6250-usb" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi6250_usb2_of_match);

static struct platform_driver hi6250_usb2_driver = {
	.probe	= hi6250_usb2_probe,
	.remove	= hi6250_usb2_remove,
	.driver	= {
		.name = "dwc2-hi6250",
		.of_match_table = hi6250_usb2_of_match,
	},
};
module_platform_driver(hi6250_usb2_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Hi6250 USB2 OTG wrapper driver");
