// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Western Digital Corporation or its affiliates.
 * Copyright (C) 2022 Ventana Micro Systems Inc.
 */

#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/irqchip/riscv-aplic.h>
#include <linux/irqchip/riscv-imsic.h>
#include <asm/acpi.h>

#include "irq-riscv-aplic-main.h"

static int nr_aplics = 0;
static struct aplic_priv *acpi_cached_priv[MAX_APLICS];

void aplic_irq_unmask(struct irq_data *d)
{
	struct aplic_priv *priv = irq_data_get_irq_chip_data(d);

	writel(d->hwirq, priv->regs + APLIC_SETIENUM);
}

void aplic_irq_mask(struct irq_data *d)
{
	struct aplic_priv *priv = irq_data_get_irq_chip_data(d);

	writel(d->hwirq, priv->regs + APLIC_CLRIENUM);
}

int aplic_irq_set_type(struct irq_data *d, unsigned int type)
{
	u32 val = 0;
	void __iomem *sourcecfg;
	struct aplic_priv *priv = irq_data_get_irq_chip_data(d);

	switch (type) {
	case IRQ_TYPE_NONE:
		val = APLIC_SOURCECFG_SM_INACTIVE;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		val = APLIC_SOURCECFG_SM_LEVEL_LOW;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		val = APLIC_SOURCECFG_SM_LEVEL_HIGH;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		val = APLIC_SOURCECFG_SM_EDGE_FALL;
		break;
	case IRQ_TYPE_EDGE_RISING:
		val = APLIC_SOURCECFG_SM_EDGE_RISE;
		break;
	default:
		return -EINVAL;
	}

	sourcecfg = priv->regs + APLIC_SOURCECFG_BASE;
	sourcecfg += (d->hwirq - 1) * sizeof(u32);
	writel(val, sourcecfg);

	return 0;
}

int aplic_irqdomain_translate(struct irq_fwspec *fwspec, u32 gsi_base,
			      unsigned long *hwirq, unsigned int *type)
{
	if (WARN_ON(fwspec->param_count < 2))
		return -EINVAL;
	if (WARN_ON(!fwspec->param[0]))
		return -EINVAL;

	/* For DT, gsi_base is always zero. */
	*hwirq = fwspec->param[0] - gsi_base;
	*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;

	WARN_ON(*type == IRQ_TYPE_NONE);

	return 0;
}

void aplic_init_hw_global(struct aplic_priv *priv, bool msi_mode)
{
	u32 val;
#ifdef CONFIG_RISCV_M_MODE
	u32 valH;

	if (msi_mode) {
		val = priv->msicfg.base_ppn;
		valH = ((u64)priv->msicfg.base_ppn >> 32) &
			APLIC_xMSICFGADDRH_BAPPN_MASK;
		valH |= (priv->msicfg.lhxw & APLIC_xMSICFGADDRH_LHXW_MASK)
			<< APLIC_xMSICFGADDRH_LHXW_SHIFT;
		valH |= (priv->msicfg.hhxw & APLIC_xMSICFGADDRH_HHXW_MASK)
			<< APLIC_xMSICFGADDRH_HHXW_SHIFT;
		valH |= (priv->msicfg.lhxs & APLIC_xMSICFGADDRH_LHXS_MASK)
			<< APLIC_xMSICFGADDRH_LHXS_SHIFT;
		valH |= (priv->msicfg.hhxs & APLIC_xMSICFGADDRH_HHXS_MASK)
			<< APLIC_xMSICFGADDRH_HHXS_SHIFT;
		writel(val, priv->regs + APLIC_xMSICFGADDR);
		writel(valH, priv->regs + APLIC_xMSICFGADDRH);
	}
#endif

	/* Setup APLIC domaincfg register */
	val = readl(priv->regs + APLIC_DOMAINCFG);
	val |= APLIC_DOMAINCFG_IE;
	if (msi_mode)
		val |= APLIC_DOMAINCFG_DM;
	writel(val, priv->regs + APLIC_DOMAINCFG);
	if (readl(priv->regs + APLIC_DOMAINCFG) != val)
		dev_warn(priv->dev, "unable to write 0x%x in domaincfg\n",
			 val);
}

static void aplic_init_hw_irqs(struct aplic_priv *priv)
{
	int i;

	/* Disable all interrupts */
	for (i = 0; i <= priv->nr_irqs; i += 32)
		writel(-1U, priv->regs + APLIC_CLRIE_BASE +
			    (i / 32) * sizeof(u32));

	/* Set interrupt type and default priority for all interrupts */
	for (i = 1; i <= priv->nr_irqs; i++) {
		writel(0, priv->regs + APLIC_SOURCECFG_BASE +
			  (i - 1) * sizeof(u32));
		writel(APLIC_DEFAULT_PRIORITY,
		       priv->regs + APLIC_TARGET_BASE +
		       (i - 1) * sizeof(u32));
	}

	/* Clear APLIC domaincfg */
	writel(0, priv->regs + APLIC_DOMAINCFG);
}

int aplic_setup_priv(struct aplic_priv *priv, struct device *dev,
		     void __iomem *regs)
{
	struct of_phandle_args parent;
	struct acpi_madt_aplic *aplic;
	int rc;

	/* Save device pointer and register base */
	priv->dev = dev;
	priv->regs = regs;

	if (is_of_node(dev->fwnode)) {
		/* Find out number of interrupt sources */
		rc = of_property_read_u32(to_of_node(dev->fwnode),
						     "riscv,num-sources",
						     &priv->nr_irqs);
		if (rc) {
			dev_err(dev, "failed to get number of interrupt sources\n");
			return rc;
		}

		/*
		 * Find out number of IDCs based on parent interrupts
		 *
		 * If "msi-parent" property is present then we ignore the
		 * APLIC IDCs which forces the APLIC driver to use MSI mode.
		 */
		if (!of_property_present(to_of_node(dev->fwnode), "msi-parent")) {
			while (!of_irq_parse_one(to_of_node(dev->fwnode),
						 priv->nr_idcs, &parent))
				priv->nr_idcs++;
		}
	} else {
		aplic = *(struct acpi_madt_aplic **)dev_get_platdata(dev);
		if (!aplic) {
			dev_err(dev, "APLIC platform data is NULL!\n");
			return -1;
		}
		priv->gsi_base = aplic->gsi_base;
		priv->nr_irqs = aplic->num_sources;
		priv->nr_idcs = aplic->num_idcs;
		priv->id = aplic->id;
	}

	/* Setup initial state APLIC interrupts */
	aplic_init_hw_irqs(priv);

	return 0;
}

#ifdef CONFIG_ACPI
struct fwnode_handle *aplic_get_gsi_domain_id(u32 gsi)
{
	int i;

	if (nr_aplics) {
		/* Find the APLIC that manages this GSI. */
		for (i = 0; i < nr_aplics; i++) {
			if (gsi >= acpi_cached_priv[i]->gsi_base &&
			    gsi < (acpi_cached_priv[i]->gsi_base + acpi_cached_priv[i]->nr_irqs))
				return acpi_cached_priv[i]->dev->fwnode;
		}
	}

	return NULL;
}
#endif

static int aplic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aplic_priv *priv;
	bool msi_mode = false;
	struct resource *res;
	void __iomem *regs;
	int rc;

	/* Map the MMIO registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "failed to get MMIO resource\n");
		return -EINVAL;
	}
	regs = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!regs) {
		dev_err(dev, "failed map MMIO registers\n");
		return -ENOMEM;
	}

	/*
	 * If msi-parent property is present then setup APLIC MSI
	 * mode otherwise setup APLIC direct mode.
	 */
	if (is_of_node(dev->fwnode))
		msi_mode = of_property_present(to_of_node(dev->fwnode),
						"msi-parent");
	else
		msi_mode = imsic_acpi_get_fwnode(NULL) ? 1 : 0;

	if (msi_mode)
		rc = aplic_msi_setup(dev, regs);
	else
		rc = aplic_direct_setup(dev, regs);
	if (rc) {
		dev_err(dev, "failed setup APLIC in %s mode\n",
			msi_mode ? "MSI" : "direct");
		return rc;
	}
	priv = dev_get_drvdata(dev);
	if (priv)
		acpi_cached_priv[nr_aplics++] = dev_get_drvdata(dev);

	return 0;
}

static const struct of_device_id aplic_match[] = {
	{ .compatible = "riscv,aplic" },
	{}
};

static struct platform_driver aplic_driver = {
	.driver = {
		.name		= "riscv-aplic",
		.of_match_table	= aplic_match,
	},
	.probe = aplic_probe,
};
builtin_platform_driver(aplic_driver);
