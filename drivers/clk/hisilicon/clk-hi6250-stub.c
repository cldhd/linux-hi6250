// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hi6250 stub clock driver
 *
 * Copyright (C) 2025, Tildeguy (tildeguy@mainlining.org)
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <dt-bindings/clock/hi6250-clock.h>

#define HI6250_STUB_CLOCK_BASE    (0x41C)

/*
 * hi3660-style mailbox register layout (matches drivers/mailbox/hi3660-mailbox.c).
 * We talk to the controller directly here instead of going through the mbox
 * subsystem, because mainline's mbox_chan API has no sync-send-with-reply path
 * and we need to actually observe whether LPM3 acked our vote — auto-ack mode
 * has the *hardware* ack the message immediately whether or not LPM3 saw it,
 * so it provides no information about LPM3 firmware state.
 */
#define MBOX_CH_REG(ch, off)		((ch) * 0x40 + (off))
#define MBOX_SRC_REG			0x00
#define MBOX_DST_REG			0x04
#define MBOX_DCLR_REG			0x08
#define MBOX_DSTAT_REG			0x0c
#define MBOX_MODE_REG			0x10
#define MBOX_IMASK_REG			0x14
#define MBOX_ICLR_REG			0x18
#define MBOX_SEND_REG			0x1c
#define MBOX_DATA_REG			0x20

#define MBOX_IPC_LOCK_REG		0xa00
#define MBOX_IPC_UNLOCK			0x1acce551

#define MBOX_MODE_AUTO_ACK		BIT(0)
#define MBOX_STATE_IDLE			BIT(4)
#define MBOX_STATE_READY		BIT(5)
#define MBOX_STATE_ACK			BIT(7)

#define MBOX_MSG_LEN			8

#define DEFINE_CLK_STUB(_id, _freqs, _cmd, _name) \
	{                                         \
		.id = (_id),                            \
		.freqs = (_freqs),                      \
		.cmd = (_cmd),                          \
		.hw.init = &(struct clk_init_data) {    \
			.name = #_name,                       \
			.ops = &hi6250_stub_clk_ops,          \
			.num_parents = 0,                     \
			.flags = CLK_GET_RATE_NOCACHE,        \
		},                                      \
	}

#define to_stub_clk(_hw) container_of(_hw, struct hi6250_stub_clk, hw)

struct hi6250_stub_clk {
	unsigned int id;
	struct clk_hw hw;
	unsigned long *freqs;
	unsigned int cmd;
	unsigned int rate; // mhz
};

/*
 * Direct (non-mbox-subsystem) access state for talking to LPM3.
 * Channel 13 is AP→LPM3 for stub-clock votes per hi6250.dtsi
 *   (mboxes = <&mailbox 13 3 0> — channel 13, dst_irq 3, ack_irq 0).
 */
struct hi6250_lpm3 {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;
	unsigned int chan;
	unsigned int dst_irq;
	unsigned int ack_irq;
};

static void __iomem *freq_reg;
static struct hi6250_lpm3 lpm3;

static int hi3660_mbox_unlock(void __iomem *base)
{
	unsigned int val;
	int retry = 3;

	do {
		writel(MBOX_IPC_UNLOCK, base + MBOX_IPC_LOCK_REG);
		val = readl(base + MBOX_IPC_LOCK_REG);
		if (!val)
			return 0;
		udelay(10);
	} while (retry--);

	return -ETIMEDOUT;
}

/*
 * Sync xfer: send `tx` (8 words) on `chan`, wait for LPM3 to manually ACK
 * and write a reply into the same MBOX_DATA_REG slots. Returns 0 on
 * success, -ETIMEDOUT if LPM3 didn't ACK within `timeout_us`.
 *
 * Pass rx=NULL to discard the reply (still requires ACK).
 */
static int hi6250_lpm3_xfer_sync_chan(struct hi6250_lpm3 *l, unsigned int chan,
				      const u32 tx[MBOX_MSG_LEN],
				      u32 rx[MBOX_MSG_LEN],
				      unsigned int timeout_us)
{
	void __iomem *cb = l->base + MBOX_CH_REG(chan, 0);
	unsigned int val, sval;
	int ret, i, retry;

	mutex_lock(&l->lock);

	ret = hi3660_mbox_unlock(l->base);
	if (ret) {
		dev_err(l->dev, "ipc unlock timeout\n");
		goto out;
	}

	for (retry = 10; retry; retry--) {
		if (readl(cb + MBOX_MODE_REG) & MBOX_STATE_IDLE) {
			writel(BIT(l->ack_irq), cb + MBOX_SRC_REG);
			sval = readl(cb + MBOX_SRC_REG);
			if (sval & BIT(l->ack_irq))
				break;
		}
	}
	if (!retry) {
		dev_err(l->dev, "channel %u acquire timeout\n", chan);
		ret = -ETIMEDOUT;
		goto out;
	}

	/* Wait for any prior outstanding ACK on this channel to be cleared. */
	if (!(readl(cb + MBOX_MODE_REG) & MBOX_STATE_READY)) {
		ret = readl_poll_timeout_atomic(cb + MBOX_MODE_REG, val,
						(val & MBOX_STATE_ACK), 10, 5000);
		if (ret) {
			dev_err(l->dev, "stale state, no prior ACK seen\n");
			goto out;
		}
		writel(BIT(l->ack_irq), cb + MBOX_ICLR_REG);
	}

	/* Configure: dest IRQ = LPM3 wake vector, manual ack mode. */
	writel_relaxed(~BIT(l->dst_irq), cb + MBOX_IMASK_REG);
	writel_relaxed(BIT(l->dst_irq), cb + MBOX_DST_REG);
	writel_relaxed(0 /* manual ACK */, cb + MBOX_MODE_REG);

	/* Fill data and trigger. */
	for (i = 0; i < MBOX_MSG_LEN; i++)
		writel_relaxed(tx[i], cb + MBOX_DATA_REG + i * 4);
	writel(BIT(l->ack_irq), cb + MBOX_SEND_REG);

	/* Wait for LPM3 to write its reply and assert ACK. */
	ret = readl_poll_timeout(cb + MBOX_MODE_REG, val,
				 (val & MBOX_STATE_ACK), 100, timeout_us);
	if (ret) {
		dev_err(l->dev, "no ACK from LPM3 within %u us (cmd=0x%08x)\n",
			timeout_us, tx[0]);
		goto release;
	}

	/* Read LPM3's reply. */
	if (rx) {
		for (i = 0; i < MBOX_MSG_LEN; i++)
			rx[i] = readl_relaxed(cb + MBOX_DATA_REG + i * 4);
	}

release:
	/* Clear our ack so the channel goes idle. */
	writel(BIT(l->ack_irq), cb + MBOX_ICLR_REG);
out:
	mutex_unlock(&l->lock);
	return ret;
}

static unsigned long hi6250_stub_clk_recalc_rate(
  struct clk_hw *hw,
	unsigned long parent_rate
) {
	struct hi6250_stub_clk *stub_clk = to_stub_clk(hw);

	unsigned int freq_id = (readl(freq_reg) >> (stub_clk->id * 4)) & 0xf;

	stub_clk->rate = stub_clk->freqs[freq_id];
	return stub_clk->rate;
}

static int hi6250_stub_clk_determine_rate(
  struct clk_hw *hw,
  struct clk_rate_request *req
) {
	return 0;
}

/*
 * LPM3 channels (matches downstream's HISI_RPROC_LPM3_MBX{13,14}):
 *  - chan 13: stub-clock frequency votes (cmd 0x000Z030[A-F])
 *  - chan 14: regulator enable/disable (cmd 0x000Y0[01]04)
 *
 * Both go to LPM3 (dst_irq 3, ack_irq 0), but they're handled by
 * different services on the LPM3 side. Sending a freq vote alone does
 * NOT power the rail on; the GPU also needs the matching regulator
 * enable on chan 14, mirroring `g3d`'s `hi3xxx_to_lpm3_enable_step =
 * <0x00030004 0x00>` in hisi_6250_powerip.dtsi.
 *
 * Per-stub regulator IDs from that DTS:
 *  - g3d  = 14 → cmd 0x00030004 / 0x00030104
 *  Other stubs (CPU clusters, DDR) don't need an explicit enable
 *  — they're always on.
 */
static const u32 hi6250_stub_clk_reg_enable_cmd[HI6250_CLK_STUB_NUM] = {
	[HI6250_CLK_STUB_GPU] = 0x00030004,
};

static const u32 hi6250_stub_clk_reg_disable_cmd[HI6250_CLK_STUB_NUM] = {
	[HI6250_CLK_STUB_GPU] = 0x00030104,
};

#define LPM3_CHAN_FREQ_VOTE  13
#define LPM3_CHAN_REGULATOR  14

static int hi6250_stub_clk_send_vote(struct hi6250_stub_clk *stub_clk,
				     unsigned int mhz)
{
	u32 tx[MBOX_MSG_LEN] = { stub_clk->cmd, mhz };
	u32 rx[MBOX_MSG_LEN] = { 0 };
	int ret;

	ret = hi6250_lpm3_xfer_sync_chan(&lpm3, LPM3_CHAN_FREQ_VOTE,
					 tx, rx, 200000);
	dev_info(lpm3.dev,
		 "stub-clk freq id=%u cmd=0x%08x mhz=%u → ret=%d ack[0]=0x%08x ack[1]=0x%08x\n",
		 stub_clk->id, tx[0], mhz, ret, rx[0], rx[1]);
	return ret;
}

static int hi6250_stub_clk_send_regulator(struct hi6250_stub_clk *stub_clk,
					  bool enable)
{
	u32 cmd = enable
		? hi6250_stub_clk_reg_enable_cmd[stub_clk->id]
		: hi6250_stub_clk_reg_disable_cmd[stub_clk->id];
	u32 tx[MBOX_MSG_LEN] = { cmd, 0 };
	u32 rx[MBOX_MSG_LEN] = { 0 };
	int ret;

	if (cmd == 0)
		return 0;	/* no regulator wrapper for this stub */

	ret = hi6250_lpm3_xfer_sync_chan(&lpm3, LPM3_CHAN_REGULATOR,
					 tx, rx, 200000);
	dev_info(lpm3.dev,
		 "stub-clk regulator id=%u %s cmd=0x%08x → ret=%d ack[0]=0x%08x ack[1]=0x%08x\n",
		 stub_clk->id, enable ? "ENABLE" : "DISABLE",
		 cmd, ret, rx[0], rx[1]);
	return ret;
}

static int hi6250_stub_clk_set_rate(
  struct clk_hw *hw,
  unsigned long rate,
	unsigned long parent_rate
) {
	struct hi6250_stub_clk *stub_clk = to_stub_clk(hw);

	hi6250_stub_clk_send_vote(stub_clk, rate / 1000000);
	stub_clk->rate = rate;
	return 0;
}

/*
 * Default opening rate (MHz) per stub clock id. The LPM3 command takes
 * a frequency in MHz as msg[1].
 *
 * GPU defaults to 480 MHz — matches the downstream BL's `vreg_mali`
 * stand-in voltage of 744 mV (mid-OPP between 696 mV @ 120 MHz and
 * 1073 mV @ 900 MHz), enough headroom for Phosh compositing without
 * burning idle power. Without devfreq wired up, this is what panfrost
 * sees during the entire session — bumping it from 120 MHz visibly
 * improves UI responsiveness.
 */
static const unsigned int hi6250_stub_clk_default_mhz[HI6250_CLK_STUB_NUM] = {
	[HI6250_CLK_STUB_CLUSTER0] = 480,
	[HI6250_CLK_STUB_CLUSTER1] = 1402,
	[HI6250_CLK_STUB_DDR]      = 240,
	[HI6250_CLK_STUB_GPU]      = 480,
};

static int hi6250_stub_clk_prepare(struct clk_hw *hw)
{
	struct hi6250_stub_clk *stub_clk = to_stub_clk(hw);
	unsigned int mhz = hi6250_stub_clk_default_mhz[stub_clk->id];
	int ret;

	/* Frequency vote first — for the GPU this also tells LPM3 what
	 * rate to bring the rail up at. */
	ret = hi6250_stub_clk_send_vote(stub_clk, mhz);
	if (ret)
		return ret;

	/* Then enable the regulator wrapper (channel 14). For non-GPU
	 * stubs this is a no-op since they don't need explicit enable. */
	return hi6250_stub_clk_send_regulator(stub_clk, true);
}

static void hi6250_stub_clk_unprepare(struct clk_hw *hw)
{
	struct hi6250_stub_clk *stub_clk = to_stub_clk(hw);

	hi6250_stub_clk_send_regulator(stub_clk, false);
	hi6250_stub_clk_send_vote(stub_clk, 0);
}

static const struct clk_ops hi6250_stub_clk_ops = {
	.prepare        = hi6250_stub_clk_prepare,
	.unprepare      = hi6250_stub_clk_unprepare,
	.recalc_rate    = hi6250_stub_clk_recalc_rate,
	.determine_rate = hi6250_stub_clk_determine_rate,
	.set_rate       = hi6250_stub_clk_set_rate,
};

/*
 * Frequency tables in Hz (matches CCF/OPP convention). LPM3 receives
 * rate in MHz (rate/1000000) — see hi6250_stub_clk_send_vote.
 * Index in each array maps to LPM3's freq_id stored in
 * sysctrl + HI6250_STUB_CLOCK_BASE.
 */
static unsigned long hi6250_stub_clk_freqs_cluster0[] = {
	 480000000,
	 807000000,
	1306000000,
	1709000000,
};
static unsigned long hi6250_stub_clk_freqs_cluster1[] = {
	1402000000,
	1805000000,
	2016000000,
	2112000000,
	2362000000,
};
static unsigned long hi6250_stub_clk_freqs_ddr[] = {
	120000000,
	240000000,
	360000000,
	533000000,
	800000000,
	933000000,
};
static unsigned long hi6250_stub_clk_freqs_gpu[] = {
	120000000,
	240000000,
	360000000,
	480000000,
	680000000,
	800000000,
	900000000,
};

static struct hi6250_stub_clk hi6250_stub_clks[HI6250_CLK_STUB_NUM] = {
	DEFINE_CLK_STUB(HI6250_CLK_STUB_CLUSTER0, hi6250_stub_clk_freqs_cluster0, 0x0001030A, "cpu-cluster.0"),
	DEFINE_CLK_STUB(HI6250_CLK_STUB_CLUSTER1, hi6250_stub_clk_freqs_cluster1, 0x0002030A, "cpu-cluster.1"),
	DEFINE_CLK_STUB(HI6250_CLK_STUB_DDR, hi6250_stub_clk_freqs_ddr, 0x00040309, "clk_ddrc"),
	DEFINE_CLK_STUB(HI6250_CLK_STUB_GPU, hi6250_stub_clk_freqs_gpu, 0x0003030A, "clk_g3d"),
};

static struct clk_hw *hi6250_stub_clk_hw_get(
  struct of_phandle_args *clkspec,
	void *data
) {
	unsigned int idx = clkspec->args[0];

	if (idx >= HI6250_CLK_STUB_NUM) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return &hi6250_stub_clks[idx].hw;
}

static int hi6250_stub_clk_init_lpm3(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct of_phandle_args spec;
	int ret;

	mutex_init(&lpm3.lock);
	lpm3.dev = dev;

	ret = of_parse_phandle_with_args(np, "mboxes", "#mbox-cells", 0, &spec);
	if (ret) {
		dev_err(dev, "no mboxes property: %d\n", ret);
		return ret;
	}
	if (spec.args_count < 3) {
		dev_err(dev, "mboxes #cells=%u, expected >=3\n", spec.args_count);
		of_node_put(spec.np);
		return -EINVAL;
	}
	lpm3.chan    = spec.args[0];
	lpm3.dst_irq = spec.args[1];
	lpm3.ack_irq = spec.args[2];

	lpm3.base = of_iomap(spec.np, 0);
	of_node_put(spec.np);
	if (!lpm3.base) {
		dev_err(dev, "failed to iomap mailbox\n");
		return -ENOMEM;
	}

	dev_info(dev, "lpm3 mailbox @ %p chan=%u dst_irq=%u ack_irq=%u\n",
		 lpm3.base, lpm3.chan, lpm3.dst_irq, lpm3.ack_irq);
	return 0;
}

static int hi6250_stub_clk_probe(
  struct platform_device *pdev
) {
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	unsigned int i;
	int ret;

	ret = hi6250_stub_clk_init_lpm3(dev);
	if (ret)
		return ret;

	freq_reg = syscon_regmap_lookup_by_phandle(np,
				"hisilicon,hi6250-sys-ctrl");
	if (IS_ERR(freq_reg)) {
		dev_err(dev, "failed to get sysctrl regmap\n");
		return PTR_ERR(freq_reg);
	}

	freq_reg += HI6250_STUB_CLOCK_BASE;

	for (i = 0; i < HI6250_CLK_STUB_NUM; i++) {
		ret = devm_clk_hw_register(&pdev->dev, &hi6250_stub_clks[i].hw);
		if (ret)
			return ret;
	}

	return devm_of_clk_add_hw_provider(&pdev->dev, hi6250_stub_clk_hw_get,
					   hi6250_stub_clks);
}

static const struct of_device_id hi6250_stub_clk_of_match[] = {
	{ .compatible = "hisilicon,hi6250-stub-clk", },
	{}
};

static struct platform_driver hi6250_stub_clk_driver = {
	.probe	= hi6250_stub_clk_probe,
	.driver = {
		.name = "hi6250-stub-clk",
		.of_match_table = hi6250_stub_clk_of_match,
	},
};

static int __init hi6250_stub_clk_init(void)
{
	return platform_driver_register(&hi6250_stub_clk_driver);
}
subsys_initcall(hi6250_stub_clk_init);
