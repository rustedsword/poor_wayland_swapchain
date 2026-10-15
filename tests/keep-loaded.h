/* SPDX-License-Identifier: MIT */
#ifndef KEEP_LOADED_H
#define KEEP_LOADED_H

#include <dlfcn.h>
#include <link.h>
#include <poor_array.h>

static const char *keep_loaded_names[256];
static unsigned keep_loaded_count;

static int keep_loaded_collect(struct dl_phdr_info *info, size_t, void *)
{
	if (info->dlpi_name[0] && keep_loaded_count < ARRAY_SIZE(keep_loaded_names))
		keep_loaded_names[keep_loaded_count++] = info->dlpi_name;
	return 0;
}

/* Mesa's drivers are unloaded with their last user, and what their globals point to would look leaked */
static void keep_loaded(void)
{
	keep_loaded_count = 0;
	dl_iterate_phdr(keep_loaded_collect, nullptr);
	foreach_array_const_ref(arrview_first(keep_loaded_count, keep_loaded_names), name)
		dlopen(*name, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
}

#endif
