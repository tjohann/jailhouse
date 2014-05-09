/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2014
 *
 * Authors:
 *  Ivan Kolchin <ivan.kolchin@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/acpi.h>
#include <jailhouse/mmio.h>
#include <jailhouse/pci.h>
#include <jailhouse/printk.h>
#include <jailhouse/utils.h>

struct acpi_mcfg_alloc {
	u64 base_addr;
	u16 segment_num;
	u8 start_bus;
	u8 end_bus;
	u32 reserved;
} __attribute__((packed));

struct acpi_mcfg_table {
	struct acpi_table_header header;
	u8 reserved[8];
	struct acpi_mcfg_alloc alloc_structs[];
} __attribute__((packed));

/* entry for PCI config space whitelist (granting access) */
struct pci_cfg_access {
	u32 reg_num; /** Register number (4-byte aligned) */
	u32 mask; /** Bit set: access allowed */
};

/* --- Whilelist for writing to PCI config space registers --- */
/* Type 1: Endpoints */
static const struct pci_cfg_access endpoint_write_access[] = {
	{ 0x04, 0xffffffff }, /* Command, Status */
	{ 0x0c, 0xff000000 }, /* BIST */
	{ 0x3c, 0x000000ff }, /* Int Line */
};
/* Type 2: Bridges */
static const struct pci_cfg_access bridge_write_access[] = {
	{ 0x04, 0xffffffff }, /* Command, Status */
	{ 0x0c, 0xff000000 }, /* BIST */
	{ 0x3c, 0xffff00ff }, /* Int Line, Bridge Control */
};

static void *pci_space;
static u64 pci_mmcfg_addr;
static u32 pci_mmcfg_size;

/**
 * pci_get_assigned_device() - Look up device owned by a cell
 * @cell:	Owning cell
 * @bdf:	16-bit bus/device/function ID
 *
 * Return: Valid pointer - owns, NULL - doesn't own.
 */
const struct jailhouse_pci_device *
pci_get_assigned_device(const struct cell *cell, u16 bdf)
{
	const struct jailhouse_pci_device *device =
		jailhouse_cell_pci_devices(cell->config);
	u32 n;

	for (n = 0; n < cell->config->num_pci_devices; n++)
		if (((device[n].bus << 8) | device[n].devfn) == bdf)
			return &device[n];

	return NULL;
}

/**
 * pci_cfg_write_allowed() - Check general config space write permission
 * @type:	JAILHOUSE_PCI_TYPE_DEVICE or JAILHOUSE_PCI_TYPE_BRIDGE
 * @reg_num:	Register number (4-byte aligned)
 * @bias:	Bias from register base address in bytes
 * @size:	Access size (1, 2 or 4 bytes)
 *
 * Return: True if writing is allowed, false otherwise.
 */
bool pci_cfg_write_allowed(u32 type, u8 reg_num, unsigned int reg_bias,
			   unsigned int size)
{
	/* initialize list to work around wrong compiler warning */
	const struct pci_cfg_access *list = NULL;
	unsigned int n, len = 0;

	if (type == JAILHOUSE_PCI_TYPE_DEVICE) {
		list = endpoint_write_access;
		len = ARRAY_SIZE(endpoint_write_access);
	} else if (type == JAILHOUSE_PCI_TYPE_BRIDGE) {
		list = bridge_write_access;
		len = ARRAY_SIZE(bridge_write_access);
	}

	for (n = 0; n < len; n++)
		if (list[n].reg_num == reg_num)
			return ((list[n].mask >> (reg_bias * 8)) &
				 BYTE_MASK(size)) == BYTE_MASK(size);

	return false;
}

/**
 * pci_init() - Initialization of PCI module
 *
 * Return: 0 - success, error code - if error.
 */
int pci_init(void)
{
	struct acpi_mcfg_table *mcfg;

	mcfg = (struct acpi_mcfg_table *)acpi_find_table("MCFG", NULL);
	if (!mcfg)
		return 0;

	if (mcfg->header.length !=
	    sizeof(struct acpi_mcfg_table) + sizeof(struct acpi_mcfg_alloc))
		return -EIO;

	pci_mmcfg_addr = mcfg->alloc_structs[0].base_addr;
	pci_mmcfg_size = (mcfg->alloc_structs[0].end_bus -
			  mcfg->alloc_structs[0].start_bus) * 256 * 4096;
	pci_space = page_alloc(&remap_pool, pci_mmcfg_size / PAGE_SIZE);
	if (pci_space)
		page_map_create(&hv_paging_structs,
				mcfg->alloc_structs[0].base_addr,
				pci_mmcfg_size, (unsigned long)pci_space,
				PAGE_DEFAULT_FLAGS | PAGE_FLAG_UNCACHED,
				PAGE_MAP_NON_COHERENT);

	return 0;
}

/**
 * pci_mmio_access_handler() - Handler for MMIO-accesses to PCI config space
 * @cell:	Request issuing cell
 * @is_write:	True if write access
 * @addr:	Address accessed
 * @value:	Pointer to value for reading/writing
 *
 * Return: 1 if handled successfully, 0 if unhandled, -1 on access error
 */
int pci_mmio_access_handler(const struct cell *cell, bool is_write,
			    u64 addr, u32 *value)
{
	const struct jailhouse_pci_device *device;
	u32 mmcfg_offset;
	u32 reg_num;
	u32 reg_bias;

	if (addr < pci_mmcfg_addr ||
	    addr >= (pci_mmcfg_addr + pci_mmcfg_size - 4))
		return 0;

	mmcfg_offset = addr - pci_mmcfg_addr;
	reg_bias = mmcfg_offset % 4;
	reg_num = mmcfg_offset & 0xfff;
	device = pci_get_assigned_device(cell, mmcfg_offset >> 12);

	if (is_write) {
		if (!device)
			return -1;
		if (reg_num < PCI_CONFIG_HEADER_SIZE) {
			if (!pci_cfg_write_allowed(device->type,
						   (reg_num - reg_bias),
						   reg_bias, 4))
				return -1;
		} else {
			// TODO: moderate capability access
			return -1;
		}
		mmio_write32(pci_space + mmcfg_offset, *value);
	} else
		if (device)
			*value = mmio_read32(pci_space + mmcfg_offset);
		else
			*value = -1;

	return 1;
}