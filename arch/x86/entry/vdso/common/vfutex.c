// SPDX-License-Identifier: GPL-2.0-only
#include <vdso/futex.h>

/*
 * Compat enabled kernels have to take the size bit into account to support the
 * mixed size use case of gaming emulators. Contrary to the kernel robust unlock
 * mechanism all of this does not test for the 32-bit modifier in 32-bit VDSOs
 * and in compat disabled kernels. User space can keep the pieces.
 */
#if defined(CONFIG_X86_64) && !defined(BUILD_VDSO32_64)

#ifdef CONFIG_COMPAT

# define ASM_CLEAR_PTR								\
		"	testl	$1, (%[pop])				\n"	\
		"	jz	.Lop64					\n"	\
		"	movl	$0, (%[pad])				\n"	\
		"	jmp	__vdso_futex_robust_try_unlock_cs_end	\n"	\
		".Lop64:						\n"	\
		"	movq	$0, (%[pad])				\n"

# define ASM_PAD_CONSTRAINT	,[pad] "S" (((unsigned long)pop) & ~0x1UL)

#else /* CONFIG_COMPAT */

# define ASM_CLEAR_PTR								\
		"	movq	$0, (%[pop])				\n"

# define ASM_PAD_CONSTRAINT

#endif /* !CONFIG_COMPAT */

#else /* CONFIG_X86_64 && !BUILD_VDSO32_64 */

# define ASM_CLEAR_PTR								\
		"	movl	$0, (%[pad])				\n"

# define ASM_PAD_CONSTRAINT	,[pad] "S" (((unsigned long)pop) & ~0x1UL)

#endif /* !CONFIG_X86_64 || BUILD_VDSO32_64 */

uint32_t __vdso_futex_robust_try_unlock(uint32_t *lock, uint32_t tid, void *pop)
{
	asm volatile (
		".global __vdso_futex_robust_try_unlock_cs_start		\n"
		".global __vdso_futex_robust_try_unlock_cs_success		\n"
		".global __vdso_futex_robust_try_unlock_cs_end			\n"
		"								\n"
		"       lock cmpxchgl	%[val], (%[ptr])			\n"
		"								\n"
		"__vdso_futex_robust_try_unlock_cs_start:			\n"
		"								\n"
		"	jnz		__vdso_futex_robust_try_unlock_cs_end	\n"
		"								\n"
		"__vdso_futex_robust_try_unlock_cs_success:			\n"
		"								\n"
			ASM_CLEAR_PTR
		"								\n"
		"__vdso_futex_robust_try_unlock_cs_end:				\n"
		: [tid] "+a" (tid)
		: [ptr] "D"  (lock),
		  [pop] "d" (pop),
		  [val] "r"  (0)
		  ASM_PAD_CONSTRAINT
		: "memory"
	);

	return tid;
}

uint32_t futex_robust_try_unlock(uint32_t *, uint32_t, void **)
	__attribute__((weak, alias("__vdso_futex_robust_try_unlock")));
