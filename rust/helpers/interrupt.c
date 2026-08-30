// SPDX-License-Identifier: GPL-2.0

#include <linux/export.h>
#include <linux/percpu.h>

#define INSTANTIATE_EXPORTED_INTERRUPT_DISABLE
#include "interrupt_rc.h"
#include "spinlock.h"

DEFINE_PER_CPU(unsigned long, local_interrupt_disable_state);

void _local_interrupt_save_state(unsigned long flags)
{
	__local_interrupt_save_state(flags);
}
EXPORT_SYMBOL(_local_interrupt_save_state);

void _local_interrupt_enable(void)
{
	__local_interrupt_enable();
}
EXPORT_SYMBOL(_local_interrupt_enable);

__rust_helper void rust_helper_local_interrupt_disable(void)
{
	local_interrupt_disable();
}

__rust_helper void rust_helper_local_interrupt_enable(void)
{
	local_interrupt_enable();
}
