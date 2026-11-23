/*
 * The device tree's graphs (ports and endpoints), which ath12k's core.c walks for the links between
 * several chips of one group (WSI): the phone's single WCN7850 has none, and no node has a graph.
 */
#ifndef _LX12_LINUX_OF_GRAPH_H_
#define _LX12_LINUX_OF_GRAPH_H_

#include <linux/of.h>

static inline struct device_node *
of_graph_get_endpoint_by_regs(const struct device_node *parent, int port, int endpoint)
{
	(void)parent;
	(void)port;
	(void)endpoint;
	return (NULL);
}

static inline struct device_node *
of_graph_get_remote_endpoint(const struct device_node *node)
{
	(void)node;
	return (NULL);
}

static inline struct device_node *
of_graph_get_port_parent(struct device_node *node)
{
	(void)node;
	return (NULL);
}

#endif
