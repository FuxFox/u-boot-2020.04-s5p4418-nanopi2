/*
 * (C) Copyright 2016 Nexell
 * Youngbok, Park <park@nexell.co.kr>
 *
 * SPDX-License-Identifier:      GPL-2.0+
 */

#include <common.h>
// #include <malloc.h>
#include <dm.h>
#include <dwmmc.h>
#include <asm/arch/nexell.h>
#include <asm/arch/clk.h>
#include <asm/arch/reset.h>
#include <asm/arch/nx_gpio.h>
#include <asm/arch/tieoff.h>
#include <linux/errno.h>
#include <fdtdec.h>
#include <linux/libfdt.h>

DECLARE_GLOBAL_DATA_PTR;

struct nexell_mmc_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

#define DWMCI_CLKSEL			0x09C
#define DWMCI_SHIFT_0			0x0
#define DWMCI_SHIFT_1			0x1
#define DWMCI_SHIFT_2			0x2
#define DWMCI_SHIFT_3			0x3
#define DWMCI_SET_SAMPLE_CLK(x)	(x)
#define DWMCI_SET_DRV_CLK(x)	((x) << 16)
#define DWMCI_SET_DIV_RATIO(x)	((x) << 24)
#define DWMCI_CLKCTRL			0x114
#define NX_MMC_CLK_DELAY(x, y, a, b)	(((x & 0xFF) << 0) |\
					((y & 0x03) << 16) |\
					((a & 0xFF) << 8)  |\
					((b & 0x03) << 24))

struct nexell_dwmmc_priv {
	struct dwmci_host	host;
	int dev;
	int frequency;
	int d_delay;
	int d_shift;
	int s_delay;
	int s_shift;
	char name[50];
	struct clk *clk;
};

static void set_pin_stat(int index, int bit, int value)
{
	nx_gpio_set_pad_function(index, bit, value);
}

static void nexell_dwmmc_set_pin(struct dwmci_host *host)
{
	switch (host->dev_index) {
	case 0:
		set_pin_stat(0, 29, 1);
		set_pin_stat(0, 31, 1);
		set_pin_stat(1, 1, 1);
		set_pin_stat(1, 3, 1);
		set_pin_stat(1, 5, 1);
		set_pin_stat(1, 7, 1);
		break;
	case 1:
		set_pin_stat(3, 22, 1);
		set_pin_stat(3, 23, 1);
		set_pin_stat(3, 24, 1);
		set_pin_stat(3, 25, 1);
		set_pin_stat(3, 26, 1);
		set_pin_stat(3, 27, 1);
		break;
	case 2:
		set_pin_stat(2, 18, 2);
		set_pin_stat(2, 19, 2);
		set_pin_stat(2, 20, 2);
		set_pin_stat(2, 21, 2);
		set_pin_stat(2, 22, 2);
		set_pin_stat(2, 23, 2);
		if (host->buswidth == 8) {
			set_pin_stat(4, 21, 2);
			set_pin_stat(4, 22, 2);
			set_pin_stat(4, 23, 2);
			set_pin_stat(4, 24, 2);
		}
		break;
	}
}

static unsigned int nexell_dwmmc_get_clk(struct dwmci_host *host, uint freq)
{
	struct nexell_dwmmc_priv *priv = host->priv;
	struct clk *clk = priv->clk;
	int index = host->dev_index;
	char name[50] = { 0, };

	if (!clk) {
		sprintf(name, "%s.%d", DEV_NAME_SDHC, index);
		clk = clk_get((const char *)name);
		if (!clk)
			return 0;
		priv->clk = clk;
	}

	return clk_get_rate(clk) / 2;
}

static unsigned long nexell_dwmmc_set_clk(struct dwmci_host *host,
				       unsigned int rate)
{
	struct nexell_dwmmc_priv *priv = host->priv;
	struct clk *clk = priv->clk;
	int index = host->dev_index;
	char name[50] = { 0, };

	if (!clk) {
		sprintf(name, "%s.%d", DEV_NAME_SDHC, index);
		clk = clk_get((const char *)name);
		if (!clk)
			return 0;
		priv->clk = clk;
	}

	clk_disable(clk);
	rate = clk_set_rate(clk, rate);
	clk_enable(clk);

	return rate;
}

static void nexell_dwmmc_clksel(struct dwmci_host *host)
{
	u32 val;

#ifdef CONFIG_BOOST_MMC
	val = DWMCI_SET_SAMPLE_CLK(DWMCI_SHIFT_0) |
	    DWMCI_SET_DRV_CLK(DWMCI_SHIFT_0) | DWMCI_SET_DIV_RATIO(1);
#else
	val = DWMCI_SET_SAMPLE_CLK(DWMCI_SHIFT_0) |
	    DWMCI_SET_DRV_CLK(DWMCI_SHIFT_0) | DWMCI_SET_DIV_RATIO(3);
#endif

	dwmci_writel(host, DWMCI_CLKSEL, val);
}

static void nexell_dwmmc_reset(int ch)
{
	int rst_id = RESET_ID_SDMMC0 + ch;

	nx_rstcon_setrst(rst_id, 0);
	nx_rstcon_setrst(rst_id, 1);
}

static void nexell_dwmmc_clk_delay(struct dwmci_host *host)
{
	unsigned int delay;
	struct nexell_dwmmc_priv *priv = host->priv;

	delay = NX_MMC_CLK_DELAY(priv->d_delay,
				 priv->d_shift, priv->s_delay, priv->s_shift);

	writel(delay, (host->ioaddr + DWMCI_CLKCTRL));
}

// static int nexell_dwmmc_of_list(const void *blob, int *node_list, int lists)
// {
// 	return fdtdec_find_aliases_for_id(blob, "mmc",
// 					  COMPAT_NEXELL_DWMMC, node_list,
// 					  lists);
// }

static int nexell_dwmmc_ofdata_to_platdata(struct udevice *dev)
{
	struct nexell_dwmmc_priv *priv = dev_get_priv(dev);
	struct dwmci_host *host = &priv->host;
	int fifo_size = 0x20;

	// unsigned long base;

	// index = fdtdec_get_int(blob, node, "index", 0);
	// bus_w = dev_read_u32_default(dev, "nexell,bus-width", 4);
	// if (bus_w <= 0) {
	// 	printf("failed to bus width %d for dwmmc.%d\n", bus_w, index);
	// 	return -EINVAL;
	// }

	// base = fdtdec_get_uint(blob, node, "reg", 0);
	// host->ioaddr = (void *)base;
	// if (!base) {
	// 	printf("failed to invalud base for dwmmc.%d\n", index);
	// 	return -EINVAL;
	// }

	priv->d_delay = dev_read_u32_default(dev, "nexell,drive_dly", 0);
	priv->d_shift = dev_read_u32_default(dev, "nexell,drive_shift", 0);
	priv->s_delay = dev_read_u32_default(dev, "nexell,sample_dly", 0);
	priv->s_shift = dev_read_u32_default(dev, "nexell,sample_shift", 0);
	priv->frequency = dev_read_u32_default(dev, "frequency", 0);

	host->name = dev->name;
	host->ioaddr = dev_read_addr_ptr(dev);
	host->dev_index = dev_read_u32_default(dev, "index", 0);
	host->dev_id = host->dev_index;
	host->buswidth = dev_read_u32_default(dev, "nexell,bus-width", 4);
	host->clksel = nexell_dwmmc_clksel;
	host->get_mmc_clk = nexell_dwmmc_get_clk;
	host->priv = priv;
	host->fifoth_val =
	    MSIZE(0x2) | RX_WMARK(fifo_size / 2 - 1) | TX_WMARK(fifo_size / 2);

	if (dev_read_u32_default(dev, "nexell,ddr", 0))
		host->caps |= MMC_MODE_DDR_52MHz;

	return 0;
}


static int nexell_dwmmc_probe(struct udevice *dev)
{
	struct nexell_mmc_plat *plat = dev_get_platdata(dev);
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct nexell_dwmmc_priv *priv = dev_get_priv(dev);
	struct dwmci_host *host = &priv->host;

	debug("nexell_dw_mmc: mmc.%d probe\n", host->dev_index);

	nexell_dwmmc_set_pin(host);
	nexell_dwmmc_set_clk(host, priv->frequency * 4);
	nexell_dwmmc_reset(host->dev_index);
	nexell_dwmmc_clk_delay(host);

	dwmci_setup_cfg(&plat->cfg, host, priv->frequency, 400000);
	host->mmc = &plat->mmc;
	host->mmc->priv = &priv->host;
	host->mmc->dev = dev;
	upriv->mmc = host->mmc;

	return dwmci_probe(dev);
}

static int nexell_dwmmc_bind(struct udevice *dev)
{
	struct nexell_mmc_plat *plat = dev_get_platdata(dev);

	return dwmci_bind(dev, &plat->mmc, &plat->cfg);
}


static const struct udevice_id nexell_dwmmc_ids[] = {
	{ .compatible = "nexell,nexell-dwmmc" },
	{ }
};

U_BOOT_DRIVER(nexell_dwmmc_drv) = {
	.name		= "nexell_dwmmc",
	.id			= UCLASS_MMC,
	.of_match	= nexell_dwmmc_ids,
	.ofdata_to_platdata = nexell_dwmmc_ofdata_to_platdata,
	.ops		= &dm_dwmci_ops,
	.bind		= nexell_dwmmc_bind,
	.probe		= nexell_dwmmc_probe,
	.priv_auto_alloc_size = sizeof(struct nexell_dwmmc_priv),
	.platdata_auto_alloc_size = sizeof(struct nexell_mmc_plat),
};
