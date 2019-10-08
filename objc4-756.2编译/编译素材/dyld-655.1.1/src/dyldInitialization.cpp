/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2008 Apple Inc. All rights reserved.
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

#define __STDC_LIMIT_MACROS
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <Availability.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <mach-o/reloc.h>
#if __x86_64__
	#include <mach-o/x86_64/reloc.h>
#endif
#include "dyld.h"
#include "dyldSyscallInterface.h"

// from dyld_gdb.cpp 
extern void addImagesToAllImages(uint32_t infoCount, const dyld_image_info info[]);
extern void syncProcessInfo();

#ifndef MH_PIE
	#define MH_PIE 0x200000 
#endif

// currently dyld has no initializers, but if some come back, set this to non-zero
#define DYLD_INITIALIZER_SUPPORT  0

#if __LP64__
	#define LC_SEGMENT_COMMAND		LC_SEGMENT_64
	#define macho_segment_command	segment_command_64
	#define macho_section			section_64
	#define RELOC_SIZE				3
#else
	#define LC_SEGMENT_COMMAND		LC_SEGMENT
	#define macho_segment_command	segment_command
	#define macho_section			section
	#define RELOC_SIZE				2
#endif

#if __x86_64__
	#define POINTER_RELOC X86_64_RELOC_UNSIGNED
#else
	#define POINTER_RELOC GENERIC_RELOC_VANILLA
#endif

#ifndef BIND_OPCODE_THREADED
#define BIND_OPCODE_THREADED    0xD0
#endif

#ifndef BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB
#define BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB    0x00
#endif

#ifndef BIND_SUBOPCODE_THREADED_APPLY
#define BIND_SUBOPCODE_THREADED_APPLY                                0x01
#endif


#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif


#if TARGET_IPHONE_SIMULATOR
const dyld::SyscallHelpers* gSyscallHelpers = NULL;
#endif


//
//  Code to bootstrap dyld into a runnable state
//
//

namespace dyldbootstrap {



#if DYLD_INITIALIZER_SUPPORT

typedef void (*Initializer)(int argc, const char* argv[], const char* envp[], const char* apple[]);

extern const Initializer  inits_start  __asm("section$start$__DATA$__mod_init_func");
extern const Initializer  inits_end    __asm("section$end$__DATA$__mod_init_func");

//
// For a regular executable, the crt code calls dyld to run the executables initializers.
// For a static executable, crt directly runs the initializers.
// dyld (should be static) but is a dynamic executable and needs this hack to run its own initializers.
// We pass argc, argv, etc in case libc.a uses those arguments
//
static void runDyldInitializers(const struct macho_header* mh, intptr_t slide, int argc, const char* argv[], const char* envp[], const char* apple[])
{
	for (const Initializer* p = &inits_start; p < &inits_end; ++p) {
		(*p)(argc, argv, envp, apple);
	}
}
#endif // DYLD_INITIALIZER_SUPPORT


//
//  The kernel may have slid a Position Independent Executable
//
static uintptr_t slideOfMainExecutable(const struct macho_header* mh)
{
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			const struct macho_segment_command* segCmd = (struct macho_segment_command*)cmd;
			if ( (segCmd->fileoff == 0) && (segCmd->filesize != 0)) {
				return (uintptr_t)mh - segCmd->vmaddr;
			}
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return 0;
}

inline uint64_t read_uleb128(const uint8_t*& p, const uint8_t* end) {
    uint64_t result = 0;
    int         bit = 0;
    do {
        if (p == end)
            throw "malformed uleb128 extends beyond trie";
        uint64_t slice = *p & 0x7f;

        if (bit >= 64 || slice << bit >> bit != slice)
            throw "uleb128 too big for 64-bits";
        else {
            result |= (slice << bit);
            bit += 7;
        }
    }
    while (*p++ & 0x80);
    return result;
}

inline int64_t read_sleb128(const uint8_t*& p, const uint8_t* end)
{
    int64_t result = 0;
    int bit = 0;
    uint8_t byte;
    do {
        if (p == end)
            throw "malformed sleb128";
        byte = *p++;
        result |= (((int64_t)(byte & 0x7f)) << bit);
        bit += 7;
    } while (byte & 0x80);
    // sign extend negative numbers
    if ( (byte & 0x40) != 0 )
        result |= (~0ULL) << bit;
    return result;
}


//
// If the kernel does not load dyld at its preferred address, we need to apply 
// fixups to various initialized parts of the __DATA segment
//
static void rebaseDyld(const struct macho_header* mh, intptr_t slide)
{
	// rebase non-lazy pointers (which all point internal to dyld, since dyld uses no shared libraries)
	// and get interesting pointers into dyld
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;

    // First look for compressed info and use it if it exists.
    const struct macho_segment_command* linkEditSeg = NULL;
    const dyld_info_command* dyldInfoCmd = NULL;
    for (uint32_t i = 0; i < cmd_count; ++i) {
        switch (cmd->cmd) {
            case LC_SEGMENT_COMMAND:
            {
                const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
                if ( strcmp(seg->segname, "__LINKEDIT") == 0 )
                    linkEditSeg = seg;
                break;
            }
            case LC_DYLD_INFO_ONLY:
                dyldInfoCmd = (struct dyld_info_command*)cmd;
                break;
        }
        if (dyldInfoCmd && linkEditSeg)
            break;
        cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
    }
    if ( linkEditSeg == NULL )
        throw "dyld missing LINKEDIT";

    // Reset the iterator.
    cmd = cmds;

    auto getSegmentAtIndex = [cmd_count, cmds](unsigned segIndex) -> const struct macho_segment_command* {
        const struct load_command* cmd = cmds;
        for (uint32_t i = 0; i < cmd_count; ++i) {
            switch (cmd->cmd) {
                case LC_SEGMENT_COMMAND:
                    if (!segIndex) {
                        const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
                        return seg;
                    }
                    --segIndex;
                    break;
            }
            cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
        }
        throw "out of bounds command";
        return 0;
    };

    auto segActualLoadAddress = [&](unsigned segIndex) -> uintptr_t {
        const struct macho_segment_command* seg = getSegmentAtIndex(segIndex);
        return seg->vmaddr + slide;
    };

#if __has_feature(ptrauth_calls)
    auto imageBaseAddress = [cmds, cmd_count]() -> uintptr_t {
        const struct load_command* cmd = cmds;
        for (uint32_t i = 0; i < cmd_count; ++i) {
            switch (cmd->cmd) {
                case LC_SEGMENT_COMMAND: {
                    const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
                    if ( (seg->fileoff == 0) && (seg->filesize != 0) )
                        return seg->vmaddr;
                    break;
                }
            }
            cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
        }
        return 0;
    };
#endif

    if (dyldInfoCmd && (dyldInfoCmd->bind_size != 0) ) {
        if ( dyldInfoCmd->rebase_size != 0 )
            throw "unthreaded rebases are not supported";

        const uint8_t* linkEditBase = (uint8_t*)(linkEditSeg->vmaddr + slide - linkEditSeg->fileoff);

        const uint8_t* const start = linkEditBase + dyldInfoCmd->bind_off;
        const uint8_t* const end = &start[dyldInfoCmd->bind_size];
        const uint8_t* p = start;

        uintptr_t segmentStartAddress = 0;
        uint64_t segOffset = 0;
        int segIndex = 0;
#if __has_feature(ptrauth_calls)
        uintptr_t fBaseAddress = imageBaseAddress();
#endif
        bool done = false;

        while ( !done && (p < end) ) {
            uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
            uint8_t opcode = *p & BIND_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case BIND_OPCODE_DONE:
                    done = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                    break;
                case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                    break;
                case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                    while (*p != '\0')
                        ++p;
                    ++p;
                    break;
                case BIND_OPCODE_SET_TYPE_IMM:
                    break;
                case BIND_OPCODE_SET_ADDEND_SLEB:
                    read_sleb128(p, end);
                    break;
                case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segIndex = immediate;
                    segmentStartAddress = segActualLoadAddress(segIndex);
                    segOffset = read_uleb128(p, end);
                    break;
                case BIND_OPCODE_ADD_ADDR_ULEB:
                    segOffset += read_uleb128(p, end);
                    break;
                case BIND_OPCODE_DO_BIND:
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                    break;
                case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                    read_uleb128(p, end);
                    read_uleb128(p, end);
                    break;
                case BIND_OPCODE_THREADED:
                    // Note the immediate is a sub opcode
                    switch (immediate) {
                        case BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB:
                            read_uleb128(p, end);
                            break;
                        case BIND_SUBOPCODE_THREADED_APPLY: {
                            uint64_t delta = 0;
                            do {
                                uintptr_t address = segmentStartAddress + (uintptr_t)segOffset;
                                uint64_t value = *(uint64_t*)address;

#if __has_feature(ptrauth_calls)
                                uint16_t diversity = (uint16_t)(value >> 32);
                                bool hasAddressDiversity = (value & (1ULL << 48)) != 0;
                                ptrauth_key key = (ptrauth_key)((value >> 49) & 0x3);
                                bool isAuthenticated = (value & (1ULL << 63)) != 0;
#endif
                                bool isRebase = (value & (1ULL << 62)) == 0;
                                if (isRebase) {

#if __has_feature(ptrauth_calls)
                                    if (isAuthenticated) {
                                        // The new value for a rebase is the low 32-bits of the threaded value plus the slide.
                                        uint64_t newValue = (value & 0xFFFFFFFF) + slide;
                                        // Add in the offset from the mach_header
                                        newValue += fBaseAddress;
                                        // We have bits to merge in to the discriminator
                                        uintptr_t discriminator = diversity;
                                        if (hasAddressDiversity) {
                                            // First calculate a new discriminator using the address of where we are trying to store the value
                                            discriminator = __builtin_ptrauth_blend_discriminator((void*)address, discriminator);
                                        }
                                        switch (key) {
                                            case ptrauth_key_asia:
                                                newValue = (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)newValue, ptrauth_key_asia, discriminator);
                                                break;
                                            case ptrauth_key_asib:
                                                newValue = (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)newValue, ptrauth_key_asib, discriminator);
                                                break;
                                            case ptrauth_key_asda:
                                                newValue = (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)newValue, ptrauth_key_asda, discriminator);
                                                break;
                                            case ptrauth_key_asdb:
                                                newValue = (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)newValue, ptrauth_key_asdb, discriminator);
                                                break;
                                        }
                                        *(uint64_t*)address = newValue;
                                    } else
#endif
                                    {
                                        // Regular pointer which needs to fit in 51-bits of value.
                                        // C++ RTTI uses the top bit, so we'll allow the whole top-byte
                                        // and the signed-extended bottom 43-bits to be fit in to 51-bits.
                                        uint64_t top8Bits = value & 0x0007F80000000000ULL;
                                        uint64_t bottom43Bits = value & 0x000007FFFFFFFFFFULL;
                                        uint64_t targetValue = ( top8Bits << 13 ) | (((intptr_t)(bottom43Bits << 21) >> 21) & 0x00FFFFFFFFFFFFFF);
                                        targetValue = targetValue + slide;
                                        *(uint64_t*)address = targetValue;
                                    }
                                }

                                // The delta is bits [51..61]
                                // And bit 62 is to tell us if we are a rebase (0) or bind (1)
                                value &= ~(1ULL << 62);
                                delta = ( value & 0x3FF8000000000000 ) >> 51;
                                segOffset += delta * sizeof(uintptr_t);
                            } while ( delta != 0 );
                            break;
                        }
                        default:
                            throw "unknown threaded bind subopcode";
                    }
                    break;
                default:
                    throw "unknown bind opcode";
            }
        }
        return;
    }

#if __x86_64__
	const struct macho_segment_command* firstWritableSeg = NULL;
#endif
	const struct dysymtab_command* dynamicSymbolTable = NULL;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						if ( type == S_NON_LAZY_SYMBOL_POINTERS ) {
							// rebase non-lazy pointers (which all point internal to dyld, since dyld uses no shared libraries)
							const uint32_t pointerCount = (uint32_t)(sect->size / sizeof(uintptr_t));
							uintptr_t* const symbolPointers = (uintptr_t*)(sect->addr + slide);
							for (uint32_t j=0; j < pointerCount; ++j) {
								symbolPointers[j] += slide;
							}
						}
					}
#if __x86_64__
					if ( (firstWritableSeg == NULL) && (seg->initprot & VM_PROT_WRITE) )
						firstWritableSeg = seg;
#endif
				}
				break;
			case LC_DYSYMTAB:
				dynamicSymbolTable = (struct dysymtab_command *)cmd;
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	
	// use reloc's to rebase all random data pointers
#if __x86_64__
    if ( firstWritableSeg == NULL )
        throw "no writable segment in dyld";
	const uintptr_t relocBase = firstWritableSeg->vmaddr + slide;
#else
	const uintptr_t relocBase = (uintptr_t)mh;
#endif
	const relocation_info* const relocsStart = (struct relocation_info*)(linkEditSeg->vmaddr + slide + dynamicSymbolTable->locreloff - linkEditSeg->fileoff);
	const relocation_info* const relocsEnd = &relocsStart[dynamicSymbolTable->nlocrel];
	for (const relocation_info* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		if ( reloc->r_length != RELOC_SIZE ) 
			throw "relocation in dyld has wrong size";

		if ( reloc->r_type != POINTER_RELOC ) 
			throw "relocation in dyld has wrong type";
		
		// update pointer by amount dyld slid
		*((uintptr_t*)(reloc->r_address + relocBase)) += slide;
	}
}


extern "C" void mach_init();
extern "C" void __guard_setup(const char* apple[]);


//
//  This is code to bootstrap dyld.  This work in normally done for a program by dyld and crt.
//  In dyld we have to do this manually.
//
uintptr_t start(const struct macho_header* appsMachHeader, int argc, const char* argv[], 
				intptr_t slide, const struct macho_header* dyldsMachHeader,
				uintptr_t* startGlue)
{
	// if kernel had to slide dyld, we need to fix up load sensitive locations
	// we have to do this before using any global variables
    slide = slideOfMainExecutable(dyldsMachHeader);
    bool shouldRebase = slide != 0;
#if __has_feature(ptrauth_calls)
    shouldRebase = true;
#endif
    if ( shouldRebase ) {
        rebaseDyld(dyldsMachHeader, slide);
    }

	// allow dyld to use mach messaging
	mach_init();

	// kernel sets up env pointer to be just past end of agv array
	const char** envp = &argv[argc+1];
	
	// kernel sets up apple pointer to be just past end of envp array
	const char** apple = envp;
	while(*apple != NULL) { ++apple; }
	++apple;

	// set up random value for stack canary
	__guard_setup(apple);

#if DYLD_INITIALIZER_SUPPORT
	// run all C++ initializers inside dyld
	runDyldInitializers(dyldsMachHeader, slide, argc, argv, envp, apple);
#endif

	// now that we are done bootstrapping dyld, call dyld's main
	uintptr_t appsSlide = slideOfMainExecutable(appsMachHeader);
	return dyld::_main(appsMachHeader, appsSlide, argc, argv, envp, apple, startGlue);
}


#if TARGET_IPHONE_SIMULATOR

extern "C" uintptr_t start_sim(int argc, const char* argv[], const char* envp[], const char* apple[],
							const macho_header* mainExecutableMH, const macho_header* dyldMH, uintptr_t dyldSlide,
							const dyld::SyscallHelpers*, uintptr_t* startGlue);
					
					
uintptr_t start_sim(int argc, const char* argv[], const char* envp[], const char* apple[],
					const macho_header* mainExecutableMH, const macho_header* dyldMH, uintptr_t dyldSlide,
					const dyld::SyscallHelpers* sc, uintptr_t* startGlue)
{
	// if simulator dyld loaded slid, it needs to rebase itself
	// we have to do this before using any global variables
	if ( dyldSlide != 0 ) {
		rebaseDyld(dyldMH, dyldSlide);
	}

	// save table of syscall pointers
	gSyscallHelpers = sc;
	
	// allow dyld to use mach messaging
	mach_init();

	// set up random value for stack canary
	__guard_setup(apple);

	// setup gProcessInfo to point to host dyld's struct
	dyld::gProcessInfo = (struct dyld_all_image_infos*)(sc->getProcessInfo());
	syncProcessInfo();

	// now that we are done bootstrapping dyld, call dyld's main
	uintptr_t appsSlide = slideOfMainExecutable(mainExecutableMH);
	return dyld::_main(mainExecutableMH, appsSlide, argc, argv, envp, apple, startGlue);
}
#endif


} // end of namespace




