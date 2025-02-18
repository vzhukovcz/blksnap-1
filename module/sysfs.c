// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME "-sysfs: " fmt
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#ifdef STANDALONE_BDEVFILTER
#include "blk_snap.h"
#else
#include <linux/blk_snap.h>
#endif
#include "memory_checker.h"
#include "sysfs.h"
#include "ctrl.h"
#ifdef STANDALONE_BDEVFILTER
#include "log.h"
#endif

static ssize_t major_show(struct class *class, struct class_attribute *attr,
			  char *buf)
{
	sprintf(buf, "%d", get_blk_snap_major());
	return strlen(buf);
}

/* Declare class_attr_major */
CLASS_ATTR_RO(major);

static struct class *blk_snap_class;

static struct device *blk_snap_device;

int sysfs_initialize(void)
{
	struct device *dev;
	int res;

	blk_snap_class = class_create(THIS_MODULE, THIS_MODULE->name);
	if (IS_ERR(blk_snap_class)) {
		res = PTR_ERR(blk_snap_class);

		pr_err("Bad class create. errno=%d\n", abs(res));
		return res;
	}

	pr_info("Create 'major' sysfs attribute\n");
	res = class_create_file(blk_snap_class, &class_attr_major);
	if (res) {
		pr_err("Failed to create 'major' sysfs file\n");

		class_destroy(blk_snap_class);
		blk_snap_class = NULL;
		return res;
	}

	dev = device_create(blk_snap_class, NULL,
			    MKDEV(get_blk_snap_major(), 0), NULL,
			    THIS_MODULE->name);
	if (IS_ERR(dev)) {
		res = PTR_ERR(dev);
		pr_err("Failed to create device, errno=%d\n", abs(res));

		class_remove_file(blk_snap_class, &class_attr_major);
		class_destroy(blk_snap_class);
		blk_snap_class = NULL;
		return res;
	}

	blk_snap_device = dev;
	return res;
}

void sysfs_finalize(void)
{
	pr_info("Cleanup sysfs\n");

	if (blk_snap_device) {
		device_unregister(blk_snap_device);
		blk_snap_device = NULL;
	}

	if (blk_snap_class != NULL) {
		class_remove_file(blk_snap_class, &class_attr_major);
		class_destroy(blk_snap_class);
		blk_snap_class = NULL;
	}
}
