/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RUST_HELPERS_SPINLOCK_H
#define __RUST_HELPERS_SPINLOCK_H

#include <linux/spinlock.h>
#include "interrupt_rc.h"

#ifdef CONFIG_SMP
void __lockfunc _raw_spin_lock_irq_disable(raw_spinlock_t *lock)	__acquires(lock);
void __lockfunc _raw_spin_unlock_irq_enable(raw_spinlock_t *lock)	__releases(lock);

/* Use the same config as spin_lock_irq() temporarily. */
#ifdef CONFIG_INLINE_SPIN_LOCK_IRQ
#define _raw_spin_lock_irq_disable(lock) __raw_spin_lock_irq_disable(lock)
#endif

/* Use the same config as spin_unlock_irq() temporarily. */
#ifdef CONFIG_INLINE_SPIN_UNLOCK_IRQ
#define _raw_spin_unlock_irq_enable(lock) __raw_spin_unlock_irq_enable(lock)
#endif

static __always_inline bool _raw_spin_trylock_irq_disable(raw_spinlock_t *lock)
	__cond_acquires(true, lock)
{
	local_interrupt_disable();
	if (_raw_spin_trylock(lock))
		return true;
	local_interrupt_enable();
	return false;
}

static inline void __raw_spin_lock_irq_disable(raw_spinlock_t *lock)
	__acquires(lock) __no_context_analysis
{
	local_interrupt_disable();
	preempt_disable();
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}

static inline void __raw_spin_unlock_irq_enable(raw_spinlock_t *lock)
	__releases(lock)
{
	spin_release(&lock->dep_map, _RET_IP_);
	do_raw_spin_unlock(lock);
	local_interrupt_enable();
	preempt_enable();
}

#else  /* CONFIG_SMP */

#define __LOCK_IRQ_DISABLE(lock, ...)						\
	do { local_interrupt_disable(); __LOCK(lock, ##__VA_ARGS__); } while (0)
#define __UNLOCK_IRQ_ENABLE(lock, ...)						\
	do { __UNLOCK(lock, ##__VA_ARGS__); local_interrupt_enable(); } while (0)

#define _raw_spin_lock_irq_disable(lock)	__LOCK_IRQ_DISABLE(lock)
#define _raw_spin_unlock_irq_enable(lock)	__UNLOCK_IRQ_ENABLE(lock)

static __always_inline int _raw_spin_trylock_irq_disable(raw_spinlock_t *lock)
	__cond_acquires(true, lock)
{
	__LOCK_IRQ_DISABLE(lock);
	return 1;
}

#endif /* CONFIG_SMP */

#define raw_spin_lock_irq_disable(lock)		_raw_spin_lock_irq_disable(lock)
#define raw_spin_unlock_irq_enable(lock)	_raw_spin_unlock_irq_enable(lock)
#define raw_spin_trylock_irq_disable(lock)	_raw_spin_trylock_irq_disable(lock)

#ifdef CONFIG_PREEMPT_RT
static __always_inline void spin_lock_irq_disable(spinlock_t *lock)
	__acquires(lock)
{
	rt_spin_lock(lock);
}

static __always_inline void spin_unlock_irq_enable(spinlock_t *lock)
	__releases(lock)
{
	rt_spin_unlock(lock);
}

static __always_inline int spin_trylock_irq_disable(spinlock_t *lock)
	__cond_acquires(true, lock)
{
	return rt_spin_trylock(lock);
}

#else  /* CONFIG_PREEMPT_RT */

static __always_inline void spin_lock_irq_disable(spinlock_t *lock)
	__acquires(lock) __no_context_analysis
{
	raw_spin_lock_irq_disable(&lock->rlock);
}

static __always_inline void spin_unlock_irq_enable(spinlock_t *lock)
	__releases(lock) __no_context_analysis
{
	raw_spin_unlock_irq_enable(&lock->rlock);
}

static __always_inline int spin_trylock_irq_disable(spinlock_t *lock)
	__cond_acquires(true, lock) __no_context_analysis
{
	return raw_spin_trylock_irq_disable(&lock->rlock);
}

#endif  /* !CONFIG_PREEMPT_RT */

#endif /* __RUST_HELPERS_SPINLOCK_H */
