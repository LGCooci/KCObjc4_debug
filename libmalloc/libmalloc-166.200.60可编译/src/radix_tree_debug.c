/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <radix_tree.h>
#include <radix_tree_internal.h>

#include <stdio.h>

static
bool
radix_tree_fsck_recursive(struct radix_tree *tree, struct radix_node *node,
						  uint64_t key, int keyshift,
						  uint64_t min)
{
	bool ok = true;
	for (int i = 0; i < 2; i++) {
		struct radix_edge *edge = &node->edges[i];
		if (edge->labelBits == 0)
			continue;

		uint64_t edgekey = extend_key(key, edge->labelBits, keyshift, edge->label);

		if (edge->isLeaf) {
			if (edgekey < min)
			{
				fprintf(stderr, "!!!! node=%p min=%llx edge=%d edgekey=%llx\n", node, min, i, edgekey);
				return false;
			}

			struct radix_node *leaf = getnode(tree, edge->index);
			min = edgekey + leaf_size(tree, leaf);
		} else {
			ok = ok && radix_tree_fsck_recursive(tree, getnode(tree, edge->index), edgekey, keyshift+edge->labelBits, min);
		}
	}
	return ok;
}

bool
radix_tree_fsck(struct radix_tree *tree)
{
	return radix_tree_fsck_recursive(tree, getnode(tree, 0), 0, 0, 0);
}

static
void
radix_tree_print_recursive(struct radix_tree *tree, struct radix_node *node, int indent,
						   uint64_t key, int keyshift) {
	if (node->edges[0].labelBits == 0 && node->edges[1].labelBits == 0) {
		printf("%p:", node);
		for (int i = 0; i < indent; i++) printf(" ");
		printf("empty\n");
	}
	for (int i = 0; i < 2; i++) {
		struct radix_edge *edge = &node->edges[i];
		if (edge->labelBits == 0)
			continue;
		printf("%p:", node);
		for (int i = 0; i < indent; i++) printf(" ");
		printf ("0x%x/%d", edge->label, edge->labelBits);
		if (edge->isLeaf) {
			struct radix_node *leaf = getnode(tree, edge->index);
			printf(" [%llx-%llx] -> stack=%llx\n",
				   extend_key(key, edge->labelBits, keyshift, edge->label),
				   extend_key(key, edge->labelBits, keyshift, edge->label) + leaf_size(tree, leaf),
				   leaf->stackid);
		} else {
			printf("\n");
			radix_tree_print_recursive(tree, getnode(tree, edge->index), indent + 4,
									   extend_key(key, edge->labelBits, keyshift, edge->label),
									   keyshift + edge->labelBits);
		}
	}
}

void
radix_tree_print(struct radix_tree *tree)
{
	radix_tree_print_recursive(tree, getnode(tree, 0), 0, 0, 0);
}
