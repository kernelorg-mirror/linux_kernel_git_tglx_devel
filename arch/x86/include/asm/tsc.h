/* SPDX-License-Identifier: GPL-2.0 */
/*
 * x86 TSC related functions
 */
#ifndef _ASM_X86_TSC_H
#define _ASM_X86_TSC_H

#include <asm/asm.h>
#include <asm/cpufeature.h>
#include <asm/rdtsc.h>

/*
 * Standard way to access the cycle counter.
 */
extern unsigned int cpu_khz;
extern unsigned int tsc_khz;

extern void disable_TSC(void);

static inline cycles_t get_cycles(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_TSC))
		return 0;
	return rdtsc();
}
#define get_cycles get_cycles

extern void tsc_early_init(void);
extern void tsc_init(void);
extern void mark_tsc_unstable(char *reason);
extern int unsynchronized_tsc(void);
extern int check_tsc_unstable(void);
extern void mark_tsc_async_resets(char *reason);
extern unsigned long native_calibrate_cpu_early(void);
extern unsigned long native_calibrate_tsc(void);
extern unsigned long long native_sched_clock_from_tsc(u64 tsc);

extern int tsc_clocksource_reliable;
extern bool tsc_async_resets;

/*
 * Boot-time check whether the TSCs are synchronized across
 * all CPUs/cores:
 */
extern bool tsc_store_and_check_tsc_adjust(bool bootcpu);
extern void tsc_verify_tsc_adjust(bool resume);
extern void check_tsc_sync_target(void);

extern int notsc_setup(char *);
extern void tsc_save_sched_clock_state(void);
extern void tsc_restore_sched_clock_state(void);

unsigned long cpu_khz_from_msr(void);

#endif /* _ASM_X86_TSC_H */
