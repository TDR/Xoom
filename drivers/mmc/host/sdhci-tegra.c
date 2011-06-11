/*
 * drivers/mmc/host/sdhci-tegra.c
 *
 * Copyright (C) 2009 Palm, Inc.
 * Author: Yvonne Yip <y@palm.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/mmc/card.h>

#include <mach/sdhci.h>

#include "sdhci.h"

#define DRIVER_NAME    "sdhci-tegra"

#define SDHCI_VENDOR_CLOCK_CNTRL       0x100

struct tegra_sdhci_host {
	struct sdhci_host *sdhci;
	struct clk *clk;
	struct tegra_sdhci_platform_data *plat;
	int clk_enabled;
};

static irqreturn_t carddetect_irq(int irq, void *data)
{
	struct sdhci_host *sdhost = (struct sdhci_host *)data;

	sdhci_card_detect_callback(sdhost);
	return IRQ_HANDLED;
};

static void sdhci_status_notify_cb(int card_present, void *dev_id)
{
	struct sdhci_host *host = (struct sdhci_host *)dev_id;
	struct tegra_sdhci_host *tegra_host = sdhci_priv(host);
	unsigned int status, oldstat;

	pr_debug("%s: card_present %d\n", mmc_hostname(host->mmc),
		card_present);

        if (!tegra_host->plat->mmc_data.status) {
		mmc_detect_change(host->mmc, 0);
		return;
	}

	status = tegra_host->plat->mmc_data.status(mmc_dev(host->mmc));

	oldstat = host->card_present;
	host->card_present = status;
	if (status ^ oldstat) {
		pr_debug("%s: Slot status change detected (%d -> %d)\n",
			mmc_hostname(host->mmc), oldstat, status);
		if (status && !tegra_host->plat->mmc_data.built_in)
			mmc_detect_change(host->mmc, (5 * HZ) / 2);
		else
			mmc_detect_change(host->mmc, 0);
	}
}

static int tegra_sdhci_enable_dma(struct sdhci_host *host)
{
	return 0;
}

static void tegra_sdhci_enable_clock(struct tegra_sdhci_host *host, int enable)
{
	if (enable && !host->clk_enabled) {
		clk_enable(host->clk);
		sdhci_writeb(host->sdhci, 1, SDHCI_VENDOR_CLOCK_CNTRL);
		host->clk_enabled = 1;
	} else if (!enable && host->clk_enabled) {
		sdhci_writeb(host->sdhci, 0, SDHCI_VENDOR_CLOCK_CNTRL);
		clk_disable(host->clk);
		host->clk_enabled = 0;
	}
}

static void tegra_sdhci_set_clock(struct sdhci_host *sdhci, unsigned int clock)
{
	struct tegra_sdhci_host *host = sdhci_priv(sdhci);
	pr_debug("tegra sdhci clock %s %u enabled=%d\n",
		mmc_hostname(sdhci->mmc), clock, host->clk_enabled);

	tegra_sdhci_enable_clock(host, clock);
}

static struct sdhci_ops tegra_sdhci_ops = {
	.enable_dma = tegra_sdhci_enable_dma,
	.set_clock = tegra_sdhci_set_clock,
};

static int __devinit tegra_sdhci_probe(struct platform_device *pdev)
{
	int rc;
	struct tegra_sdhci_platform_data *plat;
	struct sdhci_host *sdhci;
	struct tegra_sdhci_host *host;
	struct resource *res;
	int irq;
	void __iomem *ioaddr;

	plat = pdev->dev.platform_data;
	if (plat == NULL)
		return -ENXIO;

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL)
		return -ENODEV;

	irq = res->start;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL)
		return -ENODEV;

	ioaddr = ioremap(res->start, res->end - res->start);

	sdhci = sdhci_alloc_host(&pdev->dev, sizeof(struct tegra_sdhci_host));
	if (IS_ERR(sdhci)) {
		rc = PTR_ERR(sdhci);
		goto err_unmap;
	}

	host = sdhci_priv(sdhci);
	host->sdhci = sdhci;
	host->plat = plat;

#ifdef CONFIG_MMC_EMBEDDED_SDIO
	if (plat->mmc_data.embedded_sdio)
		mmc_set_embedded_sdio_data(sdhci->mmc,
				&plat->mmc_data.embedded_sdio->cis,
				&plat->mmc_data.embedded_sdio->cccr,
				plat->mmc_data.embedded_sdio->funcs,
				plat->mmc_data.embedded_sdio->num_funcs);
#endif

	host->clk = clk_get(&pdev->dev, plat->clk_id);
	if (IS_ERR(host->clk)) {
		rc = PTR_ERR(host->clk);
		goto err_free_host;
	}

	rc = clk_enable(host->clk);
	if (rc != 0)
		goto err_clkput;

	host->clk_enabled = 1;
	sdhci->hw_name = "tegra";
	sdhci->ops = &tegra_sdhci_ops;
	sdhci->irq = irq;
	sdhci->ioaddr = ioaddr;
	sdhci->version = SDHCI_SPEC_200;
	sdhci->quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
			SDHCI_QUIRK_SINGLE_POWER_WRITE |
			SDHCI_QUIRK_ENABLE_INTERRUPT_AT_BLOCK_GAP |
			SDHCI_QUIRK_BROKEN_WRITE_PROTECT |
			SDHCI_QUIRK_BROKEN_CTRL_HISPD |
			SDHCI_QUIRK_NO_HISPD_BIT |
			SDHCI_QUIRK_8_BIT_DATA |
			SDHCI_QUIRK_NO_VERSION_REG |
			SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
			SDHCI_QUIRK_NO_SDIO_IRQ;

	if (plat->force_hs != 0)
		sdhci->quirks |= SDHCI_QUIRK_FORCE_HIGH_SPEED_MODE;

	sdhci->mmc->pm_caps = MMC_PM_KEEP_POWER | MMC_PM_IGNORE_PM_NOTIFY;
	if (plat->mmc_data.built_in)
		sdhci->mmc->pm_flags = MMC_PM_KEEP_POWER | MMC_PM_IGNORE_PM_NOTIFY;

	if (plat->rt_disable != 0)
		sdhci->quirks |= SDHCI_QUIRK_RUNTIME_DISABLE;

	rc = sdhci_add_host(sdhci);
	if (rc)
		goto err_clk_disable;

	platform_set_drvdata(pdev, host);

	if (plat->cd_gpio != -1) {
		rc = request_irq(gpio_to_irq(plat->cd_gpio), carddetect_irq,
			IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			mmc_hostname(sdhci->mmc), sdhci);

		if (rc)
			goto err_remove_host;
	} else if (plat->mmc_data.register_status_notify) {
		plat->mmc_data.register_status_notify(sdhci_status_notify_cb, sdhci);
	}

	if (plat->mmc_data.status) {
		sdhci->card_present = host->plat->mmc_data.status(mmc_dev(sdhci->mmc));
	}

	if (plat->board_probe)
		plat->board_probe(pdev->id, sdhci->mmc);

	printk(KERN_INFO "sdhci%d: initialized irq %d ioaddr %p\n", pdev->id,
			sdhci->irq, sdhci->ioaddr);

	return 0;

err_remove_host:
	sdhci_remove_host(sdhci, 1);
err_clk_disable:
	clk_disable(host->clk);
err_clkput:
	clk_put(host->clk);
err_free_host:
	if (sdhci)
		sdhci_free_host(sdhci);
err_unmap:
	iounmap(sdhci->ioaddr);

	return rc;
}

static int tegra_sdhci_remove(struct platform_device *pdev)
{
	struct tegra_sdhci_host *host = platform_get_drvdata(pdev);
	if (host) {
		struct tegra_sdhci_platform_data *plat;
		plat = pdev->dev.platform_data;
		if (plat && plat->board_probe)
			plat->board_probe(pdev->id, host->sdhci->mmc);

		sdhci_remove_host(host->sdhci, 0);
		sdhci_free_host(host->sdhci);
	}
	return 0;
}

#ifdef CONFIG_PM
static int tegra_sdhci_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct tegra_sdhci_host *host = platform_get_drvdata(pdev);
	struct mmc_host *mmc = host->sdhci->mmc;
	int ret;

	if (host->plat->mmc_data.built_in)
	        mmc->pm_flags |= MMC_PM_KEEP_POWER;

	ret = sdhci_suspend_host(host->sdhci, state);
	if (ret)
		pr_err("%s: failed, error = %d\n", __func__, ret);

	tegra_sdhci_enable_clock(host, 0);
	return ret;
}

static int tegra_sdhci_resume(struct platform_device *pdev)
{
	struct tegra_sdhci_host *host = platform_get_drvdata(pdev);
	int ret;

	tegra_sdhci_enable_clock(host, 1);
	ret = sdhci_resume_host(host->sdhci);
	if (ret)
		pr_err("%s: failed, error = %d\n", __func__, ret);

	return ret;
}
#else
#define tegra_sdhci_suspend    NULL
#define tegra_sdhci_resume     NULL
#endif

static struct platform_driver tegra_sdhci_driver = {
	.probe = tegra_sdhci_probe,
	.remove = tegra_sdhci_remove,
	.suspend = tegra_sdhci_suspend,
	.resume = tegra_sdhci_resume,
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init tegra_sdhci_init(void)
{
	return platform_driver_register(&tegra_sdhci_driver);
}

static void __exit tegra_sdhci_exit(void)
{
	platform_driver_unregister(&tegra_sdhci_driver);
}

module_init(tegra_sdhci_init);
module_exit(tegra_sdhci_exit);

MODULE_DESCRIPTION("Tegra SDHCI controller driver");
MODULE_LICENSE("GPL");
