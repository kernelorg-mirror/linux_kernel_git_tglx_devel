/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_IDLE_H
#define _ASM_X86_IDLE_H

void default_idle(void);
#ifdef	CONFIG_XEN
bool xen_set_default_idle(void);
#else
#define xen_set_default_idle 0
#endif

#endif /* _ASM_X86_IDLE_H */
