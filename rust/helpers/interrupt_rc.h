/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RUST_HELPERS_INTERRUPT_RC_H
#define __RUST_HELPERS_INTERRUPT_RC_H
/*
 * refcounted local processor interrupt management.
 */
#include <linux/irqflags.h>
#include <linux/percpu.h>
#include <linux/preempt.h>

/* Per-CPU interrupt disabling state for local_interrupt_{disable,enable}(). */
DECLARE_PER_CPU(unsigned long, local_interrupt_disable_state);

static __always_inline void __local_interrupt_save_state(unsigned long flags)
{
	raw_cpu_write(local_interrupt_disable_state, flags);
}

static __always_inline void __local_interrupt_enable(void)
{
	unsigned long flags = raw_cpu_read(local_interrupt_disable_state);

	local_irq_restore(flags);
}

#ifndef INSTANTIATE_EXPORTED_INTERRUPT_DISABLE
static __always_inline void _local_interrupt_save_state(unsigned long flags)
{
	__local_interrupt_save_state(flags);
}

static __always_inline void _local_interrupt_enable(void)
{
	__local_interrupt_enable();
}
#else
extern void _local_interrupt_save_state(unsigned long flags);
extern void _local_interrupt_enable(void);
#endif

#define hardirq_disable_enter()	__preempt_count_add_return(HARDIRQ_DISABLE_OFFSET)
#define hardirq_disable_exit()	__preempt_count_sub_return(HARDIRQ_DISABLE_OFFSET)

static inline void local_interrupt_disable(void)
{
	int new_count;
	unsigned long flags;

	WARN_ON_ONCE(in_nmi());

	local_irq_save(flags);
	new_count = hardirq_disable_enter();

	if ((new_count & HARDIRQ_DISABLE_MASK) == HARDIRQ_DISABLE_OFFSET)
		_local_interrupt_save_state(flags);
}

static inline void local_interrupt_enable(void)
{
	int new_count;

	new_count = hardirq_disable_exit();

	if ((new_count & HARDIRQ_DISABLE_MASK) == 0)
		_local_interrupt_enable();
}

#endif /* !__RUST_HELPERS_INTERRUPT_RC_H */
