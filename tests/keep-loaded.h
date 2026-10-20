/* SPDX-License-Identifier: MIT */
#ifndef KEEP_LOADED_H
#define KEEP_LOADED_H

#include <dlfcn.h>
#include <link.h>
#include <poor_array.h>

struct keep_loaded_list {
	const char *names[256];
	unsigned count;
};

static int keep_loaded_collect(struct dl_phdr_info *info, size_t, void *data)
{
	struct keep_loaded_list *list = data;

	if (info->dlpi_name[0] && list->count < ARRAY_SIZE(list->names))
		list->names[list->count++] = info->dlpi_name;
	return 0;
}

/* Mesa's drivers are unloaded with their last user, and what their globals point to would look leaked */
static void keep_loaded(void)
{
	struct keep_loaded_list list;

	list.count = 0;
	dl_iterate_phdr(keep_loaded_collect, &list);
	foreach_array_const_ref(arrview_first(list.count, list.names), name)
		dlopen(*name, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
}

#endif
