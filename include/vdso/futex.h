/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VDSO_FUTEX_H
#define _VDSO_FUTEX_H

#include <linux/types.h>

struct robust_list;

/**
 * __vdso_futex_robust_try_unlock - Try to unlock an uncontended robust futex
 * @lock:	Pointer to the futex lock object
 * @tid:	The TID of the calling task
 * @op:		Pointer to the task's robust_list_head::list_pending_op
 *
 * Return: The content of *@lock. On success this is the same as @tid.
 *
 * The function implements:
 *	if (atomic_try_cmpxchg(lock, &tid, 0))
 *		*op = NULL;
 *	return tid;
 *
 * There is a race between a successful unlock and clearing the pending op
 * pointer in the robust list head. If the calling task is interrupted in the
 * race window and has to handle a (fatal) signal on return to user space then
 * the kernel handles the clearing of @pending_op before attempting to deliver
 * the signal. That ensures that a task cannot exit with a potentially invalid
 * pending op pointer.
 *
 * User space uses it in the following way:
 *
 * if (__vdso_futex_robust_try_unlock(lock, tid, &pending_op) != tid)
 *	err = sys_futex($OP | FUTEX_ROBUST_UNLOCK,....);
 *
 * If the unlock attempt fails due to the FUTEX_WAITERS bit set in the lock,
 * then the syscall does the unlock, clears the pending op pointer and wakes the
 * requested number of waiters.
 *
 * The @op pointer is intentionally void. It has the same requirements as the
 * @uaddr2 argument for sys_futex(FUTEX_ROBUST_UNLOCK) operations. See the
 * modifier and the related documentation in include/uapi/linux/futex.h
 */
uint32_t __vdso_futex_robust_try_unlock(uint32_t *lock, uint32_t tid, void *op);

#endif
