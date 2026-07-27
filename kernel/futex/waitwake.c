// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/plist.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/freezer.h>

#include "futex.h"

/*
 * READ this before attempting to hack on futexes!
 *
 * Basic futex operation and ordering guarantees
 * =============================================
 *
 * The waiter reads the futex value in user space and calls
 * futex_wait(). This function computes the hash bucket and acquires
 * the hash bucket lock. After that it reads the futex user space value
 * again and verifies that the data has not changed. If it has not changed
 * it enqueues itself into the hash bucket, releases the hash bucket lock
 * and schedules.
 *
 * The waker side modifies the user space value of the futex and calls
 * futex_wake(). This function computes the hash bucket and acquires the
 * hash bucket lock. Then it looks for waiters on that futex in the hash
 * bucket and wakes them.
 *
 * In futex wake up scenarios where no tasks are blocked on a futex, taking
 * the hb spinlock can be avoided and simply return. In order for this
 * optimization to work, ordering guarantees must exist so that the waiter
 * being added to the list is acknowledged when the list is concurrently being
 * checked by the waker, avoiding scenarios like the following:
 *
 * CPU 0                               CPU 1
 * val = *futex;
 * sys_futex(WAIT, futex, val);
 *   futex_wait(futex, val);
 *   uval = *futex;
 *                                     *futex = newval;
 *                                     sys_futex(WAKE, futex);
 *                                       futex_wake(futex);
 *                                       if (queue_empty())
 *                                         return;
 *   if (uval == val)
 *      lock(hash_bucket(futex));
 *      queue();
 *     unlock(hash_bucket(futex));
 *     schedule();
 *
 * This would cause the waiter on CPU 0 to wait forever because it
 * missed the transition of the user space value from val to newval
 * and the waker did not find the waiter in the hash bucket queue.
 *
 * The correct serialization ensures that a waiter either observes
 * the changed user space value before blocking or is woken by a
 * concurrent waker:
 *
 * CPU 0                                 CPU 1
 * val = *futex;
 * sys_futex(WAIT, futex, val);
 *   futex_wait(futex, val);
 *
 *   waiters++; (a)
 *   smp_mb(); (A) <-- paired with -.
 *                                  |
 *   lock(hash_bucket(futex));      |
 *                                  |
 *   uval = *futex;                 |
 *                                  |        *futex = newval;
 *                                  |        sys_futex(WAKE, futex);
 *                                  |          futex_wake(futex);
 *                                  |
 *                                  `--------> smp_mb(); (B)
 *   if (uval == val)
 *     queue();
 *     unlock(hash_bucket(futex));
 *     schedule();                         if (waiters)
 *                                           lock(hash_bucket(futex));
 *   else                                    wake_waiters(futex);
 *     waiters--; (b)                        unlock(hash_bucket(futex));
 *
 * Where (A) orders the waiters increment and the futex value read through
 * atomic operations (see futex_hb_waiters_inc) and where (B) orders the write
 * to futex and the waiters read (see futex_hb_waiters_pending()).
 *
 * This yields the following case (where X:=waiters, Y:=futex):
 *
 *	X = Y = 0
 *
 *	w[X]=1		w[Y]=1
 *	MB		MB
 *	r[Y]=y		r[X]=x
 *
 * Which guarantees that x==0 && y==0 is impossible; which translates back into
 * the guarantee that we cannot both miss the futex variable change and the
 * enqueue.
 *
 * Note that a new waiter is accounted for in (a) even when it is possible that
 * the wait call can return error, in which case we backtrack from it in (b).
 * Refer to the comment in futex_q_lock().
 *
 * Similarly, in order to account for waiters being requeued on another
 * address we always increment the waiters for the destination bucket before
 * acquiring the lock. It then decrements them again  after releasing it -
 * the code that actually moves the futex(es) between hash buckets (requeue_futex)
 * will do the additional required waiter count housekeeping. This is done for
 * double_lock_hb() and double_unlock_hb(), respectively.
 */

bool __futex_wake_mark(struct futex_q *q)
{
	if (WARN(q->pi_state || q->rt_waiter, "refusing to wake PI futex\n"))
		return false;

	__futex_unqueue(q);
	/*
	 * The waiting task can free the futex_q as soon as q->lock_ptr = NULL
	 * is written, without taking any locks. This is possible in the event
	 * of a spurious wakeup, for example. A memory barrier is required here
	 * to prevent the following store to lock_ptr from getting ahead of the
	 * plist_del in __futex_unqueue().
	 */
	smp_store_release(&q->lock_ptr, NULL);

	return true;
}

/*
 * The hash bucket lock must be held when this is called.
 * Afterwards, the futex_q must not be accessed. Callers
 * must ensure to later call wake_up_q() for the actual
 * wakeups to occur.
 */
void futex_wake_mark(struct wake_q_head *wake_q, struct futex_q *q)
{
	struct task_struct *p = q->task;

	get_task_struct(p);

	if (!__futex_wake_mark(q)) {
		put_task_struct(p);
		return;
	}

	/*
	 * Queue the task for later wakeup for after we've released
	 * the hb->lock.
	 */
	wake_q_add_safe(wake_q, p);
}

static int evaluate_waiters(struct futex_hash_bucket *hb, union futex_key *key,
			    unsigned int nr_wake, u32 bitset, struct futex_q **to_wake)
{
	unsigned int waiters = 0, wakees = 0;
	struct futex_q *this, *next;

	plist_for_each_entry_safe(this, next, &hb->chain, list) {
		if (!futex_match(&this->key, key))
			continue;

		if (this->pi_state || this->rt_waiter)
			return -EINVAL;

		/*
		 * Waiters are counted independent of the bitset match. Stop the
		 * list walk when the total number of waiters becomes larger
		 * than the number of to be woken up tasks. In that case it does
		 * not matter how many wakees have been found. There will be
		 * waiters queued after the wake up no matter what. But that's
		 * only possible if there is a wakee cached in @to_wake.
		 * Otherwise the @nr_wake = 1 cached wakee optimization would
		 * not work. Keep walking if @wakees is still zero.
		 */
		if (++waiters > nr_wake && wakees)
			return 1;

		/*
		 * If the bitset matches increment @wakees unconditionally. It
		 * can't get larger than @nr_wake due to the exit condition
		 * above.
		 */
		if (this->bitset & bitset) {
			*to_wake = this;
			wakees++;
		}
	}

	/* Tell the caller whether there are more waiters than wakees */
	return waiters > wakees;
}

static inline int collect_cached_waiter(struct wake_q_head *wake_q, struct futex_q *to_wake)
{
	if (!to_wake)
		return 0;
	to_wake->wake(wake_q, to_wake);
	return 1;
}

static int collect_waiters(struct futex_hash_bucket *hb, struct wake_q_head *wake_q,
			   union futex_key *key, unsigned int nr_wake, u32 bitset)
{
	struct futex_q *this, *next;
	unsigned int wakees = 0;

	plist_for_each_entry_safe(this, next, &hb->chain, list) {
		if (!futex_match(&this->key, key))
			continue;

		if (this->pi_state || this->rt_waiter)
			return -EINVAL;

		/* Check if one of the bits is set in both bitsets */
		if (!(this->bitset & bitset))
			continue;

		this->wake(wake_q, this);
		if (++wakees == nr_wake)
			break;
	}

	return wakees;
}

static int __futex_robust_unlock(u32 __user *uaddr, void __user *pop, unsigned int flags,
				 unsigned int nr_wake, u32 bitset,
				 struct wake_q_head *wake_q)
{
	union futex_key key = FUTEX_KEY_INIT;
	int ret, nr_woken = 0;

	/*
	 * In the case that unlocking of the user space lock faulted, this could
	 * be optimized to not re-evaluate the key for private futexes, but
	 * there is actually zero benefit to do so. It's very unlikely that the
	 * unlock faults right after user space attempted a TID -> 0 transition
	 * on that address, so optimizing for that corner case is pointless.
	 */
	ret = get_futex_key(uaddr, flags, &key, FUTEX_WRITE);
	if (unlikely(ret))
		return ret;

	CLASS(hbr, hbr)(&key);
	auto hb = hbr.hb;

	/*
	 * This has to take the hash bucket lock unconditionally and cannot rely
	 * on futex_hb_waiters_pending(hb) as that would open a race condition
	 * between the unlock operation and a concurrent incoming waiter:
	 *
	 *						user_lock |= FUTEX_WAITERS;
	 *   if (!futex_hb_waiters_pending(hb))
	 *	robust_unlock(0)			atomic_inc(hb::waiters);
	 *						lock(hb)
	 *						// Succeeds!
	 *						compare_user_lock()
	 *
	 *	    user_lock = 0; // clears the FUTEX_WAITERS bit
	 *
	 * Though it's likely that there is at least one waiter queued because
	 * the userspace TID -> 0 transition failed due to the FUTEX_WAITERS bit
	 * being set, which means the lock has to be taken in the majority of
	 * cases anyway.
	 *
	 * As the FUTEX_WAITERS bit stays consistent, the optimization of the
	 * lockless quick check is not that relevant anymore because this avoids
	 * that the bit has to be set unconditionally by woken up waiters when
	 * they return to user space and acquire the lock. Which means if there
	 * was only one waiter queued the unlock is uncontended and avoids the
	 * syscall completely.
	 */
	scoped_guard(spinlock, &hb->lock) {
		struct futex_q *to_wake = NULL;

		/*
		 * The unlock has to happen _before_ waiters are collected
		 * because they can return from futex_wait() without taking the
		 * hash bucket lock when collect_waiters() sets futex_q::lock_ptr
		 * to NULL. That can result in the following situation:
		 *
		 * collect_waiters()
		 *   collects T2		kill(T2);
		 *	q->lock_ptr = NULL	T2 runs
		 *				if (!q->lock_ptr)
		 *				    return;
		 *				exit_to_user()
		 *				  handle_signal()
		 *				    do_exit()
		 *				      handle_futex_death()
		 *				      // observes lock = TID(OWNER)
		 * lock = FUTEX_WAITERS;
		 *
		 * That means all waiters which are still queued can become
		 * stale unless there is another lock/unlock operation on the
		 * futex later.
		 *
		 * Evaluate waiters and wakees to keep the FUTEX_WAITERS bit in
		 * the user space lock consistent.
		 */
		ret = evaluate_waiters(hb, &key, nr_wake, bitset, &to_wake);
		if (ret < 0)
			return ret;

		/*
		 * Unlock the futex in user space while holding the hash bucket
		 * lock to keep the FUTEX_WAITERS bit consistent. Newly incoming
		 * waiters are serialized on the hash bucket lock and will
		 * observe that the user space value has changed once they
		 * acquired it.
		 */
		u32 uval = ret ? FUTEX_WAITERS : 0;

		guard(pagefault)();
		scoped_user_write_access(uaddr, efault_uaddr)
			unsafe_atomic_store_release_user(uval, uaddr, efault_uaddr);

		/*
		 * Now that the unlock has been successful, collect the waiters
		 * for wake up. If @nr_wake is 1, which is the common case, then
		 * evaluate_waiters() has stored the first eligible waiter in
		 * @to_wake, if it found one.  Spare another hash bucket walk
		 * for that case.
		 */
		if (likely(nr_wake == 1))
			nr_woken = collect_cached_waiter(wake_q, to_wake);
		else
			nr_woken = collect_waiters(hb, wake_q, &key, nr_wake, bitset);

		/*
		 * This should never happen because evaluate_waiters() would
		 * have detected a PI futex mixup already.
		 */
		if (WARN_ON_ONCE(nr_woken < 0))
			return nr_woken;
	}

	/*
	 * Clear the pending list op now that everything is done. If clearing
	 * the pending op fails, then the task is in deeper trouble as the
	 * robust list head is usually part of the TLS. The chance of survival
	 * is close to zero and retry is pointless as the fault is terminal.
	 */
	return futex_robust_list_clear_pending(pop, flags) ? nr_woken : -EFAULT;

efault_uaddr:
	/* Unlock faulted. Try to fault in @uaddr and repeat if successful */
	return fault_in_user_writeable(uaddr) ? : -EAGAIN;
}

/*
 * Robust futex unlock procedure:
 *
 *	- count waiters to determine the FUTEX_WAITERS bit for the unlock
 *	- unlock the user space lock
 *	- collect up to @nr_wake waiters
 *	- clear the user space robust list pending op pointer
 *	- wake up the collected waiters.
 */
static int futex_robust_unlock(u32 __user *uaddr, void __user *pop, unsigned int flags,
			       unsigned int nr_wake, u32 bitset)
{
	DEFINE_WAKE_Q(wake_q);
	int ret = -EAGAIN;

	/*
	 * This loops in case that unlocking the user space lock faults and the
	 * fault resolution is successful.
	 */
	for (; ret == -EAGAIN;)
		ret = __futex_robust_unlock(uaddr, pop, flags, nr_wake, bitset, &wake_q);

	wake_up_q(&wake_q);
	return ret;
}

/*
 * Plain wake() operation procedure:
 *	- collect and wake up to @nr_wake waiters
 */
static int __futex_wake(u32 __user *uaddr, unsigned int flags, unsigned int nr_wake, u32 bitset)
{
	union futex_key key = FUTEX_KEY_INIT;
	DEFINE_WAKE_Q(wake_q);
	int ret;

	ret = get_futex_key(uaddr, flags, &key, FUTEX_READ);
	if (unlikely(ret))
		return ret;

	CLASS(hbr, hbr)(&key);
	auto hb = hbr.hb;

	/*
	 * For regular wakeup operations the lockless quick check is
	 * sufficient. See the ordering guarantees documentation at the top of
	 * this file.
	 */
	if (!futex_hb_waiters_pending(hb))
		return 0;

	scoped_guard(spinlock, &hb->lock)
		ret = collect_waiters(hb, &wake_q, &key, nr_wake, bitset);

	wake_up_q(&wake_q);
	return ret;
}

/*
 * Wake up waiters matching bitset queued on this futex (uaddr).
 */
int futex_wake(u32 __user *uaddr, unsigned int flags, void __user *pop, int nr_wake, u32 bitset)
{
	bool robust_unlock = !!(flags & FLAGS_ROBUST_UNLOCK);

	if (!bitset || nr_wake < 0)
		return -EINVAL;

	if (unlikely(!nr_wake)) {
		if (flags & FLAGS_STRICT)
			return robust_unlock ? -EINVAL : 0;
		/* Backwards compatibility for sys_futex() */
		nr_wake = 1;
	}

	if (robust_unlock)
		return futex_robust_unlock(uaddr, pop, flags, nr_wake, bitset);

	return __futex_wake(uaddr, flags, nr_wake, bitset);
}

static int futex_atomic_op_inuser(unsigned int encoded_op, u32 __user *uaddr)
{
	unsigned int op =	  (encoded_op & 0x70000000) >> 28;
	unsigned int cmp =	  (encoded_op & 0x0f000000) >> 24;
	int oparg = sign_extend32((encoded_op & 0x00fff000) >> 12, 11);
	int cmparg = sign_extend32(encoded_op & 0x00000fff, 11);
	int oldval, ret;

	if (encoded_op & (FUTEX_OP_OPARG_SHIFT << 28)) {
		if (oparg < 0 || oparg > 31) {
			/*
			 * kill this print and return -EINVAL when userspace
			 * is sane again
			 */
			pr_info_ratelimited("futex_wake_op: %s tries to shift op by %d; fix this program\n",
					    current->comm, oparg);
			oparg &= 31;
		}
		oparg = 1 << oparg;
	}

	pagefault_disable();
	ret = arch_futex_atomic_op_inuser(op, oparg, &oldval, uaddr);
	pagefault_enable();
	if (ret)
		return ret;

	switch (cmp) {
	case FUTEX_OP_CMP_EQ:
		return oldval == cmparg;
	case FUTEX_OP_CMP_NE:
		return oldval != cmparg;
	case FUTEX_OP_CMP_LT:
		return oldval < cmparg;
	case FUTEX_OP_CMP_GE:
		return oldval >= cmparg;
	case FUTEX_OP_CMP_LE:
		return oldval <= cmparg;
	case FUTEX_OP_CMP_GT:
		return oldval > cmparg;
	default:
		return -ENOSYS;
	}
}

/*
 * Wake up all waiters hashed on the physical page that is mapped
 * to this virtual address:
 */
int futex_wake_op(u32 __user *uaddr1, unsigned int flags, u32 __user *uaddr2,
		  int nr_wake, int nr_wake2, int op)
{
	union futex_key key1 = FUTEX_KEY_INIT, key2 = FUTEX_KEY_INIT;
	struct futex_q *this, *next;
	int ret, op_ret;
	DEFINE_WAKE_Q(wake_q);

retry:
	ret = get_futex_key(uaddr1, flags, &key1, FUTEX_READ);
	if (unlikely(ret != 0))
		return ret;
	ret = get_futex_key(uaddr2, flags, &key2, FUTEX_WRITE);
	if (unlikely(ret != 0))
		return ret;

retry_private:
	if (1) {
		CLASS(hbr, hbr1)(&key1);
		CLASS(hbr, hbr2)(&key2);
		auto hb1 = hbr1.hb;
		auto hb2 = hbr2.hb;

		double_lock_hb(hb1, hb2);
		op_ret = futex_atomic_op_inuser(op, uaddr2);
		if (unlikely(op_ret < 0)) {
			double_unlock_hb(hb1, hb2);

			if (!IS_ENABLED(CONFIG_MMU) ||
			    unlikely(op_ret != -EFAULT && op_ret != -EAGAIN)) {
				/*
				 * we don't get EFAULT from MMU faults if we don't have
				 * an MMU, but we might get them from range checking
				 */
				ret = op_ret;
				return ret;
			}

			if (op_ret == -EFAULT) {
				ret = fault_in_user_writeable(uaddr2);
				if (ret)
					return ret;
			}

			cond_resched();
			if (!(flags & FLAGS_SHARED))
				goto retry_private;
			goto retry;
		}

		plist_for_each_entry_safe(this, next, &hb1->chain, list) {
			if (futex_match(&this->key, &key1)) {
				if (this->pi_state || this->rt_waiter) {
					ret = -EINVAL;
					goto out_unlock;
				}
				this->wake(&wake_q, this);
				if (++ret >= nr_wake)
					break;
			}
		}

		if (op_ret > 0) {
			op_ret = 0;
			plist_for_each_entry_safe(this, next, &hb2->chain, list) {
				if (futex_match(&this->key, &key2)) {
					if (this->pi_state || this->rt_waiter) {
						ret = -EINVAL;
						goto out_unlock;
					}
					this->wake(&wake_q, this);
					if (++op_ret >= nr_wake2)
						break;
				}
			}
			ret += op_ret;
		}

out_unlock:
		double_unlock_hb(hb1, hb2);
	}
	wake_up_q(&wake_q);
	return ret;
}

static long futex_wait_restart(struct restart_block *restart);

/**
 * futex_do_wait() - wait for wakeup, timeout, or signal
 * @q:		the futex_q to queue up on
 * @timeout:	the prepared hrtimer_sleeper, or null for no timeout
 */
void futex_do_wait(struct futex_q *q, struct hrtimer_sleeper *timeout)
{
	/* Arm the timer */
	if (timeout)
		hrtimer_sleeper_start_expires(timeout, HRTIMER_MODE_ABS);

	/*
	 * If we have been removed from the hash list, then another task
	 * has tried to wake us, and we can skip the call to schedule().
	 */
	if (likely(!plist_node_empty(&q->list))) {
		/*
		 * If the timer has already expired, current will already be
		 * flagged for rescheduling. Only call schedule if there
		 * is no timeout, or if it has yet to expire.
		 */
		if (!timeout || timeout->task)
			schedule();
	}
	__set_current_state(TASK_RUNNING);
}

/**
 * futex_unqueue_multiple - Remove various futexes from their hash bucket
 * @v:	   The list of futexes to unqueue
 * @count: Number of futexes in the list
 *
 * Helper to unqueue a list of futexes. This can't fail.
 *
 * Return:
 *  - >=0 - Index of the last futex that was awoken;
 *  - -1  - No futex was awoken
 */
int futex_unqueue_multiple(struct futex_vector *v, int count)
{
	int ret = -1, i;

	for (i = 0; i < count; i++) {
		if (!futex_unqueue(&v[i].q))
			ret = i;
	}

	return ret;
}

/**
 * futex_wait_multiple_setup - Prepare to wait and enqueue multiple futexes
 * @vs:		The futex list to wait on
 * @count:	The size of the list
 * @woken:	Index of the last woken futex, if any. Used to notify the
 *		caller that it can return this index to userspace (return parameter)
 *
 * Prepare multiple futexes in a single step and enqueue them. This may fail if
 * the futex list is invalid or if any futex was already awoken. On success the
 * task is ready to interruptible sleep.
 *
 * Return:
 *  -  1 - One of the futexes was woken by another thread
 *  -  0 - Success
 *  - <0 - -EFAULT, -EWOULDBLOCK or -EINVAL
 */
int futex_wait_multiple_setup(struct futex_vector *vs, int count, int *woken)
{
	bool retry = false;
	int ret, i;
	u32 uval;

	/*
	 * Make sure to have a reference on the private_hash such that we
	 * don't block on rehash after changing the task state below.
	 */
	guard(private_hash)(current->mm);

	/*
	 * Enqueuing multiple futexes is tricky, because we need to enqueue
	 * each futex on the list before dealing with the next one to avoid
	 * deadlocking on the hash bucket. But, before enqueuing, we need to
	 * make sure that current->state is TASK_INTERRUPTIBLE, so we don't
	 * lose any wake events, which cannot be done before the get_futex_key
	 * of the next key, because it calls get_user_pages, which can sleep.
	 * Thus, we fetch the list of futexes keys in two steps, by first
	 * pinning all the memory keys in the futex key, and only then we read
	 * each key and queue the corresponding futex.
	 *
	 * Private futexes doesn't need to recalculate hash in retry, so skip
	 * get_futex_key() when retrying.
	 */
retry:
	for (i = 0; i < count; i++) {
		if (!(vs[i].w.flags & FLAGS_SHARED) && retry)
			continue;

		ret = get_futex_key(u64_to_user_ptr(vs[i].w.uaddr),
				    vs[i].w.flags,
				    &vs[i].q.key, FUTEX_READ);

		if (unlikely(ret))
			return ret;
	}

	set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);

	for (i = 0; i < count; i++) {
		u32 __user *uaddr = (u32 __user *)(unsigned long)vs[i].w.uaddr;
		struct futex_q *q = &vs[i].q;
		u32 val = vs[i].w.val;

		if (1) {
			CLASS(hbr, hbr)(&q->key);
			auto hb = hbr.hb;

			futex_q_lock(q, hb);
			ret = futex_get_value_locked(&uval, uaddr);

			if (!ret && uval == val) {
				/*
				 * The bucket lock can't be held while dealing with the
				 * next futex. Queue each futex at this moment so hb can
				 * be unlocked.
				 */
				futex_queue(q, hb, current);
				continue;
			}

			futex_q_unlock(hb);
			__release(q->lock_ptr);
		}
		__set_current_state(TASK_RUNNING);

		/*
		 * Even if something went wrong, if we find out that a futex
		 * was woken, we don't return error and return this index to
		 * userspace
		 */
		*woken = futex_unqueue_multiple(vs, i);
		if (*woken >= 0)
			return 1;

		if (ret) {
			/*
			 * If we need to handle a page fault, we need to do so
			 * without any lock and any enqueued futex (otherwise
			 * we could lose some wakeup). So we do it here, after
			 * undoing all the work done so far. In success, we
			 * retry all the work.
			 */
			if (get_user(uval, uaddr))
				return -EFAULT;

			retry = true;
			goto retry;
		}

		if (uval != val)
			return -EWOULDBLOCK;
	}

	return 0;
}

/**
 * futex_sleep_multiple - Check sleeping conditions and sleep
 * @vs:    List of futexes to wait for
 * @count: Length of vs
 * @to:    Timeout
 *
 * Sleep if and only if the timeout hasn't expired and no futex on the list has
 * been woken up.
 */
static void futex_sleep_multiple(struct futex_vector *vs, unsigned int count,
				 struct hrtimer_sleeper *to)
{
	if (to && !to->task)
		return;

	for (; count; count--, vs++) {
		if (!READ_ONCE(vs->q.lock_ptr))
			return;
	}

	schedule();
}

/**
 * futex_wait_multiple - Prepare to wait on and enqueue several futexes
 * @vs:		The list of futexes to wait on
 * @count:	The number of objects
 * @to:		Timeout before giving up and returning to userspace
 *
 * Entry point for the FUTEX_WAIT_MULTIPLE futex operation, this function
 * sleeps on a group of futexes and returns on the first futex that is
 * wake, or after the timeout has elapsed.
 *
 * Return:
 *  - >=0 - Hint to the futex that was awoken
 *  - <0  - On error
 */
int futex_wait_multiple(struct futex_vector *vs, unsigned int count,
			struct hrtimer_sleeper *to)
{
	int ret, hint = 0;

	if (to)
		hrtimer_sleeper_start_expires(to, HRTIMER_MODE_ABS);

	while (1) {
		ret = futex_wait_multiple_setup(vs, count, &hint);
		if (ret) {
			if (ret > 0) {
				/* A futex was woken during setup */
				ret = hint;
			}
			return ret;
		}

		futex_sleep_multiple(vs, count, to);

		__set_current_state(TASK_RUNNING);

		ret = futex_unqueue_multiple(vs, count);
		if (ret >= 0)
			return ret;

		if (to && !to->task)
			return -ETIMEDOUT;
		else if (signal_pending(current))
			return -ERESTARTSYS;
		/*
		 * The final case is a spurious wakeup, for
		 * which just retry.
		 */
	}
}

/**
 * futex_wait_setup() - Prepare to wait on a futex
 * @uaddr:	the futex userspace address
 * @val:	the expected value
 * @flags:	futex flags (FLAGS_SHARED, etc.)
 * @q:		the associated futex_q
 * @key2:	the second futex_key if used for requeue PI
 * @task:	Task queueing this futex
 *
 * Setup the futex_q and locate the hash_bucket.  Get the futex value and
 * compare it with the expected value.  Handle atomic faults internally.
 * Return with the hb lock held on success, and unlocked on failure.
 *
 * Return:
 *  -  0 - uaddr contains val and hb has been locked;
 *  - <0 - On error and the hb is unlocked. A possible reason: the uaddr can not
 *	   be read, does not contain the expected value or is not properly aligned.
 */
int futex_wait_setup(u32 __user *uaddr, u32 val, unsigned int flags,
		     struct futex_q *q, union futex_key *key2,
		     struct task_struct *task)
{
	u32 uval;
	int ret;

	/*
	 * Access the page AFTER the hash-bucket is locked.
	 * Order is important:
	 *
	 *   Userspace waiter: val = var; if (cond(val)) futex_wait(&var, val);
	 *   Userspace waker:  if (cond(var)) { var = new; futex_wake(&var); }
	 *
	 * The basic logical guarantee of a futex is that it blocks ONLY
	 * if cond(var) is known to be true at the time of blocking, for
	 * any cond.  If we locked the hash-bucket after testing *uaddr, that
	 * would open a race condition where we could block indefinitely with
	 * cond(var) false, which would violate the guarantee.
	 *
	 * On the other hand, we insert q and release the hash-bucket only
	 * after testing *uaddr.  This guarantees that futex_wait() will NOT
	 * absorb a wakeup if *uaddr does not match the desired values
	 * while the syscall executes.
	 */
retry:
	ret = get_futex_key(uaddr, flags, &q->key, FUTEX_READ);
	if (unlikely(ret != 0))
		return ret;

retry_private:
	if (1) {
		CLASS(hbr, hbr)(&q->key);
		auto hb = hbr.hb;

		futex_q_lock(q, hb);

		ret = futex_get_value_locked(&uval, uaddr);

		if (ret) {
			futex_q_unlock(hb);
			__release(q->lock_ptr);

			ret = get_user(uval, uaddr);
			if (ret)
				return ret;

			if (!(flags & FLAGS_SHARED))
				goto retry_private;

			goto retry;
		}

		if (uval != val) {
			futex_q_unlock(hb);
			__release(q->lock_ptr);
			return -EWOULDBLOCK;
		}

		if (key2 && futex_match(&q->key, key2)) {
			futex_q_unlock(hb);
			__release(q->lock_ptr);
			return -EINVAL;
		}

		/*
		 * The task state is guaranteed to be set before another task can
		 * wake it. set_current_state() is implemented using smp_store_mb() and
		 * futex_queue() calls spin_unlock() upon completion, both serializing
		 * access to the hash list and forcing another memory barrier.
		 */
		if (task == current)
			set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
		futex_queue(q, hb, task);
	}

	return ret;
}

int __futex_wait(u32 __user *uaddr, unsigned int flags, u32 val,
		 struct hrtimer_sleeper *to, u32 bitset)
{
	struct futex_q q = futex_q_init;
	int ret;

	if (!bitset)
		return -EINVAL;

	q.bitset = bitset;

retry:
	/*
	 * Prepare to wait on uaddr. On success, it holds hb->lock and q
	 * is initialized.
	 */
	ret = futex_wait_setup(uaddr, val, flags, &q, NULL, current);
	if (ret)
		return ret;

	/* futex_queue and wait for wakeup, timeout, or a signal. */
	futex_do_wait(&q, to);

	/* If we were woken (and unqueued), we succeeded, whatever. */
	if (!futex_unqueue(&q))
		return 0;

	if (to && !to->task)
		return -ETIMEDOUT;

	/*
	 * We expect signal_pending(current), but we might be the
	 * victim of a spurious wakeup as well.
	 */
	if (!signal_pending(current))
		goto retry;

	return -ERESTARTSYS;
}

int futex_wait(u32 __user *uaddr, unsigned int flags, u32 val, ktime_t *abs_time, u32 bitset)
{
	struct hrtimer_sleeper timeout, *to;
	struct restart_block *restart;
	int ret;

	to = futex_setup_timer(abs_time, &timeout, flags,
			       current->timer_slack_ns);

	ret = __futex_wait(uaddr, flags, val, to, bitset);

	/* No timeout, nothing to clean up. */
	if (!to)
		return ret;

	hrtimer_cancel(&to->timer);
	destroy_hrtimer_on_stack(&to->timer);

	if (ret == -ERESTARTSYS) {
		restart = &current->restart_block;
		restart->futex.uaddr = uaddr;
		restart->futex.val = val;
		restart->futex.time = *abs_time;
		restart->futex.bitset = bitset;
		restart->futex.flags = flags | FLAGS_HAS_TIMEOUT;

		return set_restart_fn(restart, futex_wait_restart);
	}

	return ret;
}

static long futex_wait_restart(struct restart_block *restart)
{
	u32 __user *uaddr = restart->futex.uaddr;
	ktime_t *tp = NULL;

	if (restart->futex.flags & FLAGS_HAS_TIMEOUT)
		tp = &restart->futex.time;

	restart->fn = do_no_restart_syscall;

	return (long)futex_wait(uaddr, restart->futex.flags,
				restart->futex.val, tp, restart->futex.bitset);
}

