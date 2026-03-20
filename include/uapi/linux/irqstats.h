/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef LINUX_UAPI_IRQSTATS_H
#define LINUX_UAPI_IRQSTATS_H

/**
 * irq_proc_stat_cpu - Data record for /proc/irq/stats
 * @cpu:	The CPU associated to @cnt
 * @cnt:	The count assiciated to @cpu
 */
struct irq_proc_stat_cpu {
	unsigned int	cpu;
	unsigned int	cnt;
};

/**
 * irq_proc_stat_data - Data header for /proc/irq/stats
 * @irqnr:	The interrupt number
 * @entries:	The number of records (max. nr_cpu_ids)
 * @pcpu:	Runtime sized array of per CPU stat records
 */
struct irq_proc_stat_data {
	unsigned int			irqnr;
	unsigned int			entries;
	struct irq_proc_stat_cpu	pcpu[];
};

#endif
