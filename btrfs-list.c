/*
 * Copyright (C) 2010 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include "kerncompat.h"
#include "ctree.h"
#include "transaction.h"
#include "utils.h"
#include "version.h"

/* we store all the roots we find in an rbtree so that we can
 * search for them later.
 */
struct root_lookup {
	struct rb_root root;
};

/*
 * one of these for each root we find.
 */
struct root_info {
	struct rb_node rb_node;

	/* this root's id */
	u64 root_id;

	/* the id of the root that references this one */
	u64 ref_tree;

	/* the dir id we're in from ref_tree */
	u64 dir_id;

	/* path from the subvol we live in to this root, including the
	 * root's name.  This is null until we do the extra lookup ioctl.
	 */
	char *path;

	/* the name of this root in the directory it lives in */
	char name[];
};

static void root_lookup_init(struct root_lookup *tree)
{
	tree->root.rb_node = NULL;
}

static int comp_entry(struct root_info *entry, u64 root_id, u64 ref_tree)
{
	if (entry->root_id > root_id)
		return 1;
	if (entry->root_id < root_id)
		return -1;
	if (entry->ref_tree > ref_tree)
		return 1;
	if (entry->ref_tree < ref_tree)
		return -1;
	return 0;
}

/*
 * insert a new root into the tree.  returns the existing root entry
 * if one is already there.  Both root_id and ref_tree are used
 * as the key
 */
static struct rb_node *tree_insert(struct rb_root *root, u64 root_id,
				   u64 ref_tree, struct rb_node *node)
{
	struct rb_node ** p = &root->rb_node;
	struct rb_node * parent = NULL;
	struct root_info *entry;
	int comp;

	while(*p) {
		parent = *p;
		entry = rb_entry(parent, struct root_info, rb_node);

		comp = comp_entry(entry, root_id, ref_tree);

		if (comp < 0)
			p = &(*p)->rb_left;
		else if (comp > 0)
			p = &(*p)->rb_right;
		else
			return parent;
	}

	entry = rb_entry(parent, struct root_info, rb_node);
	rb_link_node(node, parent, p);
	rb_insert_color(node, root);
	return NULL;
}

/*
 * find a given root id in the tree.  We return the smallest one,
 * rb_next can be used to move forward looking for more if required
 */
static struct root_info *tree_search(struct rb_root *root, u64 root_id)
{
	struct rb_node * n = root->rb_node;
	struct root_info *entry;

	while(n) {
		entry = rb_entry(n, struct root_info, rb_node);

		if (entry->root_id < root_id)
			n = n->rb_left;
		else if (entry->root_id > root_id)
			n = n->rb_right;
		else {
			struct root_info *prev;
			struct rb_node *prev_n;
			while (1) {
				prev_n = rb_prev(n);
				if (!prev_n)
					break;
				prev = rb_entry(prev_n, struct root_info,
						      rb_node);
				if (prev->root_id != root_id)
					break;
				entry = prev;
				n = prev_n;
			}
			return entry;
		}
	}
	return NULL;
}

/*
 * this allocates a new root in the lookup tree.
 *
 * root_id should be the object id of the root
 *
 * ref_tree is the objectid of the referring root.
 *
 * dir_id is the directory in ref_tree where this root_id can be found.
 *
 * name is the name of root_id in that directory
 *
 * name_len is the length of name
 */
static int add_root(struct root_lookup *root_lookup,
		    u64 root_id, u64 ref_tree, u64 dir_id, char *name,
		    int name_len)
{
	struct root_info *ri;
	struct rb_node *ret;
	ri = malloc(sizeof(*ri) + name_len + 1);
	if (!ri) {
		printf("memory allocation failed\n");
		exit(1);
	}
	memset(ri, 0, sizeof(*ri) + name_len + 1);
	ri->path = NULL;
	ri->dir_id = dir_id;
	ri->root_id = root_id;
	ri->ref_tree = ref_tree;
	strncpy(ri->name, name, name_len);

	ret = tree_insert(&root_lookup->root, root_id, ref_tree, &ri->rb_node);
	if (ret) {
		printf("failed to insert tree %llu\n", (unsigned long long)root_id);
		exit(1);
	}
	return 0;
}

/*
 * for a given root_info, search through the root_lookup tree to construct
 * the full path name to it.
 *
 * This can't be called until all the root_info->path fields are filled
 * in by lookup_ino_path
 */
static int resolve_root(struct root_lookup *rl, struct root_info *ri)
{
	u64 top_id;
	char *full_path = NULL;
	int len = 0;
	struct root_info *found;

	/*
	 * we go backwards from the root_info object and add pathnames
	 * from parent directories as we go.
	 */
	found = ri;
	while (1) {
		char *tmp;
		u64 next;
		int add_len = strlen(found->path);

		/* room for / and for null */
		tmp = malloc(add_len + 2 + len);
		if (full_path) {
			memcpy(tmp + add_len + 1, full_path, len);
			tmp[add_len] = '/';
			memcpy(tmp, found->path, add_len);
			tmp [add_len + len + 1] = '\0';
			free(full_path);
			full_path = tmp;
			len += add_len + 1;
		} else {
			full_path = strdup(found->path);
			len = add_len;
		}

		next = found->ref_tree;
		/* if the ref_tree refers to ourselves, we're at the top */
		if (next == found->root_id) {
			top_id = next;
			break;
		}

		/*
		 * if the ref_tree wasn't in our tree of roots, we're
		 * at the top
		 */
		found = tree_search(&rl->root, next);
		if (!found) {
			top_id = next;
			break;
		}
	}
	printf("ID %llu top level %llu path %s\n", ri->root_id, top_id,
	       full_path);
	free(full_path);
	return 0;
}

/*
 * for a single root_info, ask the kernel to give us a path name
 * inside it's ref_root for the dir_id where it lives.
 *
 * This fills in root_info->path with the path to the directory and and
 * appends this root's name.
 */
static int lookup_ino_path(int fd, struct root_info *ri)
{
	struct btrfs_ioctl_ino_lookup_args args;
	int ret;

	if (ri->path)
		return 0;

	memset(&args, 0, sizeof(args));
	args.treeid = ri->ref_tree;
	args.objectid = ri->dir_id;

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
	if (ret) {
		fprintf(stderr, "ERROR: Failed to lookup path for root %llu\n",
			(unsigned long long)ri->ref_tree);
		return ret;
	}

	if (args.name[0]) {
		/*
		 * we're in a subdirectory of ref_tree, the kernel ioctl
		 * puts a / in there for us
		 */
		ri->path = malloc(strlen(ri->name) + strlen(args.name) + 1);
		if (!ri->path) {
			perror("malloc failed");
			exit(1);
		}
		strcpy(ri->path, args.name);
		strcat(ri->path, ri->name);
	} else {
		/* we're at the root of ref_tree */
		ri->path = strdup(ri->name);
		if (!ri->path) {
			perror("strdup failed");
			exit(1);
		}
	}
	return 0;
}

int list_subvols(int fd)
{
	struct root_lookup root_lookup;
	struct rb_node *n;
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	struct btrfs_root_ref *ref;
	unsigned long off = 0;
	int name_len;
	char *name;
	u64 dir_id;
	int i;

	root_lookup_init(&root_lookup);

	memset(&args, 0, sizeof(args));

	/* search in the tree of tree roots */
	sk->tree_id = 1;

	/*
	 * set the min and max to backref keys.  The search will
	 * only send back this type of key now.
	 */
	sk->max_type = BTRFS_ROOT_BACKREF_KEY;
	sk->min_type = BTRFS_ROOT_BACKREF_KEY;

	/*
	 * set all the other params to the max, we'll take any objectid
	 * and any trans
	 */
	sk->max_objectid = (u64)-1;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;

	/* just a big number, doesn't matter much */
	sk->nr_items = 4096;

	while(1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0) {
			fprintf(stderr, "ERROR: can't perform the search\n");
			return ret;
		}
		/* the ioctl returns the number of item it found in nr_items */
		if (sk->nr_items == 0)
			break;

		off = 0;

		/*
		 * for each item, pull the key out of the header and then
		 * read the root_ref item it contains
		 */
		for (i = 0; i < sk->nr_items; i++) {
			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);
			off += sizeof(*sh);

			ref = (struct btrfs_root_ref *)(args.buf + off);
			name_len = btrfs_stack_root_ref_name_len(ref);
			name = (char *)(ref + 1);
			dir_id = btrfs_stack_root_ref_dirid(ref);

			add_root(&root_lookup, sh->objectid, sh->offset,
				 dir_id, name, name_len);

			off += sh->len;

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_objectid = sh->objectid;
			sk->min_type = sh->type;
			sk->min_offset = sh->offset;
		}
		sk->nr_items = 4096;
		/* this iteration is done, step forward one root for the next
		 * ioctl
		 */
		if (sk->min_objectid < (u64)-1)
			sk->min_objectid++;
		else
			break;
	}
	/*
	 * now we have an rbtree full of root_info objects, but we need to fill
	 * in their path names within the subvol that is referencing each one.
	 */
	n = rb_first(&root_lookup.root);
	while (n) {
		struct root_info *entry;
		int ret;
		entry = rb_entry(n, struct root_info, rb_node);
		ret = lookup_ino_path(fd, entry);
		if(ret < 0)
			return ret;
		n = rb_next(n);
	}

	/* now that we have all the subvol-relative paths filled in,
	 * we have to string the subvols together so that we can get
	 * a path all the way back to the FS root
	 */
	n = rb_last(&root_lookup.root);
	while (n) {
		struct root_info *entry;
		entry = rb_entry(n, struct root_info, rb_node);
		resolve_root(&root_lookup, entry);
		n = rb_prev(n);
	}

	return ret;
}