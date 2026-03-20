// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2004 Linus Torvalds, Ingo Molnar
 *
 * This file contains the /proc/irq/ handling code.
 */

#include <linux/irq.h>
#include <linux/gfp.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <uapi/linux/irqstats.h>

#include "internals.h"

/*
 * Access rules:
 *
 * procfs protects read/write of /proc/irq/N/ files against a
 * concurrent free of the interrupt descriptor. remove_proc_entry()
 * immediately prevents new read/writes to happen and waits for
 * already running read/write functions to complete.
 *
 * We remove the proc entries first and then delete the interrupt
 * descriptor from the radix tree and free it. So it is guaranteed
 * that irq_to_desc(N) is valid as long as the read/writes are
 * permitted by procfs.
 *
 * The read from /proc/interrupts is a different problem because there
 * is no protection. So the lookup and the access to irqdesc
 * information must be protected by sparse_irq_lock.
 */
static struct proc_dir_entry *root_irq_dir;

#ifdef CONFIG_SMP

enum {
	AFFINITY,
	AFFINITY_LIST,
	EFFECTIVE,
	EFFECTIVE_LIST,
};

static int show_irq_affinity(int type, struct seq_file *m)
{
	struct irq_desc *desc = irq_to_desc((long)m->private);
	const struct cpumask *mask;

	guard(raw_spinlock_irq)(&desc->lock);

	switch (type) {
	case AFFINITY:
	case AFFINITY_LIST:
		mask = desc->irq_common_data.affinity;
		if (irq_move_pending(&desc->irq_data))
			mask = irq_desc_get_pending_mask(desc);
		break;
	case EFFECTIVE:
	case EFFECTIVE_LIST:
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
		mask = irq_data_get_effective_affinity_mask(&desc->irq_data);
		break;
#endif
	default:
		return -EINVAL;
	}

	switch (type) {
	case AFFINITY_LIST:
	case EFFECTIVE_LIST:
		seq_printf(m, "%*pbl\n", cpumask_pr_args(mask));
		break;
	case AFFINITY:
	case EFFECTIVE:
		seq_printf(m, "%*pb\n", cpumask_pr_args(mask));
		break;
	}
	return 0;
}

static int irq_affinity_hint_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long)m->private);
	cpumask_var_t mask;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	scoped_guard(raw_spinlock_irq, &desc->lock) {
		if (desc->affinity_hint)
			cpumask_copy(mask, desc->affinity_hint);
	}

	seq_printf(m, "%*pb\n", cpumask_pr_args(mask));
	free_cpumask_var(mask);
	return 0;
}

int no_irq_affinity;
static int irq_affinity_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(AFFINITY, m);
}

static int irq_affinity_list_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(AFFINITY_LIST, m);
}

#ifndef CONFIG_AUTO_IRQ_AFFINITY
static inline int irq_select_affinity_usr(unsigned int irq)
{
	/*
	 * If the interrupt is started up already then this fails. The
	 * interrupt is assigned to an online CPU already. There is no
	 * point to move it around randomly. Tell user space that the
	 * selected mask is bogus.
	 *
	 * If not then any change to the affinity is pointless because the
	 * startup code invokes irq_setup_affinity() which will select
	 * a online CPU anyway.
	 */
	return -EINVAL;
}
#else
/* ALPHA magic affinity auto selector. Keep it for historical reasons. */
static inline int irq_select_affinity_usr(unsigned int irq)
{
	return irq_select_affinity(irq);
}
#endif

static ssize_t write_irq_affinity(int type, struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned int irq = (int)(long)pde_data(file_inode(file));
	cpumask_var_t new_value;
	int err;

	if (!irq_can_set_affinity_usr(irq) || no_irq_affinity)
		return -EPERM;

	if (!zalloc_cpumask_var(&new_value, GFP_KERNEL))
		return -ENOMEM;

	if (type)
		err = cpumask_parselist_user(buffer, count, new_value);
	else
		err = cpumask_parse_user(buffer, count, new_value);
	if (err)
		goto free_cpumask;

	/*
	 * Do not allow disabling IRQs completely - it's a too easy
	 * way to make the system unusable accidentally :-) At least
	 * one online CPU still has to be targeted.
	 */
	if (!cpumask_intersects(new_value, cpu_online_mask)) {
		/*
		 * Special case for empty set - allow the architecture code
		 * to set default SMP affinity.
		 */
		err = irq_select_affinity_usr(irq) ? -EINVAL : count;
	} else {
		err = irq_set_affinity(irq, new_value);
		if (!err)
			err = count;
	}

free_cpumask:
	free_cpumask_var(new_value);
	return err;
}

static ssize_t irq_affinity_proc_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	return write_irq_affinity(0, file, buffer, count, pos);
}

static ssize_t irq_affinity_list_proc_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	return write_irq_affinity(1, file, buffer, count, pos);
}

static int irq_affinity_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_affinity_proc_show, pde_data(inode));
}

static int irq_affinity_list_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_affinity_list_proc_show, pde_data(inode));
}

static const struct proc_ops irq_affinity_proc_ops = {
	.proc_open	= irq_affinity_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= irq_affinity_proc_write,
};

static const struct proc_ops irq_affinity_list_proc_ops = {
	.proc_open	= irq_affinity_list_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= irq_affinity_list_proc_write,
};

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
static int irq_effective_aff_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(EFFECTIVE, m);
}

static int irq_effective_aff_list_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(EFFECTIVE_LIST, m);
}
#endif

static int default_affinity_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%*pb\n", cpumask_pr_args(irq_default_affinity));
	return 0;
}

static ssize_t default_affinity_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	cpumask_var_t new_value;
	int err;

	if (!zalloc_cpumask_var(&new_value, GFP_KERNEL))
		return -ENOMEM;

	err = cpumask_parse_user(buffer, count, new_value);
	if (err)
		goto out;

	/*
	 * Do not allow disabling IRQs completely - it's a too easy
	 * way to make the system unusable accidentally :-) At least
	 * one online CPU still has to be targeted.
	 */
	if (!cpumask_intersects(new_value, cpu_online_mask)) {
		err = -EINVAL;
		goto out;
	}

	cpumask_copy(irq_default_affinity, new_value);
	err = count;

out:
	free_cpumask_var(new_value);
	return err;
}

static int default_affinity_open(struct inode *inode, struct file *file)
{
	return single_open(file, default_affinity_show, pde_data(inode));
}

static const struct proc_ops default_affinity_proc_ops = {
	.proc_open	= default_affinity_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= default_affinity_write,
};

static int irq_node_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long) m->private);

	seq_printf(m, "%d\n", irq_desc_get_node(desc));
	return 0;
}
#endif

static int irq_spurious_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long) m->private);

	seq_printf(m, "count %u\n" "unhandled %u\n" "last_unhandled %u ms\n",
		   desc->irq_count, desc->irqs_unhandled,
		   jiffies_to_msecs(desc->last_unhandled));
	return 0;
}

#define MAX_NAMELEN 128

static bool name_unique(unsigned int irq, struct irqaction *new_action)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action;

	guard(raw_spinlock_irq)(&desc->lock);
	for_each_action_of_desc(desc, action) {
		if ((action != new_action) && action->name &&
		    !strcmp(new_action->name, action->name))
			return false;
	}
	return true;
}

void register_handler_proc(unsigned int irq, struct irqaction *action)
{
	char name[MAX_NAMELEN];
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc->dir || action->dir || !action->name || !name_unique(irq, action))
		return;

	strscpy(name, action->name);

	/* create /proc/irq/1234/handler/ */
	action->dir = proc_mkdir(name, desc->dir);
}

#undef MAX_NAMELEN

#define MAX_NAMELEN 10

void register_irq_proc(unsigned int irq, struct irq_desc *desc)
{
	static DEFINE_MUTEX(register_lock);
	void __maybe_unused *irqp = (void *)(unsigned long) irq;
	char name [MAX_NAMELEN];

	if (!root_irq_dir || (desc->irq_data.chip == &no_irq_chip))
		return;

	/*
	 * irq directories are registered only when a handler is
	 * added, not when the descriptor is created, so multiple
	 * tasks might try to register at the same time.
	 */
	guard(mutex)(&register_lock);

	if (desc->dir)
		return;

	/* create /proc/irq/1234 */
	sprintf(name, "%u", irq);
	desc->dir = proc_mkdir(name, root_irq_dir);
	if (!desc->dir)
		return;

#ifdef CONFIG_SMP
	umode_t umode = S_IRUGO;

	if (irq_can_set_affinity_usr(desc->irq_data.irq))
		umode |= S_IWUSR;

	/* create /proc/irq/<irq>/smp_affinity */
	proc_create_data("smp_affinity", umode, desc->dir, &irq_affinity_proc_ops, irqp);

	/* create /proc/irq/<irq>/affinity_hint */
	proc_create_single_data("affinity_hint", 0444, desc->dir,
				irq_affinity_hint_proc_show, irqp);

	/* create /proc/irq/<irq>/smp_affinity_list */
	proc_create_data("smp_affinity_list", umode, desc->dir,
			 &irq_affinity_list_proc_ops, irqp);

	proc_create_single_data("node", 0444, desc->dir, irq_node_proc_show, irqp);
# ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	proc_create_single_data("effective_affinity", 0444, desc->dir,
				irq_effective_aff_proc_show, irqp);
	proc_create_single_data("effective_affinity_list", 0444, desc->dir,
				irq_effective_aff_list_proc_show, irqp);
# endif
#endif
	proc_create_single_data("spurious", 0444, desc->dir,
				irq_spurious_proc_show, (void *)(long)irq);

}

void unregister_irq_proc(unsigned int irq, struct irq_desc *desc)
{
	char name [MAX_NAMELEN];

	if (!root_irq_dir || !desc->dir)
		return;
#ifdef CONFIG_SMP
	remove_proc_entry("smp_affinity", desc->dir);
	remove_proc_entry("affinity_hint", desc->dir);
	remove_proc_entry("smp_affinity_list", desc->dir);
	remove_proc_entry("node", desc->dir);
# ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	remove_proc_entry("effective_affinity", desc->dir);
	remove_proc_entry("effective_affinity_list", desc->dir);
# endif
#endif
	remove_proc_entry("spurious", desc->dir);

	sprintf(name, "%u", irq);
	remove_proc_entry(name, root_irq_dir);
}

#undef MAX_NAMELEN

void unregister_handler_proc(unsigned int irq, struct irqaction *action)
{
	proc_remove(action->dir);
}

static void register_default_affinity_proc(void)
{
#ifdef CONFIG_SMP
	proc_create("irq/default_smp_affinity", 0644, NULL,
		    &default_affinity_proc_ops);
#endif
}

void init_irq_proc(void)
{
	unsigned int irq;
	struct irq_desc *desc;

	/* create /proc/irq */
	root_irq_dir = proc_mkdir("irq", NULL);
	if (!root_irq_dir)
		return;

	register_default_affinity_proc();

	/*
	 * Create entries for all existing IRQs.
	 */
	for_each_irq_desc(irq, desc)
		register_irq_proc(irq, desc);
}

void irq_proc_update_valid(struct irq_desc *desc)
{
	u32 set = _IRQ_PROC_VALID;

	if (irq_settings_is_hidden(desc) || !desc->action ||
	    irq_desc_is_chained(desc) || !desc->kstat_irqs)
		set = 0;

	irq_settings_update_proc_valid(desc, set);
}

#ifdef CONFIG_GENERIC_IRQ_SHOW

#define ARCH_PROC_IRQDESC ((void *)0x00001111)

int __weak arch_show_interrupts(struct seq_file *p, int prec)
{
	return 0;
}

static int irq_num_prec __read_mostly = 3;

#ifndef ACTUAL_NR_IRQS
# define ACTUAL_NR_IRQS total_nr_irqs
#endif

void irq_proc_calc_prec(void)
{
	unsigned int prec, n;

	for (prec = 3, n = 1000; prec < 10 && n <= total_nr_irqs; ++prec)
		n *= 10;
	WRITE_ONCE(irq_num_prec, prec);
}

#define ZSTR1 "          0"
#define ZSTR1_LEN	(sizeof(ZSTR1) - 1)
#define ZSTR16		ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 \
			ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1
#define ZSTR256		ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 \
			ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16

static inline void irq_proc_emit_zero_counts(struct seq_file *p, unsigned int zeros)
{
	if (!zeros)
		return;

	for (unsigned int n = min(zeros, 256); n; zeros -= n, n = min(zeros, 256))
		seq_write(p, ZSTR256, n * ZSTR1_LEN);
}

static inline unsigned int irq_proc_emit_count(struct seq_file *p, unsigned int cnt,
					       unsigned int zeros)
{
	if (!cnt)
		return zeros + 1;

	irq_proc_emit_zero_counts(p, zeros);
	seq_put_decimal_ull_width(p, " ", cnt, 10);
	return 0;
}

void irq_proc_emit_counts(struct seq_file *p, unsigned int __percpu *cnts)
{
	unsigned int cpu, zeros = 0;

	for_each_online_cpu(cpu)
		zeros = irq_proc_emit_count(p, per_cpu(*cnts, cpu), zeros);
	irq_proc_emit_zero_counts(p, zeros);
}

static int irq_seq_show(struct seq_file *p, void *v)
{
	int prec = (int)(unsigned long)p->private;
	struct irq_desc *desc = v;
	struct irqaction *action;

	if (desc == ARCH_PROC_IRQDESC)
		return arch_show_interrupts(p, prec);

	/* print header for the first interrupt indicated by !p>private */
	if (!prec) {
		unsigned int cpu;

		prec = READ_ONCE(irq_num_prec);
		seq_printf(p, "%*s", prec + 8, "");
		for_each_online_cpu(cpu)
			seq_printf(p, "CPU%-8d", cpu);
		seq_putc(p, '\n');
		p->private = (void *)(unsigned long)prec;
	}

	seq_put_decimal_ull_width(p, "", irq_desc_get_irq(desc), prec);
	seq_putc(p, ':');

	/*
	 * Always output per CPU interrupts. Output device interrupts only when
	 * desc::tot_count is not zero.
	 */
	if (irq_settings_is_per_cpu(desc) || irq_settings_is_per_cpu_devid(desc) ||
	    data_race(desc->tot_count))
		irq_proc_emit_counts(p, &desc->kstat_irqs->cnt);
	else
		irq_proc_emit_zero_counts(p, num_online_cpus());
	seq_putc(p, ' ');

	guard(raw_spinlock_irq)(&desc->lock);
	if (desc->irq_data.chip) {
		if (desc->irq_data.chip->irq_print_chip)
			desc->irq_data.chip->irq_print_chip(&desc->irq_data, p);
		else if (desc->irq_data.chip->name)
			seq_printf(p, "%8s", desc->irq_data.chip->name);
		else
			seq_printf(p, "%8s", "-");
	} else {
		seq_printf(p, "%8s", "None");
	}

	seq_putc(p, ' ');
	if (desc->irq_data.domain)
		seq_put_decimal_ull_width(p, "", desc->irq_data.hwirq, prec);
	else
		seq_printf(p, " %*s", prec, "");

	if (IS_ENABLED(CONFIG_GENERIC_IRQ_SHOW_LEVEL))
		seq_printf(p, " %-8s", irqd_is_level_type(&desc->irq_data) ? "Level" : "Edge");

	if (desc->name)
		seq_printf(p, "-%-8s", desc->name);

	action = desc->action;
	if (action) {
		seq_printf(p, "  %s", action->name);
		while ((action = action->next) != NULL)
			seq_printf(p, ", %s", action->name);
	}

	seq_putc(p, '\n');
	return 0;
}

static void *irq_seq_next_desc(loff_t *pos)
{
	struct irq_desc *desc;

	if (*pos > total_nr_irqs)
		return NULL;

	guard(rcu)();
	for (;;) {
		desc = irq_find_desc_at_or_after((unsigned int) *pos);
		if (desc) {
			*pos = irq_desc_get_irq(desc);
			/*
			 * If valid for output try to acquire a reference count
			 * on the descriptor so that it can't be freed after
			 * dropping RCU read lock on return.
			 */
			if (irq_settings_proc_valid(desc) && irq_desc_get_ref(desc))
				return desc;
			(*pos)++;
		} else {
			*pos = total_nr_irqs;
			return ARCH_PROC_IRQDESC;
		}
	}
}

static void *irq_seq_start(struct seq_file *f, loff_t *pos)
{
	if (!*pos)
		f->private = NULL;
	return irq_seq_next_desc(pos);
}

static void *irq_seq_next(struct seq_file *f, void *v, loff_t *pos)
{
	if (v && v != ARCH_PROC_IRQDESC)
		irq_desc_put_ref(v);

	(*pos)++;
	return irq_seq_next_desc(pos);
}

static void irq_seq_stop(struct seq_file *f, void *v)
{
	if (v && v != ARCH_PROC_IRQDESC)
		irq_desc_put_ref(v);
}

static const struct seq_operations irq_seq_ops = {
	.start = irq_seq_start,
	.next  = irq_seq_next,
	.stop  = irq_seq_stop,
	.show  = irq_seq_show,
};

/*
 * /proc/irq/stats related code
 *
 * /proc/irq/stats provides variable record sized statistics for device
 * interrupts.
 */
struct irq_proc_stat {
	unsigned int			irqnr;
	bool				percpu;
	bool				first;
	size_t				from;
	size_t				count;
	loff_t				read_pos;
	struct irq_desc			*desc;
	struct irq_proc_stat_data	*data;
};

static inline bool irq_stat_valid_irq(struct irq_proc_stat *s)
{
	struct irq_desc *desc = s->desc;

	/* Check for general validity */
	if (!irq_settings_proc_valid(desc))
		return false;

	if (!s->percpu) {
		/*
		 * Device interrupts update desc::tot_count. Per CPU
		 * interrupts are not touching that fields due to the
		 * obvious concurrency issues.  For device interrupts it's
		 * therefore sufficient to evaluate desc::tot_count.
		 */
		if (!data_race(desc->tot_count))
			return false;
	} else {
		/*
		 * Per CPU interrupts are marked accordingly in the
		 * settings.
		 */
		if (!irq_settings_is_per_cpu(desc) && !irq_settings_is_per_cpu_devid(desc))
			return false;
	}

	/* Try to get a reference to prevent freeing before it's evaluated */
	return irq_desc_get_ref(desc);
}

static inline bool irq_stat_find_irq(struct irq_proc_stat *s)
{
	/* Loop until a valid interrupt is found */
	guard(rcu)();
	for (;; s->irqnr++) {
		s->desc = irq_find_desc_at_or_after(s->irqnr);
		/* NULL means there is no interrupt anymore in the maple tree */
		if (!s->desc) {
			s->irqnr = total_nr_irqs;
			return false;
		}

		/* Save the interrupt number for the next search */
		s->irqnr = irq_desc_get_irq(s->desc);

		if (irq_stat_valid_irq(s))
			return true;
	}
}

static inline void irq_stat_next_irq(struct irq_proc_stat *s)
{
	s->irqnr++;
	irq_stat_find_irq(s);
}

static void irq_dev_stat_update_one(struct irq_proc_stat *s)
{
	struct irq_proc_stat_data *d = s->data;
	struct irq_desc *desc = s->desc;
	struct irq_data *irqd;
	unsigned int cpu;

	/*
	 * Optimize for single CPU target affinities. Otherwise walk the
	 * effective affinity mask, which falls back to the real affinity
	 * mask if the architecture does not support effective affinity
	 * masks. Bad luck...
	 */
	irqd = irq_desc_get_irq_data(desc);
	cpu = irq_data_get_single_target(irqd);
	if (cpu < nr_cpu_ids) {
		struct irq_proc_stat_cpu pcpu = {
			.cpu = cpu,
			.cnt = data_race(per_cpu(desc->kstat_irqs->cnt, cpu)),
		};

		if (pcpu.cnt)
			d->pcpu[d->entries++] = pcpu;
	} else {
		const struct cpumask *m = irq_data_get_effective_affinity_mask(irqd);

		for_each_cpu(cpu, m) {
			struct irq_proc_stat_cpu pcpu = {
				.cpu = cpu,
				.cnt = data_race(per_cpu(desc->kstat_irqs->cnt, cpu)),
			};

			if (pcpu.cnt)
				d->pcpu[d->entries++] = pcpu;
		}
	}
}

static void irq_percpu_stat_update_one(struct irq_proc_stat *s)
{
	struct irq_proc_stat_data *d = s->data;
	struct irq_desc *desc = s->desc;
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		struct irq_proc_stat_cpu pcpu = {
			.cpu = cpu,
			.cnt = data_race(per_cpu(desc->kstat_irqs->cnt, cpu)),
		};

		if (pcpu.cnt)
			d->pcpu[d->entries++] = pcpu;
	}
}

static bool irq_stat_update_one(struct irq_proc_stat *s)
{
	struct irq_proc_stat_data *d = s->data;

	if (IS_ENABLED(CONFIG_GENERIC_IRQ_PERCPU_STATS) && s->percpu)
		irq_percpu_stat_update_one(s);
	else
		irq_dev_stat_update_one(s);

	/* Only output data if there is an actual count */
	if (d->entries) {
		d->irqnr = s->irqnr;
		s->count = sizeof(*d) + d->entries * sizeof(*d->pcpu);
	}

	/* Drop the reference count which got acquired in irq_stat_find_irq() */
	irq_desc_put_ref(s->desc);
	s->desc = NULL;
	return !!s->count;
}

static __always_inline bool irq_stat_next_data(struct irq_proc_stat *s)
{
	/*
	 * On the first read or after a lseek(fd, 0, SEEK_SET) find the
	 * first interrupt. Otherwise find the next one.
	 */
	if (unlikely(s->first)) {
		s->irqnr = 0;
		s->first = false;
		irq_stat_find_irq(s);
	} else {
		irq_stat_next_irq(s);
	}

	/* Repeat until an interrupt with non-zero counts is found */
	for (; s->desc; irq_stat_next_irq(s)) {
		if (irq_stat_update_one(s))
			return true;
	}
	return false;
}

static size_t irq_stat_copy_to_iter(struct irq_proc_stat *s, struct iov_iter *iter)
{
	size_t n = copy_to_iter(((char *)s->data) + s->from, s->count, iter);

	s->count -= n;
	s->from += n;
	return n;
}

/* Force inline as otherwise next() becomes a indirect call */
static __always_inline ssize_t __irq_stats_read(struct kiocb *iocb, struct iov_iter *iter,
						bool (*next)(struct irq_proc_stat *))
{
	struct irq_proc_stat *s = iocb->ki_filp->private_data;
	size_t copied = 0;

	/* Real seek is not supported. See irq_stat_lseek() */
	if (WARN_ON_ONCE(iocb->ki_pos != s->read_pos))
		goto done;

	if (s->count)
		copied += irq_stat_copy_to_iter(s, iter);

	for (; !s->count;) {
		s->count = s->from = 0;
		s->data->entries = 0;

		if (!next(s))
			goto done;
		copied += irq_stat_copy_to_iter(s, iter);
	}

	if (!copied)
		return -EFAULT;
done:
	iocb->ki_pos += copied;
	s->read_pos += copied;
	return copied;
}

static ssize_t irq_stats_read(struct kiocb *iocb, struct iov_iter *iter)
{
	return __irq_stats_read(iocb, iter, irq_stat_next_data);
}

static loff_t irq_stats_llseek(struct file *filp, loff_t offset, int whence)
{
	struct irq_proc_stat *s = filp->private_data;
	loff_t ret;

	/*
	 * As this is a variable record interface and the actual use case is to
	 * get a full snapshot of the active interrupts, there is no point in
	 * trying to be fully seekable. Just support rewind to the beginning of
	 * the data set. For all other operations return the current position
	 * which makes e.g. python happy.
	 */
	if (whence != SEEK_SET || offset)
		return noop_llseek(filp, offset, whence);

	ret = default_llseek(filp, 0, SEEK_SET);
	if (ret < 0)
		return ret;

	/* Reset the position, drop any leftovers and indicate to start over */
	s->read_pos = 0;
	s->count = 0;
	s->first = true;
	return 0;
}

static int __irq_stats_open(struct inode *inode, struct file *filp, bool percpu)
{
	struct irq_proc_stat *s = kzalloc_obj(*s);

	if (!s)
		return -ENOMEM;

	s->data	= kzalloc_flex(*s->data, pcpu, num_possible_cpus());
	if (!s->data) {
		kfree(s);
		return -ENOMEM;
	}

	s->first = true;
	s->percpu = percpu;
	filp->private_data = s;
	return 0;
}

static int irq_stats_open(struct inode *inode, struct file *filp)
{
	return __irq_stats_open(inode, filp, false);
}

static int irq_stats_release(struct inode *inode, struct file *filp)
{
	struct irq_proc_stat *s = filp->private_data;

	if (s) {
		kfree(s->data);
		kfree(s);
	}
	return 0;
}

static const struct proc_ops irq_dev_stat_ops = {
	.proc_flags	= PROC_ENTRY_PERMANENT,
	.proc_open	= irq_stats_open,
	.proc_release	= irq_stats_release,
	.proc_read_iter	= irq_stats_read,
	.proc_lseek	= irq_stats_llseek,
};

#ifdef CONFIG_GENERIC_IRQ_STATS_PERCPU
static int irq_pcp_stats_open(struct inode *inode, struct file *filp)
{
	return __irq_stats_open(inode, filp, true);
}

static const struct proc_ops irq_pcp_stat_ops = {
	.proc_flags	= PROC_ENTRY_PERMANENT,
	.proc_open	= irq_pcp_stats_open,
	.proc_release	= irq_stats_release,
	.proc_read_iter	= irq_stats_read,
	.proc_lseek	= irq_stats_llseek,
};

static __init void irq_pcp_stats_init(void)
{
	proc_create("percpu_stats", 0, root_irq_dir, &irq_pcp_stat_ops);
}
#else  /* CONFIG_GENERIC_IRQ_STATS_PERCPU */
static inline void irq_pcp_stats_init(void) { }
#endif /* !CONFIG_GENERIC_IRQ_STATS_PERCPU */

#ifdef CONFIG_GENERIC_IRQ_STATS_ARCH
static inline void arch_stat_update_one(struct irq_proc_stat *s)
{
	struct irq_proc_stat_data *d = s->data;
	unsigned int cpu, idx = s->irqnr;

	for_each_online_cpu(cpu) {
		struct irq_proc_stat_cpu pcpu = {
			.cpu = cpu,
			.cnt = arch_get_irq_stat(cpu, idx),
		};

		if (pcpu.cnt)
			d->pcpu[d->entries++] = pcpu;
	}

	if (d->entries) {
		d->irqnr = idx;
		s->count = sizeof(*d) + d->entries * sizeof(*d->pcpu);
	}
}

static __always_inline bool arch_stat_next_data(struct irq_proc_stat *s)
{
	if (unlikely(s->first)) {
		s->irqnr = 0;
		s->first = false;
	}

	for(; !s->count && s->irqnr < ARCH_IRQ_STATS_NUM_IRQS; s->irqnr++)
		arch_stat_update_one(s);
	return !!s->count;
}

static ssize_t irq_arch_stats_read(struct kiocb *iocb, struct iov_iter *iter)
{
	return __irq_stats_read(iocb, iter, arch_stat_next_data);
}

static const struct proc_ops irq_arch_stat_ops = {
	.proc_flags	= PROC_ENTRY_PERMANENT,
	.proc_open	= irq_stats_open,
	.proc_release	= irq_stats_release,
	.proc_read_iter	= irq_arch_stats_read,
	.proc_lseek	= irq_stats_llseek,
};

static __init void irq_arch_stats_init(void)
{
	proc_create("arch_stats", 0, root_irq_dir, &irq_arch_stat_ops);
}
#else  /* CONFIG_GENERIC_IRQ_STATS_ARCH */
static inline void irq_arch_stats_init(void) { }
#endif /* !CONFIG_GENERIC_IRQ_STATS_ARCH */

static int __init irq_proc_init(void)
{
	proc_create_seq("interrupts", 0, NULL, &irq_seq_ops);
	if (!root_irq_dir)
		return 0;

	proc_create("device_stats", 0, root_irq_dir, &irq_dev_stat_ops);
	irq_pcp_stats_init();
	irq_arch_stats_init();
	return 0;
}
fs_initcall(irq_proc_init);

#endif
