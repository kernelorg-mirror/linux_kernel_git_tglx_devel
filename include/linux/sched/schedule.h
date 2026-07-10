/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_SCHEDULE_H
#define _LINUX_SCHED_SCHEDULE_H

#define	MAX_SCHEDULE_TIMEOUT		LONG_MAX

long schedule_timeout(long timeout);
long schedule_timeout_interruptible(long timeout);
long schedule_timeout_killable(long timeout);
long schedule_timeout_uninterruptible(long timeout);
long schedule_timeout_idle(long timeout);
asmlinkage void schedule(void);
void schedule_preempt_disabled(void);
asmlinkage void preempt_schedule_irq(void);
#ifdef CONFIG_PREEMPT_RT
 void schedule_rtlock(void);
#endif

int __must_check io_schedule_prepare(void);
void io_schedule_finish(int token);
long io_schedule_timeout(long timeout);
void io_schedule(void);

#endif
