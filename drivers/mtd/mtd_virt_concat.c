// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtual concat MTD device driver
 *
 * Copyright (C) 2018 Bernhard Frauendienst
 * Author: Bernhard Frauendienst <kernel@nospam.obeliks.de>
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/mtd/mtd.h>
#include "mtdcore.h"
#include <linux/mtd/partitions.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/mtd/concat.h>

#define CONCAT_PROP "part-concat"
#define MIN_DEV_PER_CONCAT 2

static LIST_HEAD(concat_node_list);

/**
 * struct mtd_virt_concat_node - components of a concatenation
 * @head: List handle
 * @count: Number of nodes
 * @nodes: Pointer to the nodes (partitions) to concatenate
 * @concat: Concatenation container
 */
struct mtd_virt_concat_node {
	struct list_head head;
	unsigned int count;
	struct device_node **nodes;
	struct mtd_concat *concat;
};

static void mtd_virt_concat_put_mtd_devices(struct mtd_concat *concat)
{
	int i;

	for (i = 0; i < concat->num_subdev; i++)
		put_mtd_device(concat->subdev[i]);
}

static void mtd_virt_concat_destroy_joins(void)
{
	struct mtd_virt_concat_node *item, *tmp;
	struct mtd_info *mtd;

	list_for_each_entry_safe(item, tmp, &concat_node_list, head) {
		mtd = &item->concat->mtd;
		if (item->concat) {
			mtd_device_unregister(mtd);
			kfree(mtd->name);
			mtd_concat_destroy(mtd);
			mtd_virt_concat_put_mtd_devices(item->concat);
		}
	}
}

static int mtd_virt_concat_create_item(struct device_node *parts,
				       unsigned int count)
{
	struct mtd_virt_concat_node *item;
	struct mtd_concat *concat;
	int i;

	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;

	item->count = count;
	item->nodes = kcalloc(count, sizeof(*item->nodes), GFP_KERNEL);
	if (!item->nodes) {
		kfree(item);
		return -ENOMEM;
	}

	for (i = 0; i < count; i++)
		item->nodes[i] = of_parse_phandle(parts, CONCAT_PROP, i);

	concat = kzalloc(sizeof(*concat), GFP_KERNEL);
	if (!concat) {
		kfree(item);
		return -ENOMEM;
	}

	concat->subdev = kcalloc(count, sizeof(*concat->subdev), GFP_KERNEL);
	if (!concat->subdev) {
		kfree(item);
		kfree(concat);
		return -ENOMEM;
	}
	item->concat = concat;

	list_add_tail(&item->head, &concat_node_list);

	return 0;
}

static void mtd_virt_concat_destroy_items(void)
{
	struct mtd_virt_concat_node *item, *temp;
	int i;

	list_for_each_entry_safe(item, temp, &concat_node_list, head) {
		for (i = 0; i < item->count; i++)
			of_node_put(item->nodes[i]);

		kfree(item->nodes);
		kfree(item);
	}
}

/**
 * mtd_virt_concat_add - add mtd_info object to the list of subdevices for concatenation
 * @mtd: pointer to new MTD device info structure
 *
 * Returns true if the mtd_info object is added successfully else returns false.
 *
 * The mtd_info object is added to the list of subdevices for concatenation.
 * It returns true if a match is found, and false if all subdevices have
 * already been added or if the mtd_info object does not match any of the
 * intended MTD devices.
 */
bool mtd_virt_concat_add(struct mtd_info *mtd)
{
	struct mtd_virt_concat_node *item;
	struct mtd_concat *concat;
	int idx;

	list_for_each_entry(item, &concat_node_list, head) {
		concat = item->concat;
		if (item->count == concat->num_subdev)
			return false;

		for (idx = 0; idx < item->count; idx++) {
			if (item->nodes[idx] == mtd->dev.of_node) {
				concat->subdev[concat->num_subdev++] = mtd;
				return true;
			}
		}
	}
	return false;
}
EXPORT_SYMBOL_GPL(mtd_virt_concat_add);

/**
 * mtd_virt_concat_node_create - Create a component for concatenation
 *
 * Returns a positive number representing the no. of devices found for
 * concatenation, or a negative error code.
 *
 * List all the devices for concatenations found in DT and create a
 * component for concatenation.
 */
int mtd_virt_concat_node_create(void)
{
	struct mtd_concat *concat;
	struct device_node *parts = NULL;
	int ret = 0, count;

	if (!list_empty(&concat_node_list))
		return 0;

	/* List all the concatenations found in DT */
	do {
		parts = of_find_node_with_property(parts, CONCAT_PROP);
		if (!of_device_is_available(parts))
			continue;

		count = of_count_phandle_with_args(parts, CONCAT_PROP, NULL);
		if (count < MIN_DEV_PER_CONCAT)
			continue;

		ret = mtd_virt_concat_create_item(parts, count);
		if (ret) {
			of_node_put(parts);
			goto destroy_items;
		}
	} while (parts);

	concat = kzalloc(sizeof(*concat), GFP_KERNEL);
	if (!concat) {
		ret = -ENOMEM;
		of_node_put(parts);
		goto destroy_items;
	}

	concat->subdev = kcalloc(count, sizeof(*concat->subdev), GFP_KERNEL);
	if (!concat->subdev) {
		kfree(concat);
		ret = -ENOMEM;
		of_node_put(parts);
		goto destroy_items;
	}

	return count;

destroy_items:
	mtd_virt_concat_destroy_items();

	return ret;
}
EXPORT_SYMBOL_GPL(mtd_virt_concat_node_create);

static int __init mtd_virt_concat_create_join(void)
{
	struct mtd_virt_concat_node *item;
	struct mtd_concat *concat;
	struct mtd_info *mtd;
	ssize_t name_sz;
	char *name;
	int ret;

	list_for_each_entry(item, &concat_node_list, head) {
		concat = item->concat;
		mtd = &concat->mtd;
		/* Create the virtual device */
		name_sz = snprintf(NULL, 0, "%s-%s%s-concat",
				   concat->subdev[0]->name,
				   concat->subdev[1]->name,
				   concat->num_subdev > MIN_DEV_PER_CONCAT ?
				   "-+" : "");
		name = kmalloc(name_sz + 1, GFP_KERNEL);
		if (!name) {
			mtd_virt_concat_put_mtd_devices(concat);
			return -ENOMEM;
		}

		sprintf(name, "%s-%s%s-concat",
			concat->subdev[0]->name,
			concat->subdev[1]->name,
			concat->num_subdev > MIN_DEV_PER_CONCAT ?
			"-+" : "");

		mtd = mtd_concat_create(concat->subdev, concat->num_subdev, name);
		if (!mtd) {
			kfree(name);
			return -ENXIO;
		}

		/* Arbitrary set the first device as parent */
		mtd->dev.parent = concat->subdev[0]->dev.parent;
		mtd->dev = concat->subdev[0]->dev;

		/* Register the platform device */
		ret = mtd_device_register(mtd, NULL, 0);
		if (ret)
			goto destroy_concat;
	}

	return 0;

destroy_concat:
	mtd_concat_destroy(mtd);

	return ret;
}

late_initcall(mtd_virt_concat_create_join);

static void __exit mtd_virt_concat_exit(void)
{
	mtd_virt_concat_destroy_joins();
	mtd_virt_concat_destroy_items();
}
module_exit(mtd_virt_concat_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bernhard Frauendienst <kernel@nospam.obeliks.de>");
MODULE_DESCRIPTION("Virtual concat MTD device driver");
