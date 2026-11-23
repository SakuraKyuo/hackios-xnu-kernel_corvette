/*
 * The list of who is told when the kernel panics, as ath12k's core.c registers itself on: nobody is
 * told here (a panic of XNU's goes back to the loader, and shennong_wlan.c's quiesce unpowers the
 * radio on that way), so registering is accepted and does nothing.
 */
#ifndef _LX12_LINUX_PANIC_NOTIFIER_H_
#define _LX12_LINUX_PANIC_NOTIFIER_H_

#include <linux/notifier.h>

struct atomic_notifier_head {
	struct notifier_block *head;
};

static struct atomic_notifier_head panic_notifier_list __attribute__((unused));

static inline int
atomic_notifier_chain_register(struct atomic_notifier_head *list, struct notifier_block *block)
{
	(void)list;
	(void)block;
	return 0;
}

static inline int
atomic_notifier_chain_unregister(struct atomic_notifier_head *list, struct notifier_block *block)
{
	(void)list;
	(void)block;
	return 0;
}

#endif
