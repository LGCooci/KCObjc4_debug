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

#include <sys/types.h>
#include <mach/mach.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/reloc.h>
#include <mach-o/nlist.h>
#include <TargetConditionals.h>

#include "MachOAnalyzer.h"
#include "CodeSigningTypes.h"
#include "Array.h"

#include <stdio.h>


#ifndef BIND_OPCODE_THREADED
    #define BIND_OPCODE_THREADED    0xD0
#endif

#ifndef BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB
    #define BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB    0x00
#endif

#ifndef BIND_SUBOPCODE_THREADED_APPLY
    #define BIND_SUBOPCODE_THREADED_APPLY                               0x01
#endif


namespace dyld3 {


const MachOAnalyzer* MachOAnalyzer::validMainExecutable(Diagnostics& diag, const mach_header* mh, const char* path, uint64_t sliceLength, const char* reqArchName, Platform reqPlatform)
{
    const MachOAnalyzer* result = (const MachOAnalyzer*)mh;
    if ( !result->validMachOForArchAndPlatform(diag, (size_t)sliceLength, path, reqArchName, reqPlatform) )
        return nullptr;
    if ( !result->isDynamicExecutable() )
        return nullptr;

    return result;
}


closure::LoadedFileInfo MachOAnalyzer::load(Diagnostics& diag, const closure::FileSystem& fileSystem, const char* path, const char* reqArchName, Platform reqPlatform)
{
    // FIXME: This should probably be an assert, but if we happen to have a diagnostic here then something is wrong
    // above us and we should quickly return instead of doing unnecessary work.
    if (diag.hasError())
        return closure::LoadedFileInfo();

    closure::LoadedFileInfo info;
    char realerPath[MAXPATHLEN];
    if (!fileSystem.loadFile(path, info, realerPath, ^(const char *format, ...) {
        va_list list;
        va_start(list, format);
        diag.error(format, list);
        va_end(list);
    })) {
        return closure::LoadedFileInfo();
    }

    // If we now have an error, but succeeded, then we must have tried multiple paths, one of which errored, but
    // then succeeded on a later path.  So clear the error.
    if (diag.hasError())
        diag.clearError();

    // if fat, remap just slice needed
    bool fatButMissingSlice;
    const FatFile*       fh = (FatFile*)info.fileContent;
    uint64_t sliceOffset = info.sliceOffset;
    uint64_t sliceLen = info.sliceLen;
    if ( fh->isFatFileWithSlice(diag, info.fileContentLen, reqArchName, sliceOffset, sliceLen, fatButMissingSlice) ) {
        if ( (sliceOffset & 0xFFF) != 0 ) {
            // slice not page aligned
            if ( strncmp((char*)info.fileContent + sliceOffset, "!<arch>", 7) == 0 )
                diag.error("file is static library");
            else
                diag.error("slice is not page aligned");
            fileSystem.unloadFile(info);
            return closure::LoadedFileInfo();
        }
        else {
            // unmap anything before slice
            fileSystem.unloadPartialFile(info, sliceOffset, sliceLen);
            // Update the info to keep track of the new slice offset.
            info.sliceOffset = sliceOffset;
            info.sliceLen = sliceLen;
        }
    }
    else if ( fatButMissingSlice ) {
        diag.error("missing required arch %s in %s", reqArchName, path);
        fileSystem.unloadFile(info);
        return closure::LoadedFileInfo();
    }

    const MachOAnalyzer* mh = (MachOAnalyzer*)info.fileContent;

    // validate is mach-o of requested arch and platform
    if ( !mh->validMachOForArchAndPlatform(diag, (size_t)info.sliceLen, path, reqArchName, reqPlatform) ) {
        fileSystem.unloadFile(info);
        return closure::LoadedFileInfo();
    }

    // if has zero-fill expansion, re-map
    mh = mh->remapIfZeroFill(diag, fileSystem, info);

    // on error, remove mappings and return nullptr
    if ( diag.hasError() ) {
        fileSystem.unloadFile(info);
        return closure::LoadedFileInfo();
    }

    // now that LINKEDIT is at expected offset, finish validation
    mh->validLinkedit(diag, path);

    // on error, remove mappings and return nullptr
    if ( diag.hasError() ) {
        fileSystem.unloadFile(info);
        return closure::LoadedFileInfo();
    }

    return info;
}

#if DEBUG
// only used in debug builds of cache builder to verify segment moves are valid
void MachOAnalyzer::validateDyldCacheDylib(Diagnostics& diag, const char* path) const
{
    validLinkedit(diag, path);
    validSegments(diag, path, 0xffffffff);
}
#endif

uint64_t MachOAnalyzer::mappedSize() const
{
    const uint32_t pageSize = uses16KPages() ? 0x4000 : 0x1000;
    __block uint64_t textSegVmAddr   = 0;
    __block uint64_t vmSpaceRequired = 0;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            textSegVmAddr = info.vmAddr;
        }
        else if ( strcmp(info.segName, "__LINKEDIT") == 0 ) {
            vmSpaceRequired = info.vmAddr + ((info.vmSize + (pageSize-1)) & (-pageSize)) - textSegVmAddr;
            stop = true;
        }
    });

    return vmSpaceRequired;
}

bool MachOAnalyzer::validMachOForArchAndPlatform(Diagnostics& diag, size_t sliceLength, const char* path, const char* reqArchName, Platform reqPlatform) const
{
    // must start with mach-o magic value
    if ( (this->magic != MH_MAGIC) && (this->magic != MH_MAGIC_64) ) {
        diag.error("could not use '%s' because it is not a mach-o file: 0x%08X 0x%08X", path, this->magic, this->cputype);
        return false;
    }

    // must match requested architecture, if specified
    if ( reqArchName != nullptr ) {
        if ( !this->isArch(reqArchName)) {
            // except when looking for x86_64h, fallback to x86_64
            if ( (strcmp(reqArchName, "x86_64h") != 0) || !this->isArch("x86_64") ) {
#if SUPPORT_ARCH_arm64e
                // except when looking for arm64e, fallback to arm64
                if ( (strcmp(reqArchName, "arm64e") != 0) || !this->isArch("arm64") ) {
#endif
                    diag.error("could not use '%s' because it does not contain required architecture %s", path, reqArchName);
                    return false;
#if SUPPORT_ARCH_arm64e
                }
#endif
            }
        }
    }

    // must be a filetype dyld can load
    switch ( this->filetype ) {
        case MH_EXECUTE:
        case MH_DYLIB:
        case MH_BUNDLE:
            break;
        default:
            diag.error("could not use '%s' because it is not a dylib, bundle, or executable, filetype=0x%08X", path, this->filetype);
           return false;
    }

    // validate load commands structure
    if ( !this->validLoadCommands(diag, path, sliceLength) ) {
        return false;
    }

    // filter out static executables
    if ( (this->filetype == MH_EXECUTE) && !isDynamicExecutable() ) {
        diag.error("could not use '%s' because it is a static executable", path);
        return false;
    }

    // must match requested platform (do this after load commands are validated)
    if ( !this->supportsPlatform(reqPlatform) ) {
        diag.error("could not use '%s' because it was built for a different platform", path);
        return false;
    }

    // validate dylib loads
    if ( !validEmbeddedPaths(diag, path) )
        return false;

    // validate segments
    if ( !validSegments(diag, path, sliceLength) )
        return false;

    // validate entry
    if ( this->filetype == MH_EXECUTE ) {
        if ( !validMain(diag, path) )
            return false;
    }

    // <rdar://problem/45525884> to avoid heap smasher, don't load this dylib
    if ( strcmp(path, "/usr/lib/libnetsnmp.5.2.1.dylib") == 0 )
        return false;

    // further validations done in validLinkedit()

    return true;
}

bool MachOAnalyzer::validLinkedit(Diagnostics& diag, const char* path) const
{
    // validate LINKEDIT layout
    if ( !validLinkeditLayout(diag, path) )
        return false;

    if ( hasChainedFixups() ) {
        if ( !validChainedFixupsInfo(diag, path) )
            return false;
    }
    else {
        // validate rebasing info
        if ( !validRebaseInfo(diag, path) )
            return false;

       // validate binding info
        if ( !validBindInfo(diag, path) )
            return false;
    }

    return true;
}

bool MachOAnalyzer::validLoadCommands(Diagnostics& diag, const char* path, size_t fileLen) const
{
    // check load command don't exceed file length
    if ( this->sizeofcmds + sizeof(mach_header_64) > fileLen ) {
        diag.error("in '%s' load commands exceed length of file", path);
        return false;
    }

    // walk all load commands and sanity check them
    Diagnostics walkDiag;
    forEachLoadCommand(walkDiag, ^(const load_command* cmd, bool& stop) {});
    if ( walkDiag.hasError() ) {
#if BUILDING_CACHE_BUILDER
        diag.error("in '%s' %s", path, walkDiag.errorMessage().c_str());
#else
        diag.error("in '%s' %s", path, walkDiag.errorMessage());
#endif
        return false;
    }

    // check load commands fit in TEXT segment
    __block bool foundTEXT    = false;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            foundTEXT = true;
            if ( this->sizeofcmds + sizeof(mach_header_64) > info.fileSize ) {
                diag.error("in '%s' load commands exceed length of __TEXT segment", path);
            }
            if ( info.fileOffset != 0 ) {
                diag.error("in '%s' __TEXT segment not start of mach-o", path);
            }
            stop = true;
        }
    });
    if ( !diag.noError() && !foundTEXT ) {
        diag.error("in '%s' __TEXT segment not found", path);
        return false;
    }

    return true;
}

const MachOAnalyzer* MachOAnalyzer::remapIfZeroFill(Diagnostics& diag, const closure::FileSystem& fileSystem, closure::LoadedFileInfo& info) const
{
    uint64_t vmSpaceRequired;
    auto hasZeroFill = [this, &vmSpaceRequired]() {
        __block bool     hasZeroFill     = false;
        __block uint64_t textSegVmAddr   = 0;
        forEachSegment(^(const SegmentInfo& segmentInfo, bool& stop) {
            if ( strcmp(segmentInfo.segName, "__TEXT") == 0 ) {
                textSegVmAddr = segmentInfo.vmAddr;
            }
            else if ( strcmp(segmentInfo.segName, "__LINKEDIT") == 0 ) {
                uint64_t vmOffset = segmentInfo.vmAddr - textSegVmAddr;
                // A zero fill page in the __DATA segment means the file offset of __LINKEDIT is less than its vm offset
                if ( segmentInfo.fileOffset != vmOffset )
                    hasZeroFill = true;
                vmSpaceRequired = segmentInfo.vmAddr + segmentInfo.vmSize - textSegVmAddr;
                stop = true;
            }
        });
        return hasZeroFill;
    };

    if (hasZeroFill()) {
        vm_address_t newMappedAddr;
        if ( ::vm_allocate(mach_task_self(), &newMappedAddr, (size_t)vmSpaceRequired, VM_FLAGS_ANYWHERE) != 0 ) {
            diag.error("vm_allocate failure");
            return nullptr;
        }
        // mmap() each segment read-only with standard layout
        __block uint64_t textSegVmAddr;
        forEachSegment(^(const SegmentInfo& segmentInfo, bool& stop) {
            if ( strcmp(segmentInfo.segName, "__TEXT") == 0 )
                textSegVmAddr = segmentInfo.vmAddr;
            if ( segmentInfo.fileSize != 0 ) {
                kern_return_t r = vm_copy(mach_task_self(), (vm_address_t)((long)info.fileContent+segmentInfo.fileOffset), (vm_size_t)segmentInfo.fileSize, (vm_address_t)(newMappedAddr+segmentInfo.vmAddr-textSegVmAddr));
                if ( r != KERN_SUCCESS ) {
                    diag.error("vm_copy() failure");
                    stop = true;
                }
            }
        });
        if ( diag.noError() ) {
            // remove original mapping and return new mapping
            fileSystem.unloadFile(info);

            // Set vm_deallocate as the unload method.
            info.unload = [](const closure::LoadedFileInfo& info) {
                ::vm_deallocate(mach_task_self(), (vm_address_t)info.fileContent, (size_t)info.fileContentLen);
            };

            // And update the file content to the new location
            info.fileContent = (const void*)newMappedAddr;
            info.fileContentLen = vmSpaceRequired;
            return (const MachOAnalyzer*)info.fileContent;
        }
        else {
            // new mapping failed, return old mapping with an error in diag
            ::vm_deallocate(mach_task_self(), newMappedAddr, (size_t)vmSpaceRequired);
            return nullptr;
        }
    }

    return this;
}

bool MachOAnalyzer::enforceFormat(Malformed kind) const
{
#if TARGET_OS_OSX
    __block bool result = false;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        if ( platform == Platform::macOS ) {
            switch (kind) {
            case Malformed::linkeditOrder:
            case Malformed::linkeditAlignment:
            case Malformed::dyldInfoAndlocalRelocs:
                // enforce these checks on new binaries only
                result = (sdk >= 0x000A0E00); // macOS 10.14
            }
        }
    });
    // if binary is so old, there is no platform info, don't enforce malformed errors
    return result;
#else
    return true;
#endif
}

bool MachOAnalyzer::validEmbeddedPaths(Diagnostics& diag, const char* path) const
{
    __block int  index = 1;
    __block bool allGood = true;
    __block bool foundInstallName = false;
    __block int  dependentsCount = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        const dylib_command* dylibCmd;
        const rpath_command* rpathCmd;
        switch ( cmd->cmd ) {
            case LC_ID_DYLIB:
                foundInstallName = true;
                // fall through
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                if ( dylibCmd->dylib.name.offset > cmd->cmdsize ) {
                    diag.error("in '%s' load command #%d name offset (%u) outside its size (%u)", path, index, dylibCmd->dylib.name.offset, cmd->cmdsize);
                    stop = true;
                    allGood = false;
                }
                else {
                    bool foundEnd = false;
                    const char* start = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                    const char* end   = (char*)dylibCmd + cmd->cmdsize;
                    for (const char* s=start; s < end; ++s) {
                        if ( *s == '\0' ) {
                            foundEnd = true;
                            break;
                        }
                    }
                    if ( !foundEnd ) {
                        diag.error("in '%s' load command #%d string extends beyond end of load command", path, index);
                        stop = true;
                        allGood = false;
                    }
                }
                if ( cmd->cmd  != LC_ID_DYLIB )
                    ++dependentsCount;
                break;
            case LC_RPATH:
                rpathCmd = (rpath_command*)cmd;
                if ( rpathCmd->path.offset > cmd->cmdsize ) {
                    diag.error("in '%s' load command #%d path offset (%u) outside its size (%u)", path, index, rpathCmd->path.offset, cmd->cmdsize);
                    stop = true;
                    allGood = false;
                }
                else {
                    bool foundEnd = false;
                    const char* start = (char*)rpathCmd + rpathCmd->path.offset;
                    const char* end   = (char*)rpathCmd + cmd->cmdsize;
                    for (const char* s=start; s < end; ++s) {
                        if ( *s == '\0' ) {
                            foundEnd = true;
                            break;
                        }
                    }
                    if ( !foundEnd ) {
                        diag.error("in '%s' load command #%d string extends beyond end of load command", path, index);
                        stop = true;
                        allGood = false;
                    }
                }
                break;
        }
        ++index;
    });
    if ( !allGood )
        return false;

    if ( this->filetype == MH_DYLIB ) {
        if ( !foundInstallName ) {
            diag.error("in '%s' MH_DYLIB is missing LC_ID_DYLIB", path);
            return false;
        }
    }
    else {
        if ( foundInstallName ) {
            diag.error("in '%s' LC_ID_DYLIB found in non-MH_DYLIB", path);
            return false;
        }
    }

    if ( (dependentsCount == 0) && (this->filetype == MH_EXECUTE)  ) {
        diag.error("in '%s' missing LC_LOAD_DYLIB (must link with at least libSystem.dylib)", path);
        return false;
    }

    return true;
}

bool MachOAnalyzer::validSegments(Diagnostics& diag, const char* path, size_t fileLen) const
{
    // check segment load command size
    __block bool badSegmentLoadCommand = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            int32_t sectionsSpace = cmd->cmdsize - sizeof(segment_command_64);
            if ( sectionsSpace < 0 ) {
               diag.error("in '%s' load command size too small for LC_SEGMENT_64", path);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (sectionsSpace % sizeof(section_64)) != 0 ) {
               diag.error("in '%s' segment load command size 0x%X will not fit whole number of sections", path, cmd->cmdsize);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( sectionsSpace != (seg->nsects * sizeof(section_64)) ) {
               diag.error("in '%s' load command size 0x%X does not match nsects %d", path, cmd->cmdsize, seg->nsects);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( greaterThanAddOrOverflow(seg->fileoff, seg->filesize, fileLen) ) {
                diag.error("in '%s' segment load command content extends beyond end of file", path);
                badSegmentLoadCommand = true;
                stop = true;
            }
            else if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) ) {
                // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
                diag.error("in '%s' segment filesize exceeds vmsize", path);
                badSegmentLoadCommand = true;
                stop = true;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            int32_t sectionsSpace = cmd->cmdsize - sizeof(segment_command);
            if ( sectionsSpace < 0 ) {
               diag.error("in '%s' load command size too small for LC_SEGMENT", path);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (sectionsSpace % sizeof(section)) != 0 ) {
               diag.error("in '%s' segment load command size 0x%X will not fit whole number of sections", path, cmd->cmdsize);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( sectionsSpace != (seg->nsects * sizeof(section)) ) {
               diag.error("in '%s' load command size 0x%X does not match nsects %d", path, cmd->cmdsize, seg->nsects);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) ) {
                // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
                diag.error("in '%s' segment filesize exceeds vmsize", path);
                badSegmentLoadCommand = true;
                stop = true;
            }
        }
    });
     if ( badSegmentLoadCommand )
         return false;

    // check mapping permissions of segments
    __block bool badPermissions = false;
    __block bool badSize        = false;
    __block bool hasTEXT        = false;
    __block bool hasLINKEDIT    = false;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            if ( info.protections != (VM_PROT_READ|VM_PROT_EXECUTE) ) {
                diag.error("in '%s' __TEXT segment permissions is not 'r-x'", path);
                badPermissions = true;
                stop = true;
            }
            hasTEXT = true;
        }
        else if ( strcmp(info.segName, "__LINKEDIT") == 0 ) {
            if ( info.protections != VM_PROT_READ ) {
                diag.error("in '%s' __LINKEDIT segment permissions is not 'r--'", path);
                badPermissions = true;
                stop = true;
            }
            hasLINKEDIT = true;
        }
        else if ( (info.protections & 0xFFFFFFF8) != 0 ) {
            diag.error("in '%s' %s segment permissions has invalid bits set", path, info.segName);
            badPermissions = true;
            stop = true;
        }
        if ( greaterThanAddOrOverflow(info.fileOffset, info.fileSize, fileLen) ) {
            diag.error("in '%s' %s segment content extends beyond end of file", path, info.segName);
            badSize = true;
            stop = true;
        }
        if ( is64() ) {
            if ( info.vmAddr+info.vmSize < info.vmAddr ) {
                diag.error("in '%s' %s segment vm range wraps", path, info.segName);
                badSize = true;
                stop = true;
            }
       }
       else {
            if ( (uint32_t)(info.vmAddr+info.vmSize) < (uint32_t)(info.vmAddr) ) {
                diag.error("in '%s' %s segment vm range wraps", path, info.segName);
                badSize = true;
                stop = true;
            }
       }
    });
    if ( badPermissions || badSize )
        return false;
    if ( !hasTEXT ) {
       diag.error("in '%s' missing __TEXT segment", path);
       return false;
    }
    if ( !hasLINKEDIT ) {
       diag.error("in '%s' missing __LINKEDIT segment", path);
       return false;
    }

    // check for overlapping segments
    __block bool badSegments = false;
    forEachSegment(^(const SegmentInfo& info1, bool& stop1) {
        uint64_t seg1vmEnd   = info1.vmAddr + info1.vmSize;
        uint64_t seg1FileEnd = info1.fileOffset + info1.fileSize;
        forEachSegment(^(const SegmentInfo& info2, bool& stop2) {
            if ( info1.segIndex == info2.segIndex )
                return;
            uint64_t seg2vmEnd   = info2.vmAddr + info2.vmSize;
            uint64_t seg2FileEnd = info2.fileOffset + info2.fileSize;
            if ( ((info2.vmAddr <= info1.vmAddr) && (seg2vmEnd > info1.vmAddr) && (seg1vmEnd > info1.vmAddr )) || ((info2.vmAddr >= info1.vmAddr ) && (info2.vmAddr < seg1vmEnd) && (seg2vmEnd > info2.vmAddr)) ) {
                diag.error("in '%s' segment %s vm range overlaps segment %s", path, info1.segName, info2.segName);
                badSegments = true;
                stop1 = true;
                stop2 = true;
            }
             if ( ((info2.fileOffset  <= info1.fileOffset) && (seg2FileEnd > info1.fileOffset) && (seg1FileEnd > info1.fileOffset)) || ((info2.fileOffset  >= info1.fileOffset) && (info2.fileOffset  < seg1FileEnd) && (seg2FileEnd > info2.fileOffset )) ) {
                diag.error("in '%s' segment %s file content overlaps segment %s", path, info1.segName, info2.segName);
                badSegments = true;
                stop1 = true;
                stop2 = true;
            }
            if ( (info1.segIndex < info2.segIndex) && !stop1 ) {
                if ( (info1.vmAddr > info2.vmAddr) || ((info1.fileOffset > info2.fileOffset ) && (info1.fileOffset != 0) && (info2.fileOffset  != 0)) ){
                    if ( !inDyldCache() ) {
                        // dyld cache __DATA_* segments are moved around
                        diag.error("in '%s' segment load commands out of order with respect to layout for %s and %s", path, info1.segName, info2.segName);
                        badSegments = true;
                        stop1 = true;
                        stop2 = true;
                    }
                }
            }
        });
    });
    if ( badSegments )
        return false;

    // check sections are within segment
    __block bool badSections = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            const section_64* const sectionsStart = (section_64*)((char*)seg + sizeof(struct segment_command_64));
            const section_64* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section_64* sect=sectionsStart; (sect < sectionsEnd); ++sect) {
                if ( (int64_t)(sect->size) < 0 ) {
                    diag.error("in '%s' section %s size too large 0x%llX", path, sect->sectname, sect->size);
                    badSections = true;
                }
                else if ( sect->addr < seg->vmaddr ) {
                    diag.error("in '%s' section %s start address 0x%llX is before containing segment's address 0x%0llX", path, sect->sectname, sect->addr, seg->vmaddr);
                    badSections = true;
                }
                else if ( sect->addr+sect->size > seg->vmaddr+seg->vmsize ) {
                    diag.error("in '%s' section %s end address 0x%llX is beyond containing segment's end address 0x%0llX", path, sect->sectname, sect->addr+sect->size, seg->vmaddr+seg->vmsize);
                    badSections = true;
                }
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            const section* const sectionsStart = (section*)((char*)seg + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
               if ( (int64_t)(sect->size) < 0 ) {
                    diag.error("in '%s' section %s size too large 0x%X", path, sect->sectname, sect->size);
                    badSections = true;
                }
                else if ( sect->addr < seg->vmaddr ) {
                    diag.error("in '%s' section %s start address 0x%X is before containing segment's address 0x%0X", path,  sect->sectname, sect->addr, seg->vmaddr);
                    badSections = true;
                }
                else if ( sect->addr+sect->size > seg->vmaddr+seg->vmsize ) {
                    diag.error("in '%s' section %s end address 0x%X is beyond containing segment's end address 0x%0X", path, sect->sectname, sect->addr+sect->size, seg->vmaddr+seg->vmsize);
                    badSections = true;
                }
            }
        }
    });

    return !badSections;
}


bool MachOAnalyzer::validMain(Diagnostics& diag, const char* path) const
{
    __block uint64_t textSegStartAddr = 0;
    __block uint64_t textSegStartSize = 0;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            textSegStartAddr = info.vmAddr;
            textSegStartSize = info.vmSize;
            stop = true;
       }
    });

    __block int mainCount   = 0;
    __block int threadCount = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        entry_point_command* mainCmd;
        uint64_t startAddress;
        switch (cmd->cmd) {
            case LC_MAIN:
                ++mainCount;
                mainCmd = (entry_point_command*)cmd;
                if ( mainCmd->entryoff > textSegStartSize ) {
                    diag.error("LC_MAIN points outside of __TEXT segment");
                    stop = true;
                }
                break;
            case LC_UNIXTHREAD:
                ++threadCount;
                startAddress = entryAddrFromThreadCmd((thread_command*)cmd);
                if ( startAddress == 0 ) {
                    diag.error("LC_UNIXTHREAD not valid for arch %s", archName());
                    stop = true;
                }
                else if ( (startAddress < textSegStartAddr) || (startAddress > textSegStartAddr+textSegStartSize) ) {
                    diag.error("LC_UNIXTHREAD entry not in __TEXT segment");
                    stop = true;
                }
                break;
        }
    });
    if ( diag.hasError() )
        return false;
    if ( diag.noError() && (mainCount+threadCount == 1) )
        return true;

    if ( mainCount + threadCount == 0 )
        diag.error("missing LC_MAIN or LC_UNIXTHREAD");
    else
        diag.error("only one LC_MAIN or LC_UNIXTHREAD is allowed");
    return false;
}


namespace {
    struct LinkEditContentChunk
    {
        const char* name;
        uint32_t    stdOrder;
        uint32_t    fileOffsetStart;
        uint32_t    size;

        static int compareByFileOffset(const void* l, const void* r) {
            if ( ((LinkEditContentChunk*)l)->fileOffsetStart < ((LinkEditContentChunk*)r)->fileOffsetStart )
                return -1;
            else
                return 1;
        }
        static int compareByStandardOrder(const void* l, const void* r) {
           if ( ((LinkEditContentChunk*)l)->stdOrder < ((LinkEditContentChunk*)r)->stdOrder )
                return -1;
            else
                return 1;
        }
    };
} // anonymous namespace



bool MachOAnalyzer::validLinkeditLayout(Diagnostics& diag, const char* path) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    const uint32_t ptrSize = pointerSize();

    // build vector of all blobs in LINKEDIT
    LinkEditContentChunk blobs[32];
    LinkEditContentChunk* bp = blobs;
    if ( leInfo.dyldInfo != nullptr ) {
        if ( leInfo.dyldInfo->rebase_size != 0 )
            *bp++ = {"rebase opcodes",         1, leInfo.dyldInfo->rebase_off, leInfo.dyldInfo->rebase_size};
        if ( leInfo.dyldInfo->bind_size != 0 )
            *bp++ = {"bind opcodes",           2, leInfo.dyldInfo->bind_off, leInfo.dyldInfo->bind_size};
        if ( leInfo.dyldInfo->weak_bind_size != 0 )
            *bp++ = {"weak bind opcodes",      3, leInfo.dyldInfo->weak_bind_off, leInfo.dyldInfo->weak_bind_size};
        if ( leInfo.dyldInfo->lazy_bind_size != 0 )
            *bp++ = {"lazy bind opcodes",      4, leInfo.dyldInfo->lazy_bind_off, leInfo.dyldInfo->lazy_bind_size};
        if ( leInfo.dyldInfo->export_size!= 0 )
            *bp++ = {"exports trie",           5, leInfo.dyldInfo->export_off, leInfo.dyldInfo->export_size};
    }
    if ( leInfo.dynSymTab != nullptr ) {
        if ( leInfo.dynSymTab->nlocrel != 0 )
            *bp++ = {"local relocations",      6, leInfo.dynSymTab->locreloff, static_cast<uint32_t>(leInfo.dynSymTab->nlocrel*sizeof(relocation_info))};
        if ( leInfo.dynSymTab->nextrel != 0 )
            *bp++ = {"external relocations",  11, leInfo.dynSymTab->extreloff, static_cast<uint32_t>(leInfo.dynSymTab->nextrel*sizeof(relocation_info))};
        if ( leInfo.dynSymTab->nindirectsyms != 0 )
            *bp++ = {"indirect symbol table", 12, leInfo.dynSymTab->indirectsymoff, leInfo.dynSymTab->nindirectsyms*4};
    }
    if ( leInfo.splitSegInfo != nullptr ) {
        if ( leInfo.splitSegInfo->datasize != 0 )
            *bp++ = {"shared cache info",      6, leInfo.splitSegInfo->dataoff, leInfo.splitSegInfo->datasize};
    }
    if ( leInfo.functionStarts != nullptr ) {
        if ( leInfo.functionStarts->datasize != 0 )
            *bp++ = {"function starts",        7, leInfo.functionStarts->dataoff, leInfo.functionStarts->datasize};
    }
    if ( leInfo.dataInCode != nullptr ) {
        if ( leInfo.dataInCode->datasize != 0 )
            *bp++ = {"data in code",           8, leInfo.dataInCode->dataoff, leInfo.dataInCode->datasize};
    }
    if ( leInfo.symTab != nullptr ) {
        if ( leInfo.symTab->nsyms != 0 )
            *bp++ = {"symbol table",         10, leInfo.symTab->symoff, static_cast<uint32_t>(leInfo.symTab->nsyms*(ptrSize == 8 ? sizeof(nlist_64) : sizeof(struct nlist)))};
        if ( leInfo.symTab->strsize != 0 )
            *bp++ = {"symbol table strings", 20, leInfo.symTab->stroff, leInfo.symTab->strsize};
    }
    if ( leInfo.codeSig != nullptr ) {
        if ( leInfo.codeSig->datasize != 0 )
            *bp++ = {"code signature",       21, leInfo.codeSig->dataoff, leInfo.codeSig->datasize};
    }

    // check for bad combinations
    if ( (leInfo.dyldInfo != nullptr) && (leInfo.dyldInfo->cmd == LC_DYLD_INFO_ONLY) && (leInfo.dynSymTab != nullptr) ) {
        if ( (leInfo.dynSymTab->nlocrel != 0) && enforceFormat(Malformed::dyldInfoAndlocalRelocs) ) {
            diag.error("in '%s' malformed mach-o contains LC_DYLD_INFO_ONLY and local relocations", path);
            return false;
        }
        if ( leInfo.dynSymTab->nextrel != 0 ) {
            diag.error("in '%s' malformed mach-o contains LC_DYLD_INFO_ONLY and external relocations", path);
            return false;
        }
    }
    if ( (leInfo.dyldInfo == nullptr) && (leInfo.dynSymTab == nullptr) ) {
        diag.error("in '%s' malformed mach-o misssing LC_DYLD_INFO and LC_DYSYMTAB", path);
        return false;
    }
    const unsigned long blobCount = bp - blobs;
    if ( blobCount == 0 ) {
        diag.error("in '%s' malformed mach-o misssing LINKEDIT", path);
        return false;
    }

    uint32_t linkeditFileEnd = leInfo.layout.linkeditFileOffset + leInfo.layout.linkeditFileSize;


    // sort blobs by file-offset and error on overlaps
    ::qsort(blobs, blobCount, sizeof(LinkEditContentChunk), &LinkEditContentChunk::compareByFileOffset);
    uint32_t     prevEnd = leInfo.layout.linkeditFileOffset;
    const char*  prevName = "start of LINKEDIT";
    for (unsigned long i=0; i < blobCount; ++i) {
        const LinkEditContentChunk& blob = blobs[i];
        if ( blob.fileOffsetStart < prevEnd ) {
            diag.error("in '%s' LINKEDIT overlap of %s and %s", path, prevName, blob.name);
            return false;
        }
        if (greaterThanAddOrOverflow(blob.fileOffsetStart, blob.size, linkeditFileEnd)) {
            diag.error("in '%s' LINKEDIT content '%s' extends beyond end of segment", path, blob.name);
            return false;
        }
        prevEnd  = blob.fileOffsetStart + blob.size;
        prevName = blob.name;
    }

    // sort vector by order and warn on non standard order or mis-alignment
    ::qsort(blobs, blobCount, sizeof(LinkEditContentChunk), &LinkEditContentChunk::compareByStandardOrder);
    prevEnd = leInfo.layout.linkeditFileOffset;
    for (unsigned long i=0; i < blobCount; ++i) {
        const LinkEditContentChunk& blob = blobs[i];
        if ( ((blob.fileOffsetStart & (ptrSize-1)) != 0) && (blob.stdOrder != 20) && enforceFormat(Malformed::linkeditAlignment) )  // ok for "symbol table strings" to be mis-aligned
            diag.error("in '%s' mis-aligned LINKEDIT content '%s'", path, blob.name);
        if ( (blob.fileOffsetStart < prevEnd) && enforceFormat(Malformed::linkeditOrder) ) {
            diag.error("in '%s' LINKEDIT out of order %s", path, blob.name);
        }
        prevEnd  = blob.fileOffsetStart;
    }

    // Check for invalid symbol table sizes
    if ( leInfo.symTab != nullptr ) {
        if ( leInfo.symTab->nsyms > 0x10000000 ) {
            diag.error("in '%s' malformed mach-o image: symbol table too large", path);
            return false;
        }
        if ( leInfo.dynSymTab != nullptr ) {
            // validate indirect symbol table
            if ( leInfo.dynSymTab->nindirectsyms != 0 ) {
                if ( leInfo.dynSymTab->nindirectsyms > 0x10000000 ) {
                    diag.error("in '%s' malformed mach-o image: indirect symbol table too large", path);
                    return false;
                }
            }
            if ( (leInfo.dynSymTab->nlocalsym > leInfo.symTab->nsyms) || (leInfo.dynSymTab->ilocalsym > leInfo.symTab->nsyms) ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table local symbol count exceeds total symbols", path);
                return false;
            }
            if ( leInfo.dynSymTab->ilocalsym + leInfo.dynSymTab->nlocalsym < leInfo.dynSymTab->ilocalsym  ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table local symbol count wraps", path);
                return false;
            }
            if ( (leInfo.dynSymTab->nextdefsym > leInfo.symTab->nsyms) || (leInfo.dynSymTab->iextdefsym > leInfo.symTab->nsyms) ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table extern symbol count exceeds total symbols", path);
                return false;
            }
            if ( leInfo.dynSymTab->iextdefsym + leInfo.dynSymTab->nextdefsym < leInfo.dynSymTab->iextdefsym  ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table extern symbol count wraps", path);
                return false;
            }
            if ( (leInfo.dynSymTab->nundefsym > leInfo.symTab->nsyms) || (leInfo.dynSymTab->iundefsym > leInfo.symTab->nsyms) ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table undefined symbol count exceeds total symbols", path);
                return false;
            }
            if ( leInfo.dynSymTab->iundefsym + leInfo.dynSymTab->nundefsym < leInfo.dynSymTab->iundefsym  ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table undefined symbol count wraps", path);
                return false;
            }
        }
    }

    return true;
}



bool MachOAnalyzer::invalidRebaseState(Diagnostics& diag, const char* opcodeName, const char* path, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                                      bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type) const
{
    if ( !segIndexSet ) {
        diag.error("in '%s' %s missing preceding REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", path, opcodeName);
        return true;
    }
    if ( segmentIndex >= leInfo.layout.linkeditSegIndex )  {
        diag.error("in '%s' %s segment index %d too large", path, opcodeName, segmentIndex);
        return true;
    }
    if ( segmentOffset > (segments[segmentIndex].vmSize-ptrSize) ) {
        diag.error("in '%s' %s current segment offset 0x%08llX beyond segment size (0x%08llX)", path, opcodeName, segmentOffset, segments[segmentIndex].vmSize);
        return true;
    }
    switch ( type )  {
        case REBASE_TYPE_POINTER:
            if ( !segments[segmentIndex].writable() ) {
                diag.error("in '%s' %s pointer rebase is in non-writable segment", path, opcodeName);
                return true;
            }
            if ( segments[segmentIndex].executable() ) {
                diag.error("in '%s' %s pointer rebase is in executable segment", path, opcodeName);
                return true;
            }
            break;
        case REBASE_TYPE_TEXT_ABSOLUTE32:
        case REBASE_TYPE_TEXT_PCREL32:
            if ( !segments[segmentIndex].textRelocs ) {
                diag.error("in '%s' %s text rebase is in segment that does not support text relocations", path, opcodeName);
                return true;
            }
            if ( segments[segmentIndex].writable() ) {
                diag.error("in '%s' %s text rebase is in writable segment", path, opcodeName);
                return true;
            }
            if ( !segments[segmentIndex].executable() ) {
                diag.error("in '%s' %s pointer rebase is in non-executable segment", path, opcodeName);
                return true;
            }
            break;
        default:
            diag.error("in '%s' %s unknown rebase type %d", path, opcodeName, type);
            return true;
    }
    return false;
}


void MachOAnalyzer::getAllSegmentsInfos(Diagnostics& diag, SegmentInfo segments[]) const
{
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        segments[info.segIndex] = info;
    });
}


bool MachOAnalyzer::validRebaseInfo(Diagnostics& diag, const char* path) const
{
    forEachRebase(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                          bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, bool& stop) {
        if ( invalidRebaseState(diag, opcodeName, path, leInfo, segments, segIndexSet, ptrSize, segmentIndex, segmentOffset, type) )
            stop = true;
    });
    return diag.noError();
}


void MachOAnalyzer::forEachTextRebase(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, bool& stop)) const
{
    __block bool     startVmAddrSet = false;
    __block uint64_t startVmAddr    = 0;
    forEachRebase(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                          bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, bool& stop) {
        if ( type != REBASE_TYPE_TEXT_ABSOLUTE32 )
            return;
        if ( !startVmAddrSet ) {
            for (int i=0; i <= segmentIndex; ++i) {
                if ( strcmp(segments[i].segName, "__TEXT") == 0 ) {
                    startVmAddr = segments[i].vmAddr;
                    startVmAddrSet = true;
                    break;
               }
            }
        }
        uint64_t rebaseVmAddr  = segments[segmentIndex].vmAddr + segmentOffset;
        uint64_t runtimeOffset = rebaseVmAddr - startVmAddr;
        handler(runtimeOffset, stop);
    });
}


void MachOAnalyzer::forEachRebase(Diagnostics& diag, bool ignoreLazyPointers, void (^handler)(uint64_t runtimeOffset, bool& stop)) const
{
    __block bool     startVmAddrSet = false;
    __block uint64_t startVmAddr    = 0;
    __block uint64_t lpVmAddr       = 0;
    __block uint64_t lpEndVmAddr    = 0;
    __block uint64_t shVmAddr       = 0;
    __block uint64_t shEndVmAddr    = 0;
    if ( ignoreLazyPointers ) {
        forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& info, bool malformedSectionRange, bool &stop) {
            if ( (info.sectFlags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS ) {
                lpVmAddr    = info.sectAddr;
                lpEndVmAddr = info.sectAddr + info.sectSize;
            }
            else if ( (info.sectFlags & S_ATTR_PURE_INSTRUCTIONS) && (strcmp(info.sectName, "__stub_helper") == 0) ) {
                shVmAddr    = info.sectAddr;
                shEndVmAddr = info.sectAddr + info.sectSize;
            }
        });
    }
    forEachRebase(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                          bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, bool& stop) {
        if ( type != REBASE_TYPE_POINTER )
            return;
        if ( !startVmAddrSet ) {
            for (int i=0; i < segmentIndex; ++i) {
                if ( strcmp(segments[i].segName, "__TEXT") == 0 ) {
                    startVmAddr = segments[i].vmAddr;
                    startVmAddrSet = true;
                    break;
               }
            }
        }
        uint64_t rebaseVmAddr  = segments[segmentIndex].vmAddr + segmentOffset;
        bool skipRebase = false;
        if ( (rebaseVmAddr >= lpVmAddr) && (rebaseVmAddr < lpEndVmAddr) ) {
            // rebase is in lazy pointer section
            uint64_t lpValue = 0;
            if ( ptrSize == 8 )
                lpValue = *((uint64_t*)(rebaseVmAddr-startVmAddr+(uint8_t*)this));
            else
                lpValue = *((uint32_t*)(rebaseVmAddr-startVmAddr+(uint8_t*)this));
            if ( (lpValue >= shVmAddr) && (lpValue < shEndVmAddr) ) {
                // content is into stub_helper section
                uint64_t lpTargetImageOffset = lpValue - startVmAddr;
                const uint8_t* helperContent = (uint8_t*)this + lpTargetImageOffset;
                bool isLazyStub = contentIsRegularStub(helperContent);
                // ignore rebases for normal lazy pointers, but leave rebase for resolver helper stub
                if ( isLazyStub )
                    skipRebase = true;
            }
            else {
                // if lazy pointer does not point into stub_helper, then it points to weak-def symbol and we need rebase
            }
        }
        if ( !skipRebase ) {
            uint64_t runtimeOffset = rebaseVmAddr - startVmAddr;
            handler(runtimeOffset, stop);
        }
    });
}


bool MachOAnalyzer::contentIsRegularStub(const uint8_t* helperContent) const
{
    switch (this->cputype) {
        case CPU_TYPE_X86_64:
            return ( (helperContent[0] == 0x68) && (helperContent[5] == 0xE9) ); // push $xxx / JMP pcRel
        case CPU_TYPE_I386:
            return ( (helperContent[0] == 0x68) && (helperContent[5] == 0xFF) && (helperContent[2] == 0x26) ); // push $xxx / JMP *pcRel
        case CPU_TYPE_ARM:
            return ( (helperContent[0] == 0x00) && (helperContent[1] == 0xC0) && (helperContent[2] == 0x9F) && (helperContent[3] == 0xE5) ); // ldr  ip, [pc, #0]
        case CPU_TYPE_ARM64:
            return ( (helperContent[0] == 0x50) && (helperContent[1] == 0x00) && (helperContent[2] == 0x00) && (helperContent[3] == 0x18) ); // ldr w16, L0

    }
    return false;
}

static int uint32Sorter(const void* l, const void* r) {
    if ( *((uint32_t*)l) < *((uint32_t*)r) )
        return -1;
    else
        return 1;
}


void MachOAnalyzer::forEachRebase(Diagnostics& diag,
                                 void (^handler)(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                                                 bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                                 uint8_t type, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(SegmentInfo, segmentsInfo, leInfo.layout.linkeditSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;

    if ( leInfo.dyldInfo != nullptr ) {
        const uint8_t* p    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->rebase_off);
        const uint8_t* end  = p + leInfo.dyldInfo->rebase_size;
        const uint32_t ptrSize = pointerSize();
        uint8_t  type = 0;
        int      segIndex = 0;
        uint64_t segOffset = 0;
        uint64_t count;
        uint64_t skip;
        bool     segIndexSet = false;
        bool     stop = false;
        while ( !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & REBASE_IMMEDIATE_MASK;
            uint8_t opcode = *p & REBASE_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case REBASE_OPCODE_DONE:
                    stop = true;
                    break;
                case REBASE_OPCODE_SET_TYPE_IMM:
                    type = immediate;
                    break;
                case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segIndex = immediate;
                    segOffset = read_uleb128(diag, p, end);
                    segIndexSet = true;
                    break;
                case REBASE_OPCODE_ADD_ADDR_ULEB:
                    segOffset += read_uleb128(diag, p, end);
                    break;
                case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                    segOffset += immediate*ptrSize;
                    break;
                case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                    for (int i=0; i < immediate; ++i) {
                        handler("REBASE_OPCODE_DO_REBASE_IMM_TIMES", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, type, stop);
                        segOffset += ptrSize;
                        if ( stop )
                            break;
                    }
                    break;
                case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                    count = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, type, stop);
                        segOffset += ptrSize;
                        if ( stop )
                            break;
                    }
                    break;
                case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                    handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, type, stop);
                    segOffset += read_uleb128(diag, p, end) + ptrSize;
                    break;
                case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                    count = read_uleb128(diag, p, end);
                    if ( diag.hasError() )
                        break;
                    skip = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        handler("REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, type, stop);
                        segOffset += skip + ptrSize;
                        if ( stop )
                            break;
                    }
                    break;
                default:
                    diag.error("unknown rebase opcode 0x%02X", opcode);
            }
        }
    }
    else {
        // old binary, walk relocations
        const uint64_t                  relocsStartAddress = relocBaseAddress(segmentsInfo, leInfo.layout.linkeditSegIndex);
        const relocation_info* const    relocsStart = (relocation_info*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->locreloff);
        const relocation_info* const    relocsEnd   = &relocsStart[leInfo.dynSymTab->nlocrel];
        bool                            stop = false;
        const uint8_t                   relocSize = (is64() ? 3 : 2);
        const uint8_t                   ptrSize   = pointerSize();
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(uint32_t, relocAddrs, 2048);
        for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
            if ( reloc->r_length != relocSize ) {
                diag.error("local relocation has wrong r_length");
                break;
            }
            if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA ==  ARM64_RELOC_UNSIGNED
                diag.error("local relocation has wrong r_type");
                break;
            }
            relocAddrs.push_back(reloc->r_address);
        }
        if ( !relocAddrs.empty() ) {
            ::qsort(&relocAddrs[0], relocAddrs.count(), sizeof(uint32_t), &uint32Sorter);
            for (uint32_t addrOff : relocAddrs) {
                uint32_t segIndex  = 0;
                uint64_t segOffset = 0;
                if ( segIndexAndOffsetForAddress(relocsStartAddress+addrOff, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                    uint8_t type = REBASE_TYPE_POINTER;
                    if ( this->cputype == CPU_TYPE_I386 ) {
                        if ( segmentsInfo[segIndex].executable() )
                            type = REBASE_TYPE_TEXT_ABSOLUTE32;
                    }
                    handler("local relocation", leInfo, segmentsInfo, true, ptrSize, segIndex, segOffset, type , stop);
                }
                else {
                    diag.error("local relocation has out of range r_address");
                    break;
                }
            }
        }
        // then process indirect symbols
        forEachIndirectPointer(diag, ^(uint64_t address, bool bind, int bindLibOrdinal,
                                       const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
            if ( bind )
               return;
            uint32_t segIndex  = 0;
            uint64_t segOffset = 0;
            if ( segIndexAndOffsetForAddress(address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                handler("local relocation", leInfo, segmentsInfo, true, ptrSize, segIndex, segOffset, REBASE_TYPE_POINTER, indStop);
            }
            else {
                diag.error("local relocation has out of range r_address");
                indStop = true;
            }
        });
    }
}

bool MachOAnalyzer::segIndexAndOffsetForAddress(uint64_t addr, const SegmentInfo segmentsInfos[], uint32_t segCount, uint32_t& segIndex, uint64_t& segOffset) const
{
    for (uint32_t i=0; i < segCount; ++i) {
        if ( (segmentsInfos[i].vmAddr <= addr) && (addr < segmentsInfos[i].vmAddr+segmentsInfos[i].vmSize) ) {
            segIndex  = i;
            segOffset = addr - segmentsInfos[i].vmAddr;
            return true;
        }
    }
    return false;
}

uint64_t MachOAnalyzer::relocBaseAddress(const SegmentInfo segmentsInfos[], uint32_t segCount) const
{
    if ( is64() ) {
        // x86_64 reloc base address is first writable segment
        for (uint32_t i=0; i < segCount; ++i) {
            if ( segmentsInfos[i].writable() )
                return segmentsInfos[i].vmAddr;
        }
    }
    return segmentsInfos[0].vmAddr;
}



void MachOAnalyzer::forEachIndirectPointer(Diagnostics& diag, void (^handler)(uint64_t pointerAddress, bool bind, int bindLibOrdinal, const char* bindSymbolName, 
                                                                             bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    // find lazy and non-lazy pointer sections
    const bool              is64Bit                  = is64();
    const uint32_t* const   indirectSymbolTable      = (uint32_t*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->indirectsymoff);
    const uint32_t          indirectSymbolTableCount = leInfo.dynSymTab->nindirectsyms;
    const uint32_t          ptrSize                  = pointerSize();
    const void*             symbolTable              = getLinkEditContent(leInfo.layout, leInfo.symTab->symoff);
    const struct nlist_64*  symbols64                = (nlist_64*)symbolTable;
    const struct nlist*     symbols32                = (struct nlist*)symbolTable;
    const char*             stringPool               = (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
    uint32_t                symCount                 = leInfo.symTab->nsyms;
    uint32_t                poolSize                 = leInfo.symTab->strsize;
    __block bool            stop                     = false;
    forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& sectInfo, bool malformedSectionRange, bool& sectionStop) {
        uint8_t  sectionType  = (sectInfo.sectFlags & SECTION_TYPE);
        bool selfModifyingStub = (sectionType == S_SYMBOL_STUBS) && (sectInfo.sectFlags & S_ATTR_SELF_MODIFYING_CODE) && (sectInfo.reserved2 == 5) && (this->cputype == CPU_TYPE_I386);
        if ( (sectionType != S_LAZY_SYMBOL_POINTERS) && (sectionType != S_NON_LAZY_SYMBOL_POINTERS) && !selfModifyingStub )
            return;
        if ( (flags & S_ATTR_SELF_MODIFYING_CODE) && !selfModifyingStub ) {
            diag.error("S_ATTR_SELF_MODIFYING_CODE section type only valid in old i386 binaries");
            sectionStop = true;
            return;
        }
        uint32_t elementSize = selfModifyingStub ? sectInfo.reserved2 : ptrSize;
        uint32_t elementCount = (uint32_t)(sectInfo.sectSize/elementSize);
        if ( greaterThanAddOrOverflow(sectInfo.reserved1, elementCount, indirectSymbolTableCount) ) {
            diag.error("section %s overflows indirect symbol table", sectInfo.sectName);
            sectionStop = true;
            return;
        }

        for (uint32_t i=0; (i < elementCount) && !stop; ++i) {
            uint32_t symNum = indirectSymbolTable[sectInfo.reserved1 + i];
            if ( symNum == INDIRECT_SYMBOL_ABS )
                continue;
            if ( symNum == INDIRECT_SYMBOL_LOCAL ) {
                handler(sectInfo.sectAddr+i*elementSize, false, 0, "", false, false, false, stop);
                continue;
            }
            if ( symNum > symCount ) {
                diag.error("indirect symbol[%d] = %d which is invalid symbol index", sectInfo.reserved1 + i, symNum);
                sectionStop = true;
                return;
            }
            uint16_t n_desc = is64Bit ? symbols64[symNum].n_desc : symbols32[symNum].n_desc;
            uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
            uint32_t strOffset = is64Bit ? symbols64[symNum].n_un.n_strx : symbols32[symNum].n_un.n_strx;
            if ( strOffset > poolSize ) {
               diag.error("symbol[%d] string offset out of range", sectInfo.reserved1 + i);
                sectionStop = true;
                return;
            }
            const char* symbolName  = stringPool + strOffset;
            bool        weakImport  = (n_desc & N_WEAK_REF);
            bool        lazy        = (sectionType == S_LAZY_SYMBOL_POINTERS);
            handler(sectInfo.sectAddr+i*elementSize, true, libOrdinal, symbolName, weakImport, lazy, selfModifyingStub, stop);
        }
        sectionStop = stop;
    });
}

int MachOAnalyzer::libOrdinalFromDesc(uint16_t n_desc) const
{
    // -flat_namespace is always flat lookup
    if ( (this->flags & MH_TWOLEVEL) == 0 )
        return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

    // extract byte from undefined symbol entry
    int libIndex = GET_LIBRARY_ORDINAL(n_desc);
    switch ( libIndex ) {
        case SELF_LIBRARY_ORDINAL:
            return BIND_SPECIAL_DYLIB_SELF;

        case DYNAMIC_LOOKUP_ORDINAL:
            return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

        case EXECUTABLE_ORDINAL:
            return BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE;
    }

    return libIndex;
}

bool MachOAnalyzer::validBindInfo(Diagnostics& diag, const char* path) const
{
    forEachBind(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                         bool segIndexSet, bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                         uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset,
                         uint8_t type, const char* symbolName, bool weakImport, uint64_t addend, bool& stop) {
        if ( invalidBindState(diag, opcodeName, path, leInfo, segments, segIndexSet, libraryOrdinalSet, dylibCount,
                              libOrdinal, ptrSize, segmentIndex, segmentOffset, type, symbolName) ) {
            stop = true;
        }
    }, ^(const char* symbolName) {
    });
    return diag.noError();
}

bool MachOAnalyzer::invalidBindState(Diagnostics& diag, const char* opcodeName, const char* path, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                                    bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint32_t ptrSize,
                                    uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, const char* symbolName) const
{
    if ( !segIndexSet ) {
        diag.error("in '%s' %s missing preceding BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", path, opcodeName);
        return true;
    }
    if ( segmentIndex >= leInfo.layout.linkeditSegIndex )  {
        diag.error("in '%s' %s segment index %d too large", path, opcodeName, segmentIndex);
        return true;
    }
    if ( segmentOffset > (segments[segmentIndex].vmSize-ptrSize) ) {
        diag.error("in '%s' %s current segment offset 0x%08llX beyond segment size (0x%08llX)", path, opcodeName, segmentOffset, segments[segmentIndex].vmSize);
        return true;
    }
    if ( symbolName == NULL ) {
        diag.error("in '%s' %s missing preceding BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM", path, opcodeName);
        return true;
    }
    if ( !libraryOrdinalSet ) {
        diag.error("in '%s' %s missing preceding BIND_OPCODE_SET_DYLIB_ORDINAL", path, opcodeName);
        return true;
    }
    if ( libOrdinal > (int)dylibCount ) {
        diag.error("in '%s' %s has library ordinal too large (%d) max (%d)", path, opcodeName, libOrdinal, dylibCount);
        return true;
    }
    if ( libOrdinal < BIND_SPECIAL_DYLIB_WEAK_DEF_COALESCE ) {
        diag.error("in '%s' %s has unknown library special ordinal (%d)", path, opcodeName, libOrdinal);
        return true;
    }
    switch ( type )  {
        case BIND_TYPE_POINTER:
            if ( !segments[segmentIndex].writable() ) {
                diag.error("in '%s' %s pointer bind is in non-writable segment", path, opcodeName);
                return true;
            }
            if ( segments[segmentIndex].executable() ) {
                diag.error("in '%s' %s pointer bind is in executable segment", path, opcodeName);
                return true;
            }
            break;
        case BIND_TYPE_TEXT_ABSOLUTE32:
        case BIND_TYPE_TEXT_PCREL32:
            if ( !segments[segmentIndex].textRelocs ) {
                diag.error("in '%s' %s text bind is in segment that does not support text relocations", path, opcodeName);
                return true;
            }
            if ( segments[segmentIndex].writable() ) {
                diag.error("in '%s' %s text bind is in writable segment", path, opcodeName);
                return true;
            }
            if ( !segments[segmentIndex].executable() ) {
                diag.error("in '%s' %s pointer bind is in non-executable segment", path, opcodeName);
                return true;
            }
            break;
        default:
            diag.error("in '%s' %s unknown bind type %d", path, opcodeName, type);
            return true;
    }
    return false;
}

void MachOAnalyzer::forEachBind(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, int libOrdinal, const char* symbolName,
                                                                  bool weakImport, uint64_t addend, bool& stop),
                                                  void (^strongHandler)(const char* symbolName)) const
{
    __block bool     startVmAddrSet = false;
    __block uint64_t startVmAddr    = 0;
    forEachBind(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                        bool segIndexSet, bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                        uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset,
                        uint8_t type, const char* symbolName, bool weakImport, uint64_t addend, bool& stop) {
       if ( !startVmAddrSet ) {
            for (int i=0; i <= segmentIndex; ++i) {
                if ( strcmp(segments[i].segName, "__TEXT") == 0 ) {
                    startVmAddr = segments[i].vmAddr;
                    startVmAddrSet = true;
                    break;
               }
            }
        }
        uint64_t bindVmOffset  = segments[segmentIndex].vmAddr + segmentOffset;
        uint64_t runtimeOffset = bindVmOffset - startVmAddr;
        handler(runtimeOffset, libOrdinal, symbolName, weakImport, addend, stop);
    }, ^(const char* symbolName) {
        strongHandler(symbolName);
    });
}

void MachOAnalyzer::forEachBind(Diagnostics& diag,
                                 void (^handler)(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                                                 bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                                 uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                                 uint8_t type, const char* symbolName, bool weakImport, uint64_t addend, bool& stop),
                                 void (^strongHandler)(const char* symbolName)) const
{
    const uint32_t  ptrSize = this->pointerSize();
    bool            stop    = false;

    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(SegmentInfo, segmentsInfo, leInfo.layout.linkeditSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;



    const uint32_t dylibCount = dependentDylibCount();

    if ( leInfo.dyldInfo != nullptr ) {
        // process bind opcodes
        const uint8_t*  p    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->bind_off);
        const uint8_t*  end  = p + leInfo.dyldInfo->bind_size;
        uint8_t         type = 0;
        uint64_t        segmentOffset = 0;
        uint8_t         segmentIndex = 0;
        const char*     symbolName = NULL;
        int             libraryOrdinal = 0;
        bool            segIndexSet = false;
        bool            libraryOrdinalSet = false;

        int64_t         addend = 0;
        uint64_t        count;
        uint64_t        skip;
        bool            weakImport = false;
        while ( !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
            uint8_t opcode = *p & BIND_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case BIND_OPCODE_DONE:
                    stop = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                    libraryOrdinal = immediate;
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                    libraryOrdinal = (int)read_uleb128(diag, p, end);
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                    // the special ordinals are negative numbers
                    if ( immediate == 0 )
                        libraryOrdinal = 0;
                    else {
                        int8_t signExtended = BIND_OPCODE_MASK | immediate;
                        libraryOrdinal = signExtended;
                    }
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                    weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                    symbolName = (char*)p;
                    while (*p != '\0')
                        ++p;
                    ++p;
                    break;
                case BIND_OPCODE_SET_TYPE_IMM:
                    type = immediate;
                    break;
                case BIND_OPCODE_SET_ADDEND_SLEB:
                    addend = read_sleb128(diag, p, end);
                    break;
                case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segmentIndex = immediate;
                    segmentOffset = read_uleb128(diag, p, end);
                    segIndexSet = true;
                    break;
                case BIND_OPCODE_ADD_ADDR_ULEB:
                    segmentOffset += read_uleb128(diag, p, end);
                    break;
                case BIND_OPCODE_DO_BIND:
                    handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                    segmentOffset += ptrSize;
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                    handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                    segmentOffset += read_uleb128(diag, p, end) + ptrSize;
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                    handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                    segmentOffset += immediate*ptrSize + ptrSize;
                    break;
                case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                    count = read_uleb128(diag, p, end);
                    skip = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                        segmentOffset += skip + ptrSize;
                        if ( stop )
                            break;
                    }
                    break;
                default:
                    diag.error("bad bind opcode 0x%02X", *p);
            }
        }
        if ( diag.hasError() )
            return;

        // process lazy bind opcodes
        if ( leInfo.dyldInfo->lazy_bind_size != 0 ) {
            p               = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->lazy_bind_off);
            end             = p + leInfo.dyldInfo->lazy_bind_size;
            type            = BIND_TYPE_POINTER;
            segmentOffset   = 0;
            segmentIndex    = 0;
            symbolName      = NULL;
            libraryOrdinal  = 0;
            segIndexSet     = false;
            libraryOrdinalSet= false;
            addend          = 0;
            weakImport      = false;
            stop            = false;
            while (  !stop && diag.noError() && (p < end) ) {
                uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
                uint8_t opcode = *p & BIND_OPCODE_MASK;
                ++p;
                switch (opcode) {
                    case BIND_OPCODE_DONE:
                        // this opcode marks the end of each lazy pointer binding
                        break;
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                        libraryOrdinal = immediate;
                        libraryOrdinalSet = true;
                        break;
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                        libraryOrdinal = (int)read_uleb128(diag, p, end);
                        libraryOrdinalSet = true;
                        break;
                    case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                        // the special ordinals are negative numbers
                        if ( immediate == 0 )
                            libraryOrdinal = 0;
                        else {
                            int8_t signExtended = BIND_OPCODE_MASK | immediate;
                            libraryOrdinal = signExtended;
                        }
                        libraryOrdinalSet = true;
                        break;
                    case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                        weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                        symbolName = (char*)p;
                        while (*p != '\0')
                            ++p;
                        ++p;
                        break;
                    case BIND_OPCODE_SET_ADDEND_SLEB:
                        addend = read_sleb128(diag, p, end);
                        break;
                    case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                        segmentIndex = immediate;
                        segmentOffset = read_uleb128(diag, p, end);
                        segIndexSet = true;
                        break;
                    case BIND_OPCODE_DO_BIND:
                        handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                        segmentOffset += ptrSize;
                        break;
                    case BIND_OPCODE_SET_TYPE_IMM:
                    case BIND_OPCODE_ADD_ADDR_ULEB:
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                    case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                    default:
                        diag.error("bad lazy bind opcode 0x%02X", opcode);
                        break;
                }
            }
        }
        if ( diag.hasError() )
            return;

        // process weak bind info
        if ( leInfo.dyldInfo->weak_bind_size != 0 ) {
            p               = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->weak_bind_off);
            end             = p + leInfo.dyldInfo->weak_bind_size;
            type            = BIND_TYPE_POINTER;
            segmentOffset   = 0;
            segmentIndex    = 0;
            symbolName      = NULL;
            libraryOrdinal  = BIND_SPECIAL_DYLIB_WEAK_DEF_COALESCE;
            segIndexSet     = false;
            libraryOrdinalSet= true;
            addend          = 0;
            weakImport      = false;
            stop            = false;
            while ( !stop && diag.noError() && (p < end) ) {
                uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
                uint8_t opcode = *p & BIND_OPCODE_MASK;
                ++p;
                switch (opcode) {
                    case BIND_OPCODE_DONE:
                        stop = true;
                        break;
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                    case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                        diag.error("unexpected dylib ordinal in weak_bind");
                        break;
                    case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                        weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                        symbolName = (char*)p;
                        while (*p != '\0')
                            ++p;
                        ++p;
                        if ( immediate & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION ) {
                            strongHandler(symbolName);
                        }
                        break;
                    case BIND_OPCODE_SET_TYPE_IMM:
                        type = immediate;
                        break;
                    case BIND_OPCODE_SET_ADDEND_SLEB:
                        addend = read_sleb128(diag, p, end);
                        break;
                    case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                        segmentIndex = immediate;
                        segmentOffset = read_uleb128(diag, p, end);
                        segIndexSet = true;
                        break;
                    case BIND_OPCODE_ADD_ADDR_ULEB:
                        segmentOffset += read_uleb128(diag, p, end);
                        break;
                    case BIND_OPCODE_DO_BIND:
                        handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                        segmentOffset += ptrSize;
                        break;
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                        handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                        segmentOffset += read_uleb128(diag, p, end) + ptrSize;
                        break;
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                        handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                        segmentOffset += immediate*ptrSize + ptrSize;
                        break;
                    case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                        count = read_uleb128(diag, p, end);
                        skip = read_uleb128(diag, p, end);
                        for (uint32_t i=0; i < count; ++i) {
                            handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                    ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, addend, stop);
                            segmentOffset += skip + ptrSize;
                            if ( stop )
                                break;
                        }
                        break;
                    default:
                        diag.error("bad bind opcode 0x%02X", *p);
                }
            }
        }
    }
    else {
        // old binary, process external relocations
        const uint64_t                  relocsStartAddress = relocBaseAddress(segmentsInfo, leInfo.layout.linkeditSegIndex);
        const relocation_info* const    relocsStart = (relocation_info*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->extreloff);
        const relocation_info* const    relocsEnd   = &relocsStart[leInfo.dynSymTab->nextrel];
        bool                            is64Bit     = is64() ;
        const uint8_t                   relocSize   = (is64Bit ? 3 : 2);
        const void*                     symbolTable = getLinkEditContent(leInfo.layout, leInfo.symTab->symoff);
        const struct nlist_64*          symbols64   = (nlist_64*)symbolTable;
        const struct nlist*             symbols32   = (struct nlist*)symbolTable;
        const char*                     stringPool  = (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        uint32_t                        symCount    = leInfo.symTab->nsyms;
        uint32_t                        poolSize    = leInfo.symTab->strsize;
        for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
            if ( reloc->r_length != relocSize ) {
                diag.error("external relocation has wrong r_length");
                break;
            }
            if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA == ARM64_RELOC_UNSIGNED
                diag.error("external relocation has wrong r_type");
                break;
            }
            uint32_t segIndex  = 0;
            uint64_t segOffset = 0;
            if ( segIndexAndOffsetForAddress(relocsStartAddress+reloc->r_address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                uint32_t symbolIndex = reloc->r_symbolnum;
                if ( symbolIndex > symCount ) {
                    diag.error("external relocation has out of range r_symbolnum");
                    break;
                }
                else {
                    uint32_t strOffset  = is64Bit ? symbols64[symbolIndex].n_un.n_strx : symbols32[symbolIndex].n_un.n_strx;
                    uint16_t n_desc     = is64Bit ? symbols64[symbolIndex].n_desc : symbols32[symbolIndex].n_desc;
                    uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
                    if ( strOffset >= poolSize ) {
                        diag.error("external relocation has r_symbolnum=%d which has out of range n_strx", symbolIndex);
                        break;
                    }
                    else {
                        const char*     symbolName = stringPool + strOffset;
                        bool            weakImport = (n_desc & N_WEAK_REF);
                        const uint8_t*  content    = (uint8_t*)this + segmentsInfo[segIndex].vmAddr - leInfo.layout.textUnslidVMAddr + segOffset;
                        uint64_t        addend     = is64Bit ? *((uint64_t*)content) : *((uint32_t*)content);
                        handler("external relocation", leInfo, segmentsInfo, true, true, dylibCount, libOrdinal,
                                ptrSize, segIndex, segOffset, BIND_TYPE_POINTER, symbolName, weakImport, addend, stop);
                    }
                }
            }
            else {
                diag.error("local relocation has out of range r_address");
                break;
            }
        }
        // then process indirect symbols
        forEachIndirectPointer(diag, ^(uint64_t address, bool bind, int bindLibOrdinal,
                                       const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
            if ( !bind )
               return;
            uint32_t segIndex  = 0;
            uint64_t segOffset = 0;
            if ( segIndexAndOffsetForAddress(address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                handler("indirect symbol", leInfo, segmentsInfo, true, true, dylibCount, bindLibOrdinal,
                         ptrSize, segIndex, segOffset, BIND_TYPE_POINTER, bindSymbolName, bindWeakImport, 0, indStop);
            }
            else {
                diag.error("indirect symbol has out of range address");
                indStop = true;
            }
        });
    }

}


bool MachOAnalyzer::validChainedFixupsInfo(Diagnostics& diag, const char* path) const
{
    __block uint32_t maxTargetCount = 0;
    __block uint32_t currentTargetCount = 0;
    forEachChainedFixup(diag,
        ^(uint32_t totalTargets, bool& stop) {
            maxTargetCount = totalTargets;
        },
        ^(const LinkEditInfo& leInfo, const SegmentInfo segments[], bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
           if ( symbolName == NULL ) {
                diag.error("in '%s' missing BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM", path);
            }
            else if ( !libraryOrdinalSet ) {
                diag.error("in '%s' missing BIND_OPCODE_SET_DYLIB_ORDINAL", path);
            }
            else if ( libOrdinal > (int)dylibCount ) {
                diag.error("in '%s' has library ordinal too large (%d) max (%d)", path, libOrdinal, dylibCount);
            }
            else if ( libOrdinal < BIND_SPECIAL_DYLIB_WEAK_DEF_COALESCE ) {
                diag.error("in '%s' has unknown library special ordinal (%d)", path, libOrdinal);
            }
            else if ( type != BIND_TYPE_POINTER ) {
                diag.error("in '%s' unknown bind type %d", path, type);
            }
            else if ( currentTargetCount > maxTargetCount ) {
                diag.error("in '%s' chained target counts exceeds BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB", path);
            }
            ++currentTargetCount;
            if ( diag.hasError() )
                stop = true;
        },
        ^(const LinkEditInfo& leInfo, const SegmentInfo segments[], uint8_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, bool& stop) {
           if ( !segIndexSet ) {
                diag.error("in '%s' missing BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", path);
            }
            else if ( segmentIndex >= leInfo.layout.linkeditSegIndex )  {
                diag.error("in '%s' segment index %d too large", path, segmentIndex);
            }
            else if ( segmentOffset > (segments[segmentIndex].vmSize-8) ) {
                diag.error("in '%s' current segment offset 0x%08llX beyond segment size (0x%08llX)", path, segmentOffset, segments[segmentIndex].vmSize);
            }
            else if ( !segments[segmentIndex].writable() ) {
                diag.error("in '%s' pointer bind is in non-writable segment", path);
            }
            else if ( segments[segmentIndex].executable() ) {
                diag.error("in '%s' pointer bind is in executable segment", path);
            }
            if ( diag.hasError() )
                stop = true;
        }
    );

    return diag.noError();
}


void MachOAnalyzer::forEachChainedFixup(Diagnostics& diag, void (^targetCount)(uint32_t totalTargets, bool& stop),
                                                           void (^addTarget)(const LinkEditInfo& leInfo, const SegmentInfo segments[], bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop),
                                                           void (^addChainStart)(const LinkEditInfo& leInfo, const SegmentInfo segments[], uint8_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, bool& stop)) const
{
    bool            stop    = false;

    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(SegmentInfo, segmentsInfo, leInfo.layout.linkeditSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;

    const uint32_t dylibCount = dependentDylibCount();

    if ( leInfo.dyldInfo != nullptr ) {
        // process bind opcodes
        const uint8_t*  p    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->bind_off);
        const uint8_t*  end  = p + leInfo.dyldInfo->bind_size;
        uint8_t         type = 0;
        uint64_t        segmentOffset = 0;
        uint8_t         segmentIndex = 0;
        const char*     symbolName = NULL;
        int             libraryOrdinal = 0;
        bool            segIndexSet = false;
        bool            libraryOrdinalSet = false;
        uint64_t        targetTableCount;
        uint64_t        addend = 0;
        bool            weakImport = false;
        while ( !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
            uint8_t opcode = *p & BIND_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case BIND_OPCODE_DONE:
                    stop = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                    libraryOrdinal = immediate;
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                    libraryOrdinal = (int)read_uleb128(diag, p, end);
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                    // the special ordinals are negative numbers
                    if ( immediate == 0 )
                        libraryOrdinal = 0;
                    else {
                        int8_t signExtended = BIND_OPCODE_MASK | immediate;
                        libraryOrdinal = signExtended;
                    }
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                    weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                    symbolName = (char*)p;
                    while (*p != '\0')
                        ++p;
                    ++p;
                    break;
                case BIND_OPCODE_SET_TYPE_IMM:
                    type = immediate;
                    break;
                case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segmentIndex = immediate;
                    segmentOffset = read_uleb128(diag, p, end);
                    segIndexSet = true;
                    break;
                case BIND_OPCODE_SET_ADDEND_SLEB:
                    addend = read_sleb128(diag, p, end);
                    break;
                case BIND_OPCODE_DO_BIND:
                    if ( addTarget )
                        addTarget(leInfo, segmentsInfo, libraryOrdinalSet, dylibCount, libraryOrdinal, type, symbolName, addend, weakImport, stop);
                    break;
                case BIND_OPCODE_THREADED:
                    switch (immediate) {
                        case BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB:
                            targetTableCount = read_uleb128(diag, p, end);
                            if ( targetTableCount > 65535 ) {
                                diag.error("BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB size too large");
                                stop = true;
                            }
                            else {
                                if ( targetCount )
                                    targetCount((uint32_t)targetTableCount, stop);
                            }
                            break;
                        case BIND_SUBOPCODE_THREADED_APPLY:
                            if ( addChainStart )
                                addChainStart(leInfo, segmentsInfo, segmentIndex, segIndexSet, segmentOffset, stop);
                            break;
                        default:
                            diag.error("bad BIND_OPCODE_THREADED sub-opcode 0x%02X", immediate);
                    }
                    break;
                default:
                    diag.error("bad bind opcode 0x%02X", immediate);
            }
        }
        if ( diag.hasError() )
            return;
    }
}

void MachOAnalyzer::forEachChainedFixupStart(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, bool& stop)) const
{
    __block bool     startVmAddrSet = false;
    __block uint64_t startVmAddr    = 0;
    forEachChainedFixup(diag, nullptr, nullptr, ^(const LinkEditInfo& leInfo, const SegmentInfo segments[], uint8_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, bool& stop) {
       if ( !startVmAddrSet ) {
            for (int i=0; i <= segmentIndex; ++i) {
                if ( strcmp(segments[i].segName, "__TEXT") == 0 ) {
                    startVmAddr = segments[i].vmAddr;
                    startVmAddrSet = true;
                    break;
               }
            }
        }
        uint64_t startVmOffset = segments[segmentIndex].vmAddr + segmentOffset;
        uint64_t runtimeOffset = startVmOffset - startVmAddr;
        callback((uint32_t)runtimeOffset, stop);
    });
}

void MachOAnalyzer::forEachChainedFixupTarget(Diagnostics& diag, void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop)) const
{
    forEachChainedFixup(diag, nullptr, ^(const LinkEditInfo& leInfo, const SegmentInfo segments[], bool libraryOrdinalSet, uint32_t dylibCount,
                                         int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop){
        callback(libOrdinal, symbolName, addend, weakImport, stop);
    }, nullptr);
}

uint32_t MachOAnalyzer::segmentCount() const
{
    __block uint32_t count   = 0;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        ++count;
    });
    return count;
}

bool MachOAnalyzer::hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const
{
    fileOffset = 0;
    size = 0;

    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_CODE_SIGNATURE ) {
            const linkedit_data_command* sigCmd = (linkedit_data_command*)cmd;
            fileOffset = sigCmd->dataoff;
            size       = sigCmd->datasize;
            stop = true;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call

    // early exist if no LC_CODE_SIGNATURE
    if ( fileOffset == 0 )
        return false;

    // <rdar://problem/13622786> ignore code signatures in macOS binaries built with pre-10.9 tools
    if ( (this->cputype == CPU_TYPE_X86_64) || (this->cputype == CPU_TYPE_I386) ) {
        __block bool foundPlatform = false;
        __block bool badSignature  = false;
        forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
            foundPlatform = true;
            if ( (platform == Platform::macOS) && (sdk < 0x000A0900) )
                badSignature = true;
        });
        return foundPlatform && !badSignature;
    }

    return true;
}

bool MachOAnalyzer::hasInitializer(Diagnostics& diag, bool contentRebased, const void* dyldCache) const
{
    __block bool result = false;
    forEachInitializer(diag, contentRebased, ^(uint32_t offset) {
        result = true;
    }, dyldCache);
    return result;
}

void MachOAnalyzer::forEachInitializer(Diagnostics& diag, bool contentRebased, void (^callback)(uint32_t offset), const void* dyldCache) const
{
    __block uint64_t prefTextSegAddrStart = 0;
    __block uint64_t prefTextSegAddrEnd   = 0;

    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            prefTextSegAddrStart = info.vmAddr;
            prefTextSegAddrEnd   = info.vmAddr + info.vmSize;
            stop = true;
        }
    });
    if ( prefTextSegAddrStart == prefTextSegAddrEnd ) {
        diag.error("no __TEXT segment");
        return;
    }
    uint64_t slide = (long)this - prefTextSegAddrStart;

    // if dylib linked with -init linker option, that initializer is first
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ROUTINES ) {
            const routines_command* routines = (routines_command*)cmd;
            uint64_t dashInit = routines->init_address;
            if ( (prefTextSegAddrStart < dashInit) && (dashInit < prefTextSegAddrEnd) )
                callback((uint32_t)(dashInit - prefTextSegAddrStart));
            else
                diag.error("-init does not point within __TEXT segment");
        }
        else if ( cmd->cmd == LC_ROUTINES_64 ) {
            const routines_command_64* routines = (routines_command_64*)cmd;
            uint64_t dashInit = routines->init_address;
            if ( (prefTextSegAddrStart < dashInit) && (dashInit < prefTextSegAddrEnd) )
                callback((uint32_t)(dashInit - prefTextSegAddrStart));
            else
                diag.error("-init does not point within __TEXT segment");
        }
    });

    // next any function pointers in mod-init section
    unsigned ptrSize = pointerSize();
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
        if ( (info.sectFlags & SECTION_TYPE) == S_MOD_INIT_FUNC_POINTERS ) {
            const uint8_t* content;
            content = (uint8_t*)(info.sectAddr + slide);
            if ( (info.sectSize % ptrSize) != 0 ) {
                diag.error("initializer section %s/%s has bad size", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            if ( malformedSectionRange ) {
                diag.error("initializer section %s/%s extends beyond its segment", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            if ( ((long)content % ptrSize) != 0 ) {
                diag.error("initializer section %s/%s is not pointer aligned", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            if ( ptrSize == 8 ) {
                const uint64_t* initsStart = (uint64_t*)content;
                const uint64_t* initsEnd   = (uint64_t*)((uint8_t*)content + info.sectSize);
                for (const uint64_t* p=initsStart; p < initsEnd; ++p) {
                    uint64_t anInit = *p;
                    if ( contentRebased )
                        anInit -= slide;
                    if ( hasChainedFixups() ) {
                        ChainedFixupPointerOnDisk* aChainedInit = (ChainedFixupPointerOnDisk*)p;
                        if ( aChainedInit->authBind.bind )
                            diag.error("initializer uses bind");
                        if ( aChainedInit->authRebase.auth ) {
                            anInit = aChainedInit->authRebase.target;
                        }
                        else {
                            anInit = aChainedInit->plainRebase.signExtendedTarget();
                        }
                    }
                    if ( (anInit <= prefTextSegAddrStart) || (anInit > prefTextSegAddrEnd) ) {
                         diag.error("initializer 0x%0llX does not point within __TEXT segment", anInit);
                         stop = true;
                         break;
                    }
                    callback((uint32_t)(anInit - prefTextSegAddrStart));
                }
            }
            else {
                const uint32_t* initsStart = (uint32_t*)content;
                const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)content + info.sectSize);
                for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
                    uint32_t anInit = *p;
                    if ( contentRebased )
                        anInit -= slide;
                    if ( (anInit <= prefTextSegAddrStart) || (anInit > prefTextSegAddrEnd) ) {
                         diag.error("initializer 0x%0X does not point within __TEXT segment", anInit);
                         stop = true;
                         break;
                    }
                    callback(anInit - (uint32_t)prefTextSegAddrStart);
                }
            }
        }
    });
}


void MachOAnalyzer::forEachRPath(void (^callback)(const char* rPath, bool& stop)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         if ( cmd->cmd == LC_RPATH ) {
            const char* rpath = (char*)cmd + ((struct rpath_command*)cmd)->path.offset;
            callback(rpath, stop);
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}


bool MachOAnalyzer::hasObjC() const
{
    __block bool result = false;
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
        if ( (strcmp(info.sectName, "__objc_imageinfo") == 0) && (strncmp(info.segInfo.segName, "__DATA", 6) == 0) ) {
            result = true;
            stop = true;
        }
        if ( (this->cputype == CPU_TYPE_I386) && (strcmp(info.sectName, "__image_info") == 0) && (strcmp(info.segInfo.segName, "__OBJC") == 0) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

bool MachOAnalyzer::hasPlusLoadMethod(Diagnostics& diag) const
{
    __block bool result = false;
    if ( (this->cputype == CPU_TYPE_I386) && supportsPlatform(Platform::macOS) ) {
        // old objc runtime has no special section for +load methods, scan for string
        int64_t slide = getSlide();
        forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
            if ( ( (info.sectFlags & SECTION_TYPE) == S_CSTRING_LITERALS ) ) {
                if ( malformedSectionRange ) {
                    diag.error("cstring section %s/%s extends beyond the end of the segment", info.segInfo.segName, info.sectName);
                    stop = true;
                    return;
                }
                const uint8_t* content = (uint8_t*)(info.sectAddr + slide);
                const char* s   = (char*)content;
                const char* end = s + info.sectSize;
                while ( s < end ) {
                    if ( strcmp(s, "load") == 0 ) {
                        result = true;
                        stop = true;
                        return;
                    }
                    while (*s != '\0' )
                        ++s;
                    ++s;
                }
            }
        });
    }
    else {
        // in new objc runtime compiler puts classes/categories with +load method in specical section
        forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
            if ( strncmp(info.segInfo.segName, "__DATA", 6) != 0 )
                return;
            if ( (strcmp(info.sectName, "__objc_nlclslist") == 0) || (strcmp(info.sectName, "__objc_nlcatlist") == 0)) {
                result = true;
                stop = true;
            }
        });
    }
    return result;
}

const void* MachOAnalyzer::getRebaseOpcodes(uint32_t& size) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.dyldInfo == nullptr) )
        return nullptr;

    size = leInfo.dyldInfo->rebase_size;
    return getLinkEditContent(leInfo.layout, leInfo.dyldInfo->rebase_off);
}

const void* MachOAnalyzer::getBindOpcodes(uint32_t& size) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.dyldInfo == nullptr) )
        return nullptr;

    size = leInfo.dyldInfo->bind_size;
    return getLinkEditContent(leInfo.layout, leInfo.dyldInfo->bind_off);
}

const void* MachOAnalyzer::getLazyBindOpcodes(uint32_t& size) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.dyldInfo == nullptr) )
        return nullptr;

    size = leInfo.dyldInfo->lazy_bind_size;
    return getLinkEditContent(leInfo.layout, leInfo.dyldInfo->lazy_bind_off);
}


uint64_t MachOAnalyzer::segAndOffsetToRuntimeOffset(uint8_t targetSegIndex, uint64_t targetSegOffset) const
{
    __block uint64_t textVmAddr = 0;
    __block uint64_t result     = 0;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 )
            textVmAddr = info.vmAddr;
        if ( info.segIndex == targetSegIndex ) {
            result = (info.vmAddr - textVmAddr) + targetSegOffset;
        }
    });
    return result;
}

bool MachOAnalyzer::hasLazyPointers(uint32_t& runtimeOffset, uint32_t& size) const
{
    size = 0;
    forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( (info.sectFlags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS ) {
            runtimeOffset = (uint32_t)(info.sectAddr - preferredLoadAddress());
            size          = (uint32_t)info.sectSize;
            stop = true;
        }
    });
    return (size != 0);
}

uint64_t MachOAnalyzer::preferredLoadAddress() const
{
    __block uint64_t textVmAddr = 0;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            textVmAddr = info.vmAddr;
            stop = true;
        }
    });
    return textVmAddr;
}


bool MachOAnalyzer::getEntry(uint32_t& offset, bool& usesCRT) const
{
    Diagnostics diag;
    offset = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_MAIN ) {
            entry_point_command* mainCmd = (entry_point_command*)cmd;
            usesCRT = false;
            offset = (uint32_t)mainCmd->entryoff;
            stop = true;
        }
        else if ( cmd->cmd == LC_UNIXTHREAD ) {
            stop = true;
            usesCRT = true;
            uint64_t startAddress = entryAddrFromThreadCmd((thread_command*)cmd);
            offset = (uint32_t)(startAddress - preferredLoadAddress());
        }
    });
    return (offset != 0);
}

uint64_t MachOAnalyzer::entryAddrFromThreadCmd(const thread_command* cmd) const
{
    assert(cmd->cmd == LC_UNIXTHREAD);
    const uint32_t* regs32 = (uint32_t*)(((char*)cmd) + 16);
    const uint64_t* regs64 = (uint64_t*)(((char*)cmd) + 16);
    uint64_t startAddress = 0;
    switch ( this->cputype ) {
        case CPU_TYPE_I386:
            startAddress = regs32[10]; // i386_thread_state_t.eip
            break;
        case CPU_TYPE_X86_64:
            startAddress = regs64[16]; // x86_thread_state64_t.rip
            break;
    }
    return startAddress;
}


void MachOAnalyzer::forEachInterposingSection(Diagnostics& diag, void (^handler)(uint64_t vmOffset, uint64_t vmSize, bool& stop)) const
{
    const unsigned ptrSize   = pointerSize();
    const unsigned entrySize = 2 * ptrSize;
    forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( ((info.sectFlags & SECTION_TYPE) == S_INTERPOSING) || ((strcmp(info.sectName, "__interpose") == 0) && (strcmp(info.segInfo.segName, "__DATA") == 0)) ) {
            if ( info.sectSize % entrySize != 0 ) {
                diag.error("interposing section %s/%s has bad size", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            if ( malformedSectionRange ) {
                diag.error("interposing section %s/%s extends beyond the end of the segment", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            if ( (info.sectAddr % ptrSize) != 0 ) {
                diag.error("interposing section %s/%s is not pointer aligned", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            handler(info.sectAddr - preferredLoadAddress(), info.sectSize, stop);
        }
    });
}

void MachOAnalyzer::forEachDOFSection(Diagnostics& diag, void (^callback)(uint32_t offset)) const
{
    forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( ( (info.sectFlags & SECTION_TYPE) == S_DTRACE_DOF ) && !malformedSectionRange ) {
            callback((uint32_t)(info.sectAddr - info.segInfo.vmAddr));
        }
    });
}

bool MachOAnalyzer::getCDHash(uint8_t cdHash[20]) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.codeSig == nullptr) )
        return false;

    return cdHashOfCodeSignature(getLinkEditContent(leInfo.layout, leInfo.codeSig->dataoff), leInfo.codeSig->datasize, cdHash);
}

bool MachOAnalyzer::isRestricted() const
{
    __block bool result = false;
    forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( (strcmp(info.segInfo.segName, "__RESTRICT") == 0) && (strcmp(info.sectName, "__restrict") == 0) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

bool MachOAnalyzer::usesLibraryValidation() const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.codeSig == nullptr) )
        return false;

    const CS_CodeDirectory* cd = (const CS_CodeDirectory*)findCodeDirectoryBlob(getLinkEditContent(leInfo.layout, leInfo.codeSig->dataoff), leInfo.codeSig->datasize);
    if ( cd == nullptr )
        return false;

    // check for CS_REQUIRE_LV in CS_CodeDirectory.flags
    return (htonl(cd->flags) & CS_REQUIRE_LV);
}

bool MachOAnalyzer::canHavePrecomputedDlopenClosure(const char* path, void (^failureReason)(const char*)) const
{
    __block bool retval = true;

    // only dylibs can go in cache
    if ( (this->filetype != MH_DYLIB) && (this->filetype != MH_BUNDLE) ) {
        retval = false;
        failureReason("not MH_DYLIB or MH_BUNDLE");
    }

    // flat namespace files cannot go in cache
    if ( (this->flags & MH_TWOLEVEL) == 0 ) {
        retval = false;
        failureReason("not built with two level namespaces");
    }

    // can only depend on other dylibs with absolute paths
    __block bool allDepPathsAreGood = true;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        if ( loadPath[0] != '/' ) {
            allDepPathsAreGood = false;
            stop = true;
        }
    });
    if ( !allDepPathsAreGood ) {
        retval = false;
        failureReason("depends on dylibs that are not absolute paths");
    }

    // dylibs with interposing info cannot have dlopen closure pre-computed
    __block bool hasInterposing = false;
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( ((info.sectFlags & SECTION_TYPE) == S_INTERPOSING) || ((strcmp(info.sectName, "__interpose") == 0) && (strcmp(info.segInfo.segName, "__DATA") == 0)) )
            hasInterposing = true;
    });
    if ( hasInterposing ) {
        retval = false;
        failureReason("has interposing tuples");
    }

    // images that use dynamic_lookup, bundle_loader, or have weak-defs cannot have dlopen closure pre-computed
    Diagnostics diag;
    auto checkBind = ^(int libOrdinal, bool& stop) {
        switch (libOrdinal) {
            case BIND_SPECIAL_DYLIB_WEAK_DEF_COALESCE:
                failureReason("has weak externals");
                retval = false;
                stop = true;
                break;
            case BIND_SPECIAL_DYLIB_FLAT_LOOKUP:
                failureReason("has dynamic_lookup binds");
                retval = false;
                stop = true;
                break;
            case BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE:
                failureReason("has reference to main executable (bundle loader)");
                retval = false;
                stop = true;
                break;
        }
    };

    if (hasChainedFixups()) {
        forEachChainedFixupTarget(diag, ^(int libOrdinal, const char *symbolName, uint64_t addend, bool weakImport, bool &stop) {
            checkBind(libOrdinal, stop);
        });
    } else {
        forEachBind(diag, ^(uint64_t runtimeOffset, int libOrdinal, const char* symbolName, bool weakImport, uint64_t addend, bool& stop) {
            checkBind(libOrdinal, stop);
        },
        ^(const char* symbolName) {
        });
    }

    // special system dylib overrides cannot have closure pre-computed
    if ( strncmp(path, "/usr/lib/system/introspection/", 30) == 0 ) {
        retval = false;
        failureReason("override of OS dylib");
    }

    return retval;
}

bool MachOAnalyzer::canBePlacedInDyldCache(const char* path, void (^failureReason)(const char*)) const
{
    if (!MachOFile::canBePlacedInDyldCache(path, failureReason))
        return false;
    if ( !(isArch("x86_64") || isArch("x86_64h")) )
        return true;

    // Kick dylibs out of the x86_64 cache if they are using TBI.
    __block bool rebasesOk = true;
    Diagnostics diag;
    uint64_t startVMAddr = preferredLoadAddress();
    uint64_t endVMAddr = startVMAddr + mappedSize();
    forEachRebase(diag, false, ^(uint64_t runtimeOffset, bool &stop) {
        uint64_t value = *(uint64_t*)((uint8_t*)this + runtimeOffset);
        if ( (value < startVMAddr) || (value >= endVMAddr) ) {
            failureReason("rebase value out of range of dylib");
            rebasesOk = false;
            stop = true;
        }
    });
    return rebasesOk;
}

} // dyld3


