// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe host controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>

#include "pcie-cadence.h"

void __iomem *cdns_pci_map_bus(struct pci_bus *bus, unsigned int devfn,
			       int where)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct cdns_pcie_rc *rc = pci_host_bridge_priv(bridge);
	struct cdns_pcie *pcie = &rc->pcie;
	unsigned int busn = bus->number;
	u32 addr0, desc0;

	if (busn == rc->bus_range->start) {
		/*
		 * Only the root port (devfn == 0) is connected to this bus.
		 * All other PCI devices are behind some bridge hence on another
		 * bus.
		 */
		if (devfn)
			return NULL;

		return pcie->reg_base + (where & 0xfff);
	}
	/* Check that the link is up */
	if (!(cdns_pcie_readl(pcie, CDNS_PCIE_LM_BASE) & 0x1))
		return NULL;
	/* Clear AXI link-down status */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_LINKDOWN, 0x0);

	/* Update Output registers for AXI region 0. */
	addr0 = CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(12) |
		CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) |
		CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS(busn);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(0), addr0);

	/* Configuration Type 0 or Type 1 access. */
	desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_HARDCODED_RID |
		CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(0);
	/*
	 * The bus number was already set once for all in desc1 by
	 * cdns_pcie_host_init_address_translation().
	 */
	if (busn == rc->bus_range->start + 1)
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0;
	else
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1;
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(0), desc0);

	return rc->cfg_base + (where & 0xfff);
}

static struct pci_ops cdns_pcie_host_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

static int cdns_pcie_host_wait_for_link(struct cdns_pcie *pcie)
{
	struct device *dev = pcie->dev;
	int retries;

	/* Check if the link is up or not */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {
		if (cdns_pcie_link_up(pcie)) {
			dev_info(dev, "Link up\n");
			return 0;
		}
		usleep_range(LINK_WAIT_USLEEP_MIN, LINK_WAIT_USLEEP_MAX);
	}

	return -ETIMEDOUT;
}

static int cdns_pcie_retrain(struct cdns_pcie *pcie)
{
	u32 lnk_cap_sls, pcie_cap_off = CDNS_PCIE_RP_CAP_OFFSET;
	u16 lnk_stat, lnk_ctl;
	int ret = 0;

	/*
	 * Set retrain bit if current speed is 2.5 GB/s,
	 * but the PCIe root port support is > 2.5 GB/s.
	 */

	lnk_cap_sls = cdns_pcie_readl(pcie, (CDNS_PCIE_RP_BASE + pcie_cap_off +
					     PCI_EXP_LNKCAP));
	if ((lnk_cap_sls & PCI_EXP_LNKCAP_SLS) <= PCI_EXP_LNKCAP_SLS_2_5GB)
		return ret;

	lnk_stat = cdns_pcie_rp_readw(pcie, pcie_cap_off + PCI_EXP_LNKSTA);
	if ((lnk_stat & PCI_EXP_LNKSTA_CLS) == PCI_EXP_LNKSTA_CLS_2_5GB) {
		lnk_ctl = cdns_pcie_rp_readw(pcie,
					     pcie_cap_off + PCI_EXP_LNKCTL);
		lnk_ctl |= PCI_EXP_LNKCTL_RL;
		cdns_pcie_rp_writew(pcie, pcie_cap_off + PCI_EXP_LNKCTL,
				    lnk_ctl);

		ret = cdns_pcie_host_wait_for_link(pcie);
	}
	return ret;
}

static int cdns_pcie_host_start_link(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	int ret;

	ret = cdns_pcie_host_wait_for_link(pcie);

	/*
	 * Retrain link for Gen2 training defect
	 * if quirk flag is set.
	 */
	if (!ret && rc->quirk_retrain_flag)
		ret = cdns_pcie_retrain(pcie);

	return ret;
}

static int cdns_pcie_host_init_root_port(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	u32 value, ctrl;
	u32 id;

	/*
	 * Set the root complex BAR configuration register:
	 * - disable both BAR0 and BAR1.
	 * - enable Prefetchable Memory Base and Limit registers in type 1
	 *   config space (64 bits).
	 * - enable IO Base and Limit registers in type 1 config
	 *   space (32 bits).
	 */
	ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED;
	value = CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_32BITS;
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_RC_BAR_CFG, value);

	/* Set root port configuration space */
	if (rc->vendor_id != 0xffff) {
		id = CDNS_PCIE_LM_ID_VENDOR(rc->vendor_id) |
			CDNS_PCIE_LM_ID_SUBSYS(rc->vendor_id);
		cdns_pcie_writel(pcie, CDNS_PCIE_LM_ID, id);
	}

	if (rc->device_id != 0xffff)
		cdns_pcie_rp_writew(pcie, PCI_DEVICE_ID, rc->device_id);

	cdns_pcie_rp_writeb(pcie, PCI_CLASS_REVISION, 0);
	cdns_pcie_rp_writeb(pcie, PCI_CLASS_PROG, 0);
	cdns_pcie_rp_writew(pcie, PCI_CLASS_DEVICE, PCI_CLASS_BRIDGE_PCI);

	return 0;
}

static int cdns_pcie_host_init_address_translation(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct resource *bus_range = rc->bus_range;
	struct resource *cfg_res = rc->cfg_res;
	struct device *dev = pcie->dev;
	struct device_node *np = dev->of_node;
	struct of_pci_range_parser parser;
	u64 cpu_addr = cfg_res->start;
	struct of_pci_range range;
	u32 addr0, addr1, desc1;
	int r, err;

	/*
	 * Reserve region 0 for PCI configure space accesses:
	 * OB_REGION_PCI_ADDR0 and OB_REGION_DESC0 are updated dynamically by
	 * cdns_pci_map_bus(), other region registers are set here once for all.
	 */
	addr1 = 0; /* Should be programmed to zero. */
	desc1 = CDNS_PCIE_AT_OB_REGION_DESC1_BUS(bus_range->start);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(0), addr1);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(0), desc1);

	if (pcie->ops->cpu_addr_fixup)
		cpu_addr = pcie->ops->cpu_addr_fixup(pcie, cpu_addr);

	addr0 = CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(12) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(0), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(0), addr1);

	err = of_pci_range_parser_init(&parser, np);
	if (err)
		return err;

	r = 1;
	for_each_of_pci_range(&parser, &range) {
		bool is_io;

		if (r >= rc->max_regions)
			break;

		if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_MEM)
			is_io = false;
		else if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_IO)
			is_io = true;
		else
			continue;

		cdns_pcie_set_outbound_region(pcie, 0, r, is_io,
					      range.cpu_addr,
					      range.pci_addr,
					      range.size);
		r++;
	}

	/*
	 * Set Root Port no BAR match Inbound Translation registers:
	 * needed for MSI and DMA.
	 * Root Port BAR0 and BAR1 are disabled, hence no need to set their
	 * inbound translation registers.
	 */
	addr0 = CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS(rc->no_bar_nbits);
	addr1 = 0;
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR0(RP_NO_BAR), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR1(RP_NO_BAR), addr1);

	return 0;
}

static int cdns_pcie_host_init(struct device *dev,
			       struct list_head *resources,
			       struct cdns_pcie_rc *rc)
{
	struct resource *bus_range = NULL;
	int err;

	/* Parse our PCI ranges and request their resources */
	err = pci_parse_request_of_pci_ranges(dev, resources, &bus_range);
	if (err)
		return err;

	rc->bus_range = bus_range;
	rc->pcie.bus = bus_range->start;

	err = cdns_pcie_host_init_root_port(rc);
	if (err)
		goto err_out;

	err = cdns_pcie_host_init_address_translation(rc);
	if (err)
		goto err_out;

	return 0;

 err_out:
	pci_free_resource_list(resources);
	return err;
}

int cdns_pcie_host_setup(struct cdns_pcie_rc *rc)
{
	struct device *dev = rc->pcie.dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *np = dev->of_node;
	struct pci_host_bridge *bridge;
	struct list_head resources;
	struct cdns_pcie *pcie;
	struct resource *res;
	int ret;

	bridge = pci_host_bridge_from_priv(rc);
	if (!bridge)
		return -ENOMEM;

	pcie = &rc->pcie;
	pcie->is_rc = true;

	rc->max_regions = 32;
	of_property_read_u32(np, "cdns,max-outbound-regions", &rc->max_regions);

	rc->no_bar_nbits = 32;
	of_property_read_u32(np, "cdns,no-bar-match-nbits", &rc->no_bar_nbits);

	rc->vendor_id = 0xffff;
	of_property_read_u16(np, "vendor-id", &rc->vendor_id);

	rc->device_id = 0xffff;
	of_property_read_u16(np, "device-id", &rc->device_id);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reg");
	pcie->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->reg_base)) {
		dev_err(dev, "missing \"reg\"\n");
		return PTR_ERR(pcie->reg_base);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	rc->cfg_base = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(rc->cfg_base)) {
		dev_err(dev, "missing \"cfg\"\n");
		return PTR_ERR(rc->cfg_base);
	}
	rc->cfg_res = res;

	ret = cdns_pcie_start_link(pcie);
	if (ret) {
		dev_err(dev, "Failed to start link\n");
		return ret;
	}

	ret = cdns_pcie_host_start_link(rc);
	if (ret)
		dev_dbg(dev, "PCIe link never came up\n");

	ret = cdns_pcie_host_init(dev, &resources, rc);
	if (ret)
		return ret;

	list_splice_init(&resources, &bridge->windows);
	bridge->dev.parent = dev;
	bridge->busnr = pcie->bus;
	if (!bridge->ops)
		bridge->ops = &cdns_pcie_host_ops;
	bridge->map_irq = of_irq_parse_and_map_pci;
	bridge->swizzle_irq = pci_common_swizzle;

	ret = pci_host_probe(bridge);
	if (ret < 0)
		goto err_host_probe;

	return 0;

 err_host_probe:
	pci_free_resource_list(&resources);

	return ret;
}
