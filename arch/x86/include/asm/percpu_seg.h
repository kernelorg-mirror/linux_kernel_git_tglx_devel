/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PERCPU_SEG_H
#define _ASM_X86_PERCPU_SEG_H

#ifdef CONFIG_X86_64
# define __percpu_seg		gs
# define __percpu_rel		(%rip)
#else
# define __percpu_seg		fs
# define __percpu_rel
#endif

#endif
