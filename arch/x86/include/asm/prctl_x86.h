/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PRCTL_X86_H
#define _ASM_X86_PRCTL_X86_H

long do_arch_prctl_64(struct task_struct *task, int option, unsigned long arg2);

#endif /* _ASM_X86_PRCTL_X86_H */
