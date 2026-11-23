/*
 * wifi/lx's reserved memory, and what ath12k adds to it: the region a node names, which its
 * qmi.c takes for the firmware's memory when the device tree fixes it ("memory-region"). The
 * phone's radio has none: the firmware's memory is ours (shennong_wlan.c's, as ath12k gives it
 * when there is no fixed region), so no node is ever a reserved region here.
 */
#ifndef _LX12_LINUX_OF_RESERVED_MEM_H_
#define _LX12_LINUX_OF_RESERVED_MEM_H_

#include_next <linux/of_reserved_mem.h>

struct reserved_mem {
	const char		*name;
	phys_addr_t		base;
	phys_addr_t		size;
};

static inline struct reserved_mem *
of_reserved_mem_lookup(struct device_node *node)
{
	(void)node;
	return (NULL);
}

#endif
