/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
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

#ifndef __RADIX_TREE_INTERNAL_H
#define __RADIX_TREE_INTERNAL_H

#include <radix_tree.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>

#define RADIX_LABEL_BITS 11 //max number of label bits per edge
#define RADIX_TREE_KEY_BITS (64 - 12) //number of keybits we use (starting from MSB)

struct radix_edge {
    unsigned label : RADIX_LABEL_BITS;
    unsigned index : 16;
    unsigned labelBits : 4;
    unsigned isLeaf : 1;
};

_Static_assert(sizeof(struct radix_edge) == 4, "size of radix_edge must be 4");

struct radix_node {
    union {
        struct { 
            struct radix_edge edges[2];
        };
		struct {
			uint64_t stackid : 32;
			uint64_t size : 32;
		};
        struct { 
            uint16_t next_free;
            bool next_free_is_initialized;
        };
		uint64_t as_u64;
    };
};

_Static_assert(sizeof(struct radix_node) == 8, "size of radix_node must be 8");


struct radix_tree {
	char header[8];
	uint32_t leaf_size_shift;
    uint32_t num_nodes;
    uint32_t next_free;
    struct radix_node nodes[];
};

static inline
uint64_t
leaf_size(struct radix_tree *tree, struct radix_node *node)
{
	return ((uint64_t)node->size) << tree->leaf_size_shift;
}

static inline
void
set_leaf_size(struct radix_tree *tree, struct radix_node *node, uint64_t size)
{
	node->size = size >> tree->leaf_size_shift;
	assert(leaf_size(tree, node) == size);
}


/*
 * Does this edge even exist?
 */
static inline 
bool 
edge_valid(struct radix_edge *edge) 
{
    return edge->labelBits != 0;
}

/*
 * Read the most significant labelBits out of (key << keyshift)
 */
static inline 
unsigned
keybits(uint64_t key, int labelBits, int keyshift)
{
    uint64_t mask = (1 << labelBits) - 1;
    return (unsigned) (key >> (64 - labelBits - keyshift)) & mask;
}

/*
 * Add labelBits to key.
 */
static inline
uint64_t
extend_key(uint64_t key, int labelBits, int keyshift, uint64_t label) {
	uint64_t mask __unused = (1 << labelBits) - 1;
	assert((label & ~mask) == 0);
	int shift = 64 - keyshift - labelBits; // [ keyshift | labelbits | 64 - keyshift - labelbits ]
	assert((key & (mask << shift)) == 0);
	return key | (label << shift);
}

/*
 * Return true if exact radix tree traversal should follow this edge.
 * That is, return true if the edge label matches the key bits exactly.
 */
static inline 
bool 
edge_matches(struct radix_edge *edge, uint64_t key, int keyshift) 
{
    if (!edge_valid(edge)) {
        return false;
    }
    return keybits(key, edge->labelBits, keyshift) == edge->label;
}


/*
 * Count the number of most-significant bits that (key << keyshift) has in
 * common with edge.
 */
static inline 
int 
count_matching_bits(struct radix_edge *edge, uint64_t key, int keyshift)
{
    int labelBits = edge->labelBits;
    uint64_t label = edge->label; 
    while (labelBits) {
        if (keybits(key, labelBits, keyshift) == label) { 
            return labelBits; 
        } 
        labelBits--; 
        label >>= 1;
    }
    return 0;
}

/*
 * Lookup a radix tree node by index.  Returns NULL for invalid index.
 */
static inline 
struct radix_node *
getnode(struct radix_tree *tree, unsigned index)
{ 
    if (index > tree->num_nodes) {
        return NULL;
    } else { 
        return &tree->nodes[index];
    }
}

/*
 * Initialize a radix tree in a new buffer
 */
struct radix_tree *
radix_tree_init(void *buf, size_t size);


/*
 * Print a representation of a radix tree to stdout.
 */
void
radix_tree_print(struct radix_tree *tree);

/*
 * Check a radix tree for consistency.  Returns true if everything is ok.
 */
bool
radix_tree_fsck(struct radix_tree *tree);


#endif
