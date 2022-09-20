/*
 * lsfd-sock-xinfo.c - read various information from files under /proc/net/
 *
 * Copyright (C) 2022 Red Hat, Inc. All rights reserved.
 * Written by Masatake YAMATO <yamato@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <sched.h>		/* for setns(2) */
#include <search.h>

#include "xalloc.h"
#include "nls.h"
#include "libsmartcols.h"

#include "lsfd.h"
#include "lsfd-sock.h"

static int self_netns_fd = -1;
struct stat self_netns_sb;

static void *xinfo_tree;	/* for tsearch/tfind */
static void *netns_tree;

static int netns_compare(const void *a, const void *b)
{
	if (*(ino_t *)a < *(ino_t *)b)
		return -1;
	else if (*(ino_t *)a > *(ino_t *)b)
		return 1;
	else
		return 0;
}

static bool is_sock_xinfo_loaded(ino_t netns)
{
	return tfind(&netns, &netns_tree, netns_compare)? true: false;
}

static void mark_sock_xinfo_loaded(ino_t ino)
{
	ino_t *netns = xmalloc(sizeof(ino));
	ino_t **tmp;

	*netns = ino;
	tmp = tsearch(netns, &netns_tree, netns_compare);
	if (tmp == NULL)
		errx(EXIT_FAILURE, _("failed to allocate memory"));
}

static void load_sock_xinfo_no_nsswitch(ino_t netns __attribute__((__unused__)))
{
	/* TODO: load files under /proc/ns */
}

static void load_sock_xinfo_with_fd(int fd, ino_t netns)
{
	if (setns (fd, CLONE_NEWNET) == 0) {
		load_sock_xinfo_no_nsswitch(netns);
		setns (self_netns_fd, CLONE_NEWNET);
	}
}

void load_sock_xinfo(struct path_cxt *pc, const char *name, ino_t netns)
{
	if (self_netns_fd == -1)
		return;

	if (!is_sock_xinfo_loaded(netns)) {
		int fd;

		mark_sock_xinfo_loaded(netns);
		fd = ul_path_open(pc, O_RDONLY, name);
		if (fd < 0)
			return;

		load_sock_xinfo_with_fd(fd, netns);
		close(fd);
	}
}

void initialize_sock_xinfos(void)
{
	struct path_cxt *pc;
	DIR *dir;
	struct dirent *d;

	self_netns_fd = open("/proc/self/ns/net", O_RDONLY);

	if (self_netns_fd < 0)
		load_sock_xinfo_no_nsswitch(0);
	else {
		if (fstat(self_netns_fd, &self_netns_sb) == 0) {
			mark_sock_xinfo_loaded(self_netns_sb.st_ino);
			load_sock_xinfo_no_nsswitch(self_netns_sb.st_ino);
		}
	}

	/* Load /proc/net/{unix,...} of the network namespace
	 * specified with netns files under /var/run/netns/.
	 *
	 * `ip netns' command pins a network namespace on
	 * /var/run/netns.
	 */
	pc = ul_new_path("/var/run/netns");
	if (!pc)
		err(EXIT_FAILURE, _("failed to alloc path context for /var/run/netns"));
	dir = ul_path_opendir(pc, NULL);
	if (dir == NULL) {
		ul_unref_path(pc);
		return;
	}
	while ((d = readdir(dir))) {
		struct stat sb;
		int fd;
		if (ul_path_stat(pc, &sb, 0, d->d_name) < 0)
			continue;
		if (is_sock_xinfo_loaded(sb.st_ino))
			continue;
		mark_sock_xinfo_loaded(sb.st_ino);
		fd = ul_path_open(pc, O_RDONLY, d->d_name);
		if (fd < 0)
			continue;
		load_sock_xinfo_with_fd(fd, sb.st_ino);
		close(fd);
	}
	closedir(dir);
	ul_unref_path(pc);
}

static void free_sock_xinfo (void *node)
{
	struct sock_xinfo *xinfo = node;
	if (xinfo->class->free)
		xinfo->class->free (xinfo);
	free(node);
}

void finalize_sock_xinfos(void)
{
	if (self_netns_fd != -1)
		close(self_netns_fd);
	tdestroy(netns_tree, free);
	tdestroy(xinfo_tree, free_sock_xinfo);
}

static int xinfo_compare(const void *a, const void *b)
{
	if (((struct sock_xinfo *)a)->inode < ((struct sock_xinfo *)b)->inode)
		return -1;
	if (((struct sock_xinfo *)a)->inode > ((struct sock_xinfo *)b)->inode)
		return 1;
	return 0;
}

struct sock_xinfo *get_sock_xinfo(ino_t netns_inode)
{
	struct sock_xinfo **xinfo = tfind(&netns_inode, &xinfo_tree, xinfo_compare);

	if (xinfo)
		return *xinfo;
	return NULL;
}

bool is_nsfs_dev(dev_t dev)
{
	return (dev == self_netns_sb.st_dev);
}
