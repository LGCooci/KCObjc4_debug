/*
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

#include "stack_trace.h"

#include <TargetConditionals.h>
// #include <thread_stack_pcs.h>


// Note: Shifts on signed types are a minefield.  Avoid doing it!

static uintptr_t
zigzag_encode(uintptr_t val)
{
	uintptr_t x = val << 1;
	return ((intptr_t)val < 0) ? ~x : x;
}

static uintptr_t
zigzag_decode(uintptr_t encoded_val)
{
	uintptr_t x = encoded_val >> 1;
	return (encoded_val & 1) ? ~x : x;
}

static uintptr_t
codeoffset_encode(intptr_t val)
{
	intptr_t x = val;
	if (TARGET_CPU_ARM64) x /= 4;  // instructions are 4 byte-aligned
	return zigzag_encode(x);
}

static intptr_t
codeoffset_decode(uintptr_t encoded_val)
{
	intptr_t x = zigzag_decode(encoded_val);
	if (TARGET_CPU_ARM64) x *= 4;  // instructions are 4 byte-aligned
	return x;
}

const static uintptr_t k_shift = 7;
const static uintptr_t k_stop_bit = 1 << k_shift;
const static uintptr_t k_data_bits = ~k_stop_bit;

static size_t
varint_encode(uint8_t *buffer, size_t size, uintptr_t val)
{
	uintptr_t x = val;
	size_t len = 0;
	do {
		if (len == size) {
			return 0;
		}
		buffer[len] = x & k_data_bits;
		x >>= k_shift;
		len++;
	} while (x);

	buffer[len - 1] |= k_stop_bit;
	return len;
}

static size_t
varint_decode(const uint8_t *buffer, size_t size, uintptr_t *val)
{
	uintptr_t x = 0;
	size_t len = 0;
	do {
		if (len == size) {
			return 0;
		}
		uintptr_t bits = buffer[len] & k_data_bits;
		x |= bits << (len * k_shift);
		len++;
	} while (!(buffer[len - 1] & k_stop_bit));

	*val = x;
	return len;
}

static size_t
trace_encode(uint8_t *buffer, size_t size, const vm_address_t *addrs, uint32_t num_addrs)
{
	size_t used = 0;
	for (uint32_t i = 0; i < num_addrs; i++) {
		intptr_t offset = addrs[i] - (i > 0 ? addrs[i - 1] : 0);
		uintptr_t encoded_offset = codeoffset_encode(offset);
		size_t len = varint_encode(&buffer[used], size - used, encoded_offset);
		if (len == 0) {
			break;
		}
		used += len;
	}

	return used;
}

uint32_t
trace_decode(const uint8_t *buffer, size_t size, vm_address_t *addrs, uint32_t num_addrs)
{
	size_t used = 0;
	uint32_t i;
	for (i = 0; i < num_addrs; i++) {
		uintptr_t encoded_offset;
		size_t len = varint_decode(&buffer[used], size - used, &encoded_offset);
		if (len == 0) {
			break;
		}
		used += len;

		intptr_t offset = codeoffset_decode(encoded_offset);
		addrs[i] = offset + (i > 0 ? addrs[i - 1] : 0);
	}

	return i;
}

MALLOC_NOINLINE
size_t
trace_collect(uint8_t *buffer, size_t size)
{
	// frame 0: thread_stack_pcs() itself
	// frame 1: this function (no inline)
	// last frame: usually garbage value
	const uint32_t dropped_frames = 3;
	const uint32_t good_frames = 64;
	const uint32_t max_frames = good_frames + dropped_frames;
	vm_address_t frames[max_frames];

	uint32_t num_frames;
	thread_stack_pcs(frames, max_frames, &num_frames);
	if (num_frames <= dropped_frames) {
		return 0;
	}

	uint32_t num_addrs = num_frames - dropped_frames;
	return trace_encode(buffer, size, &frames[2], num_addrs);
}
