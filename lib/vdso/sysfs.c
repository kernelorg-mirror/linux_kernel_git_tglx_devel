// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <vdso/sysfs.h>

static struct kobject *vdso_kobj __ro_after_init;
static DEFINE_MUTEX(sysfs_mutex);

int __init vdso_sysfs_init_image(const char *name, void *addr, unsigned int size)
{
	struct bin_attribute *attr = NULL;
	int ret = -ENOMEM;

	guard(mutex)(&sysfs_mutex);
	if (!vdso_kobj) {
		vdso_kobj = kobject_create_and_add("vdso", kernel_kobj);
		if (!vdso_kobj)
			return -ENOMEM;
	}

	attr = kzalloc_obj(*attr);
	if (!attr)
		goto out;

	sysfs_bin_attr_init(attr);
	attr->attr.name = name;
	attr->attr.mode = 0444;
	attr->private = addr;
	attr->size = size;
	attr->read = sysfs_bin_attr_simple_read;

	ret = sysfs_create_bin_file(vdso_kobj, attr);
	if (ret) {
		pr_warn("Failed to register %s in sysfs: %d\n", name, ret);
		goto out;
	}
	return 0;
out:
	kobject_put(vdso_kobj);
	kfree(attr);
	return ret;
}
