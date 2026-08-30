// SPDX-License-Identifier: GPL-2.0

#include <linux/export.h>
#include "spinlock.h"

#if !defined(CONFIG_GENERIC_LOCKBREAK) || defined(CONFIG_DEBUG_LOCK_ALLOC)
/* The __lock_function inlines are taken from "spinlock.h" */
#else

/* No rwlock_t variants for now, so just build this function by hand */
static void __lockfunc __raw_spin_lock_irq_disable(raw_spinlock_t *lock)
{
	for (;;) {
		preempt_disable();
		local_interrupt_disable();
		if (likely(do_raw_spin_trylock(lock)))
			break;
		local_interrupt_enable();
		preempt_enable();

		arch_spin_relax(&lock->raw_lock);
	}
}
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_IRQ
noinline void __lockfunc _raw_spin_lock_irq_disable(raw_spinlock_t *lock)
{
	__raw_spin_lock_irq_disable(lock);
}
EXPORT_SYMBOL_GPL(_raw_spin_lock_irq_disable);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_IRQ
noinline void __lockfunc _raw_spin_unlock_irq_enable(raw_spinlock_t *lock)
{
	__raw_spin_unlock_irq_enable(lock);
}
EXPORT_SYMBOL_GPL(_raw_spin_unlock_irq_enable);
#endif

__rust_helper void rust_helper___spin_lock_init(spinlock_t *lock,
						const char *name,
						struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_SPINLOCK
# if defined(CONFIG_PREEMPT_RT)
	__spin_lock_init(lock, name, key, false);
# else /*!CONFIG_PREEMPT_RT */
	__raw_spin_lock_init(spinlock_check(lock), name, key, LD_WAIT_CONFIG);
# endif /* CONFIG_PREEMPT_RT */
#else /* !CONFIG_DEBUG_SPINLOCK */
	spin_lock_init(lock);
#endif /* CONFIG_DEBUG_SPINLOCK */
}

__rust_helper void rust_helper_spin_lock(spinlock_t *lock)
{
	spin_lock(lock);
}

__rust_helper void rust_helper_spin_unlock(spinlock_t *lock)
{
	spin_unlock(lock);
}

__rust_helper int rust_helper_spin_trylock(spinlock_t *lock)
{
	return spin_trylock(lock);
}

__rust_helper void rust_helper_spin_assert_is_held(spinlock_t *lock)
{
	lockdep_assert_held(lock);
}

__rust_helper void rust_helper_spin_lock_irq_disable(spinlock_t *lock)
{
	spin_lock_irq_disable(lock);
}

__rust_helper void rust_helper_spin_unlock_irq_enable(spinlock_t *lock)
{
	spin_unlock_irq_enable(lock);
}

__rust_helper int rust_helper_spin_trylock_irq_disable(spinlock_t *lock)
{
	return spin_trylock_irq_disable(lock);
}
