/*
 * Copyright (C) 2011 Samsung Electronics
 *
 * Donghwa Lee <dh09.lee@samsung.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/* This codes are copied from arch/arm/cpu/armv7/s5p-common/pwm.c */

#include <common.h>
#include <errno.h>
#include <pwm.h>
#include <asm/io.h>
#include <asm/arch/clk.h>
#include "pwm-nexell.h"

#define NS_IN_SEC 1000000000UL

struct pwm_device {
	int ch;
	int grp;
	int bit;
	int pwm_fn;
};

static const struct pwm_device pwm_dev[] = {
	[0] = { .ch = 0, .grp = 3, .bit = 1,  .pwm_fn = 1 },
	[1] = { .ch = 1, .grp = 2, .bit = 13, .pwm_fn = 2 },
	[2] = { .ch = 2, .grp = 2, .bit = 14, .pwm_fn =	2 },
	[3] = { .ch = 3, .grp = 3, .bit = 0,  .pwm_fn = 2 },
};

#if defined(CONFIG_ARCH_NEXELL)
#include <asm/arch/nexell.h>
#include <asm/arch/reset.h>
#include <asm/arch/nx_gpio.h>
#include <asm/arch/tieoff.h>
#endif

static unsigned long nexell_pwm_calc_tin(int pwm_id, unsigned long freq)
{
	unsigned long tin_parent_rate;
	unsigned int div;
#if defined(CONFIG_ARCH_NEXELL)
	unsigned int pre_div;
	const struct s5p_timer *pwm =
		(struct s5p_timer *)PHY_BASEADDR_PWM;
	unsigned int val;
#endif

#if defined(CONFIG_ARCH_NEXELL)
	struct clk *clk = clk_get(CORECLK_NAME_PCLK);
	tin_parent_rate = clk_get_rate(clk);
#else
	tin_parent_rate = get_pwm_clk();
#endif

#if defined(CONFIG_ARCH_NEXELL)
	writel(0, &pwm->tcfg0);
	val = readl(&pwm->tcfg0);

	if (pwm_id < 2)
		div = ((val >> 0) & 0xff) + 1;
	else
		div = ((val >> 8) & 0xff) + 1;

	writel(0, &pwm->tcfg1);
	val = readl(&pwm->tcfg1);
	val = (val >> MUX_DIV_SHIFT(pwm_id)) & 0xF;
	pre_div = (1UL << val);

	freq = tin_parent_rate / div / pre_div;

	return freq;
#else
	for (div = 2; div <= 16; div *= 2) {
		if ((tin_parent_rate / (div << 16)) < freq)
			return tin_parent_rate / div;
	}

	return tin_parent_rate / 16;
#endif
}


static int nexell_pwm_set_config(struct udevice *dev, uint channel,
				uint period_ns, uint duty_ns)
{
	const struct s5p_timer *pwm =
#if defined(CONFIG_ARCH_NEXELL)
		(struct s5p_timer *)PHY_BASEADDR_PWM;
#else
		(struct s5p_timer *)samsung_get_base_timer();
#endif
	unsigned int offset;
	unsigned long tin_rate;
	unsigned long tin_ns;
	unsigned long frequency;
	unsigned long tcon;
	unsigned long tcnt;
	unsigned long tcmp;

	/*
	 * We currently avoid using 64bit arithmetic by using the
	 * fact that anything faster than 1GHz is easily representable
	 * by 32bits.
	 */
	if (period_ns > NS_IN_SEC || duty_ns > NS_IN_SEC || period_ns == 0)
		return -ERANGE;

	if (duty_ns > period_ns)
		return -EINVAL;

	frequency = NS_IN_SEC / period_ns;

	/* Check to see if we are changing the clock rate of the PWM */
	tin_rate = nexell_pwm_calc_tin(channel, frequency);

	tin_ns = NS_IN_SEC / tin_rate;
#if defined(CONFIG_ARCH_NEXELL)
	/* The counter starts at zero. */
	tcnt = (period_ns / tin_ns) - 1;
#else
	tcnt = period_ns / tin_ns;
#endif

	/* Note, counters count down */
	tcmp = duty_ns / tin_ns;
	tcmp = tcnt - tcmp;

	/* Update the PWM register block. */
	offset = channel * 3;
	if (channel < 4) {
		writel(tcnt, &pwm->tcntb0 + offset);
		writel(tcmp, &pwm->tcmpb0 + offset);
	}

	tcon = readl(&pwm->tcon);
	tcon |= TCON_UPDATE(channel);
	if (channel < 4)
		tcon |= TCON_AUTO_RELOAD(channel);
	else
		tcon |= TCON4_AUTO_RELOAD;
	writel(tcon, &pwm->tcon);

	tcon &= ~TCON_UPDATE(channel);
	writel(tcon, &pwm->tcon);

	return 0;
}

static int nexell_pwm_set_enable(struct udevice *dev, uint channel, bool enable)
{
	const struct s5p_timer *pwm =
#if defined(CONFIG_ARCH_NEXELL)
			(struct s5p_timer *)PHY_BASEADDR_PWM;
#else
			(struct s5p_timer *)samsung_get_base_timer();
#endif
	unsigned long tcon;

	tcon = readl(&pwm->tcon);
	if (enable)
		tcon |= TCON_START(pwm_id);
	else
		tcon &= ~TCON_START(pwm_id);	

	writel(tcon, &pwm->tcon);

	return 0;
}

static int nexell_pwm_ofdata_to_platdata(struct udevice *dev)
{
	return 0;
}

static const struct pwm_ops nexell_pwm_ops = {
	.set_config	= nexell_pwm_set_config,
	.set_enable	= nexell_pwm_set_enable,
};

static const struct udevice_id nexell_pwm_ids[] = {
	{ .compatible = "nexell,s5p4418-pwm" },
	{ }
};

U_BOOT_DRIVER(nexell_pwm) = {
	.name	= "nexell_pwm",
	.id	= UCLASS_PWM,
	.of_match = nexell_pwm_ids,
	.ops	= &nexell_pwm_ops,
	.ofdata_to_platdata	= nexell_pwm_ofdata_to_platdata,
};
