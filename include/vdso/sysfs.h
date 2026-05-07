/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_SYSFS_H
#define __VDSO_SYSFS_H

int vdso_sysfs_init_image(const char *name, void *addr, unsigned int size);

#endif	/* __VDSO_SYSFS_H */
