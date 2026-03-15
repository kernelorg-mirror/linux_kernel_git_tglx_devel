/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_FUTEX_ROBUST_H
#define _ASM_X86_FUTEX_ROBUST_H

#include <asm/ptrace.h>

static __always_inline bool x86_futex_needs_robust_unlock_fixup(struct pt_regs *regs)
{
	/*
	 * This is tricky in the compat case as it has to take the size check
	 * into account. See the ASM magic in the VDSO vfutex code. If compat is
	 * disabled or this is a 32-bit kernel then ZF is authoritive no matter
	 * what.
	 */
	if (!IS_ENABLED(CONFIG_X86_64) || !IS_ENABLED(CONFIG_IA32_EMULATION))
		return !!(regs->flags & X86_EFLAGS_ZF);

	/*
	 * For the compat case, the core code already established that regs->ip
	 * is >= cs_start and < cs_end. Now check whether it is at the
	 * conditional jump which checks the cmpxchg() or if it succeeded and
	 * does the size check, which obviously modifies ZF too.
	 */
	if (regs->ip >= current->mm->futex.unlock_cs_success_ip)
		return true;
	/*
	 * It's at the jnz right after the cmpxchg(). ZF tells whether this
	 * succeeded or not.
	 */
	return !!(regs->flags & X86_EFLAGS_ZF);
}

#define arch_futex_needs_robust_unlock_fixup(regs)		\
	x86_futex_needs_robust_unlock_fixup(regs)

static __always_inline void __user *x86_futex_robust_unlock_get_pop(struct pt_regs *regs)
{
	return (void __user *)regs->dx;
}

#define arch_futex_robust_unlock_get_pop(regs)			\
	x86_futex_robust_unlock_get_pop(regs)

#endif /* _ASM_X86_FUTEX_ROBUST_H */
