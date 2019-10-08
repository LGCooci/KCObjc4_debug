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
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <mach-o/reloc.h>
#include <mach-o/nlist.h>
#include <CommonCrypto/CommonDigest.h>

#include <stdio.h>

#include "MachOLoaded.h"
#include "MachOFile.h"
#include "MachOFile.h"
#include "CodeSigningTypes.h"


#ifndef LC_BUILD_VERSION
    #define LC_BUILD_VERSION 0x32 /* build for platform min OS version */

    /*
     * The build_version_command contains the min OS version on which this
     * binary was built to run for its platform.  The list of known platforms and
     * tool values following it.
     */
    struct build_version_command {
        uint32_t    cmd;        /* LC_BUILD_VERSION */
        uint32_t    cmdsize;    /* sizeof(struct build_version_command) plus */
        /* ntools * sizeof(struct build_tool_version) */
        uint32_t    platform;   /* platform */
        uint32_t    minos;      /* X.Y.Z is encoded in nibbles xxxx.yy.zz */
        uint32_t    sdk;        /* X.Y.Z is encoded in nibbles xxxx.yy.zz */
        uint32_t    ntools;     /* number of tool entries following this */
    };

    struct build_tool_version {
        uint32_t    tool;       /* enum for the tool */
        uint32_t    version;    /* version number of the tool */
    };

    /* Known values for the platform field above. */
    #define PLATFORM_MACOS      1
    #define PLATFORM_IOS        2
    #define PLATFORM_TVOS       3
    #define PLATFORM_WATCHOS    4
    #define PLATFORM_BRIDGEOS   5

    /* Known values for the tool field above. */
    #define TOOL_CLANG    1
    #define TOOL_SWIFT    2
    #define TOOL_LD       3
#endif



namespace dyld3 {


void MachOLoaded::getLinkEditLoadCommands(Diagnostics& diag, LinkEditInfo& result) const
{
    result.dyldInfo       = nullptr;
    result.symTab         = nullptr;
    result.dynSymTab      = nullptr;
    result.splitSegInfo   = nullptr;
    result.functionStarts = nullptr;
    result.dataInCode     = nullptr;
    result.codeSig        = nullptr;
    __block bool hasUUID    = false;
    __block bool hasMinVersion = false;
    __block bool hasEncrypt = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                if ( cmd->cmdsize != sizeof(dyld_info_command) )
                    diag.error("LC_DYLD_INFO load command size wrong");
                else if ( result.dyldInfo != nullptr )
                    diag.error("multiple LC_DYLD_INFO load commands");
                result.dyldInfo = (dyld_info_command*)cmd;
                break;
            case LC_SYMTAB:
                if ( cmd->cmdsize != sizeof(symtab_command) )
                    diag.error("LC_SYMTAB load command size wrong");
                else if ( result.symTab != nullptr )
                    diag.error("multiple LC_SYMTAB load commands");
                result.symTab = (symtab_command*)cmd;
                break;
            case LC_DYSYMTAB:
                if ( cmd->cmdsize != sizeof(dysymtab_command) )
                    diag.error("LC_DYSYMTAB load command size wrong");
                else if ( result.dynSymTab != nullptr )
                    diag.error("multiple LC_DYSYMTAB load commands");
                result.dynSymTab = (dysymtab_command*)cmd;
                break;
            case LC_SEGMENT_SPLIT_INFO:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_SEGMENT_SPLIT_INFO load command size wrong");
                else if ( result.splitSegInfo != nullptr )
                    diag.error("multiple LC_SEGMENT_SPLIT_INFO load commands");
                result.splitSegInfo = (linkedit_data_command*)cmd;
                break;
            case LC_FUNCTION_STARTS:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_FUNCTION_STARTS load command size wrong");
                else if ( result.functionStarts != nullptr )
                    diag.error("multiple LC_FUNCTION_STARTS load commands");
                result.functionStarts = (linkedit_data_command*)cmd;
                break;
            case LC_DATA_IN_CODE:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_DATA_IN_CODE load command size wrong");
                else if ( result.dataInCode != nullptr )
                    diag.error("multiple LC_DATA_IN_CODE load commands");
                result.dataInCode = (linkedit_data_command*)cmd;
                break;
            case LC_CODE_SIGNATURE:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_CODE_SIGNATURE load command size wrong");
                else if ( result.codeSig != nullptr )
                     diag.error("multiple LC_CODE_SIGNATURE load commands");
                result.codeSig = (linkedit_data_command*)cmd;
                break;
            case LC_UUID:
                if ( cmd->cmdsize != sizeof(uuid_command) )
                    diag.error("LC_UUID load command size wrong");
                else if ( hasUUID )
                     diag.error("multiple LC_UUID load commands");
                hasUUID = true;
                break;
            case LC_VERSION_MIN_IPHONEOS:
            case LC_VERSION_MIN_MACOSX:
            case LC_VERSION_MIN_TVOS:
            case LC_VERSION_MIN_WATCHOS:
                if ( cmd->cmdsize != sizeof(version_min_command) )
                    diag.error("LC_VERSION_* load command size wrong");
                 else if ( hasMinVersion )
                     diag.error("multiple LC_VERSION_MIN_* load commands");
                hasMinVersion = true;
                break;
            case LC_BUILD_VERSION:
                if ( cmd->cmdsize != (sizeof(build_version_command) + ((build_version_command*)cmd)->ntools * sizeof(build_tool_version)) )
                    diag.error("LC_BUILD_VERSION load command size wrong");
                else if ( hasMinVersion )
                    diag.error("LC_BUILD_VERSION cannot coexist LC_VERSION_MIN_* with load commands");
                break;
            case LC_ENCRYPTION_INFO:
                if ( cmd->cmdsize != sizeof(encryption_info_command) )
                    diag.error("LC_ENCRYPTION_INFO load command size wrong");
                else if ( hasEncrypt )
                    diag.error("multiple LC_ENCRYPTION_INFO load commands");
                else if ( is64() )
                    diag.error("LC_ENCRYPTION_INFO found in 64-bit mach-o");
                hasEncrypt = true;
                break;
            case LC_ENCRYPTION_INFO_64:
                if ( cmd->cmdsize != sizeof(encryption_info_command_64) )
                    diag.error("LC_ENCRYPTION_INFO_64 load command size wrong");
                else if ( hasEncrypt )
                     diag.error("multiple LC_ENCRYPTION_INFO_64 load commands");
                else if ( !is64() )
                      diag.error("LC_ENCRYPTION_INFO_64 found in 32-bit mach-o");
                hasEncrypt = true;
                break;
        }
    });
    if ( diag.noError() && (result.dynSymTab != nullptr) && (result.symTab == nullptr) )
        diag.error("LC_DYSYMTAB but no LC_SYMTAB load command");
}

void MachOLoaded::getLinkEditPointers(Diagnostics& diag, LinkEditInfo& result) const
{
    getLinkEditLoadCommands(diag, result);
    if ( diag.noError() )
        getLayoutInfo(result.layout);
}

void MachOLoaded::getLayoutInfo(LayoutInfo& result) const
{
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            result.textUnslidVMAddr = (uintptr_t)info.vmAddr;
            result.slide = (uintptr_t)(((uint64_t)this) - info.vmAddr);
        }
        else if ( strcmp(info.segName, "__LINKEDIT") == 0 ) {
            result.linkeditUnslidVMAddr = (uintptr_t)info.vmAddr;
            result.linkeditFileOffset   = (uint32_t)info.fileOffset;
            result.linkeditFileSize     = (uint32_t)info.fileSize;
            result.linkeditSegIndex     = info.segIndex;
        }
    });
}

bool MachOLoaded::hasExportTrie(uint32_t& runtimeOffset, uint32_t& size) const
{
    runtimeOffset = 0;
    size = 0;
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    if ( diag.hasError() )
        return false;
    if ( leInfo.dyldInfo != nullptr ) {
        uint32_t offsetInLinkEdit = leInfo.dyldInfo->export_off - leInfo.layout.linkeditFileOffset;
        runtimeOffset = offsetInLinkEdit + (uint32_t)(leInfo.layout.linkeditUnslidVMAddr - leInfo.layout.textUnslidVMAddr);
        size = leInfo.dyldInfo->export_size;
        return true;
    }
    return false;
}


#if BUILDING_LIBDYLD
// this is only used by dlsym() at runtime.  All other binding is done when the closure is built.
bool MachOLoaded::hasExportedSymbol(const char* symbolName, DependentToMachOLoaded finder, void** result,
                                    bool* resultPointsToInstructions) const
{
    typedef void* (*ResolverFunc)(void);
    ResolverFunc resolver;
    Diagnostics diag;
    FoundSymbol foundInfo;
    if ( findExportedSymbol(diag, symbolName, foundInfo, finder) ) {
        switch ( foundInfo.kind ) {
            case FoundSymbol::Kind::headerOffset: {
                *result = (uint8_t*)foundInfo.foundInDylib + foundInfo.value;
                *resultPointsToInstructions = false;
                int64_t slide = foundInfo.foundInDylib->getSlide();
                foundInfo.foundInDylib->forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
                    uint64_t sectStartAddr = sectInfo.sectAddr + slide;
                    uint64_t sectEndAddr = sectStartAddr + sectInfo.sectSize;
                    if ( ((uint64_t)*result >= sectStartAddr) && ((uint64_t)*result < sectEndAddr) ) {
                        *resultPointsToInstructions = (sectInfo.sectFlags & S_ATTR_PURE_INSTRUCTIONS) || (sectInfo.sectFlags & S_ATTR_SOME_INSTRUCTIONS);
                        stop = true;
                    }
                });
                break;
            }
            case FoundSymbol::Kind::absolute:
                *result = (void*)(long)foundInfo.value;
                *resultPointsToInstructions = false;
                break;
            case FoundSymbol::Kind::resolverOffset:
                // foundInfo.value contains "stub".
                // in dlsym() we want to call resolver function to get final function address
                resolver = (ResolverFunc)((uint8_t*)foundInfo.foundInDylib + foundInfo.resolverFuncOffset);
                *result = (*resolver)();
                // FIXME: Set this properly
                *resultPointsToInstructions = true;
                break;
        }
        return true;
    }
    return false;
}
#endif // BUILDING_LIBDYLD

bool MachOLoaded::findExportedSymbol(Diagnostics& diag, const char* symbolName, FoundSymbol& foundInfo, DependentToMachOLoaded findDependent) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    if ( leInfo.dyldInfo != nullptr ) {
        const uint8_t* trieStart = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->export_off);
        const uint8_t* trieEnd   = trieStart + leInfo.dyldInfo->export_size;
        const uint8_t* node      = trieWalk(diag, trieStart, trieEnd, symbolName);
        if ( node == nullptr ) {
            // symbol not exported from this image. Seach any re-exported dylibs
            __block unsigned        depIndex = 0;
            __block bool            foundInReExportedDylib = false;
            forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
                if ( isReExport && findDependent ) {
                    if ( const MachOLoaded* depMH = findDependent(this, depIndex) ) {
                       if ( depMH->findExportedSymbol(diag, symbolName, foundInfo, findDependent) ) {
                            stop = true;
                            foundInReExportedDylib = true;
                        }
                    }
                }
                ++depIndex;
            });
            return foundInReExportedDylib;
        }
        const uint8_t* p = node;
        const uint64_t flags = read_uleb128(diag, p, trieEnd);
        if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
            if ( !findDependent )
                return false;
            // re-export from another dylib, lookup there
            const uint64_t ordinal = read_uleb128(diag, p, trieEnd);
            const char* importedName = (char*)p;
            if ( importedName[0] == '\0' )
                importedName = symbolName;
            if ( (ordinal == 0) || (ordinal > dependentDylibCount()) ) {
                diag.error("re-export ordinal %lld out of range for %s", ordinal, symbolName);
                return false;
            }
            uint32_t depIndex = (uint32_t)(ordinal-1);
            if ( const MachOLoaded* depMH = findDependent(this, depIndex) ) {
                return depMH->findExportedSymbol(diag, importedName, foundInfo, findDependent);
            }
            else {
                diag.error("dependent dylib %lld not found for re-exported symbol %s", ordinal, symbolName);
                return false;
            }
        }
        foundInfo.kind               = FoundSymbol::Kind::headerOffset;
        foundInfo.isThreadLocal      = false;
        foundInfo.isWeakDef          = false;
        foundInfo.foundInDylib       = this;
        foundInfo.value              = read_uleb128(diag, p, trieEnd);
        foundInfo.resolverFuncOffset = 0;
        foundInfo.foundSymbolName    = symbolName;
        if ( diag.hasError() )
            return false;
        switch ( flags & EXPORT_SYMBOL_FLAGS_KIND_MASK ) {
            case EXPORT_SYMBOL_FLAGS_KIND_REGULAR:
                if ( flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER ) {
                    foundInfo.kind = FoundSymbol::Kind::headerOffset;
                    foundInfo.resolverFuncOffset = (uint32_t)read_uleb128(diag, p, trieEnd);
                }
                else {
                    foundInfo.kind = FoundSymbol::Kind::headerOffset;
                }
                if ( flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION )
                    foundInfo.isWeakDef = true;
                break;
            case EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL:
                foundInfo.isThreadLocal = true;
                break;
            case EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE:
                foundInfo.kind = FoundSymbol::Kind::absolute;
                break;
            default:
                diag.error("unsupported exported symbol kind. flags=%llu at node offset=0x%0lX", flags, (long)(node-trieStart));
                return false;
        }
        return true;
    }
    else {
        // this is an old binary (before macOS 10.6), scan the symbol table
        foundInfo.foundInDylib = nullptr;
        forEachGlobalSymbol(diag, ^(const char* aSymbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop) {
            if ( strcmp(aSymbolName, symbolName) == 0 ) {
                foundInfo.kind               = FoundSymbol::Kind::headerOffset;
                foundInfo.isThreadLocal      = false;
                foundInfo.foundInDylib       = this;
                foundInfo.value              = n_value - leInfo.layout.textUnslidVMAddr;
                foundInfo.resolverFuncOffset = 0;
                foundInfo.foundSymbolName    = symbolName;
                stop = true;
            }
        });
        if ( foundInfo.foundInDylib == nullptr ) {
            // symbol not exported from this image. Search any re-exported dylibs
            __block unsigned depIndex = 0;
            forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
                if ( isReExport && findDependent ) {
                    if ( const MachOLoaded* depMH = findDependent(this, depIndex) ) {
                        if ( depMH->findExportedSymbol(diag, symbolName, foundInfo, findDependent) ) {
                            stop = true;
                        }
                    }
                }
                ++depIndex;
            });
        }
        return (foundInfo.foundInDylib != nullptr);
    }
}

intptr_t MachOLoaded::getSlide() const
{
    Diagnostics diag;
    __block intptr_t slide = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            if ( strcmp(seg->segname, "__TEXT") == 0 ) {
                slide = (uintptr_t)(((uint64_t)this) - seg->vmaddr);
                stop = true;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            if ( strcmp(seg->segname, "__TEXT") == 0 ) {
                slide = (uintptr_t)(((uint64_t)this) - seg->vmaddr);
                stop = true;
            }
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    return slide;
}

const uint8_t* MachOLoaded::getLinkEditContent(const LayoutInfo& info, uint32_t fileOffset) const
{
    uint32_t offsetInLinkedit   = fileOffset - info.linkeditFileOffset;
    uintptr_t linkeditStartAddr = info.linkeditUnslidVMAddr + info.slide;
    return (uint8_t*)(linkeditStartAddr + offsetInLinkedit);
}


void MachOLoaded::forEachGlobalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    const bool is64Bit = is64();
    if ( leInfo.symTab != nullptr ) {
        uint32_t globalsStartIndex = 0;
        uint32_t globalsCount      = leInfo.symTab->nsyms;
        if ( leInfo.dynSymTab != nullptr ) {
            globalsStartIndex = leInfo.dynSymTab->iextdefsym;
            globalsCount      = leInfo.dynSymTab->nextdefsym;
        }
        uint32_t               maxStringOffset  = leInfo.symTab->strsize;
        const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        const struct nlist_64* symbols64        = (struct nlist_64*)symbols;
        bool                   stop             = false;
        for (uint32_t i=0; (i < globalsCount) && !stop; ++i) {
            if ( is64Bit ) {
                const struct nlist_64& sym = symbols64[globalsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_EXT) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
            else {
                const struct nlist& sym = symbols[globalsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_EXT) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
        }
    }
}

void MachOLoaded::forEachLocalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    const bool is64Bit = is64();
    if ( leInfo.symTab != nullptr ) {
        uint32_t localsStartIndex = 0;
        uint32_t localsCount      = leInfo.symTab->nsyms;
        if ( leInfo.dynSymTab != nullptr ) {
            localsStartIndex = leInfo.dynSymTab->ilocalsym;
            localsCount      = leInfo.dynSymTab->nlocalsym;
        }
        uint32_t               maxStringOffset  = leInfo.symTab->strsize;
        const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        const struct nlist_64* symbols64        = (struct nlist_64*)(getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        bool                   stop             = false;
        for (uint32_t i=0; (i < localsCount) && !stop; ++i) {
            if ( is64Bit ) {
                const struct nlist_64& sym = symbols64[localsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( ((sym.n_type & N_EXT) == 0) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
            else {
                const struct nlist& sym = symbols[localsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( ((sym.n_type & N_EXT) == 0) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
        }
    }
}

uint32_t MachOLoaded::dependentDylibCount() const
{
    __block uint32_t count = 0;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        ++count;
    });
    return count;
}

const char* MachOLoaded::dependentDylibLoadPath(uint32_t depIndex) const
{
    __block const char* foundLoadPath = nullptr;
    __block uint32_t curDepIndex = 0;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        if ( curDepIndex == depIndex ) {
            foundLoadPath = loadPath;
            stop = true;
        }
        ++curDepIndex;
    });
    return foundLoadPath;
}

const char* MachOLoaded::segmentName(uint32_t targetSegIndex) const
{
    __block const char* result = nullptr;
	forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( targetSegIndex == info.segIndex ) {
            result = info.segName;
            stop = true;
        }
    });
    return result;
}

bool MachOLoaded::findClosestFunctionStart(uint64_t address, uint64_t* functionStartAddress) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    if ( leInfo.functionStarts == nullptr )
        return false;

    const uint8_t* starts    = getLinkEditContent(leInfo.layout, leInfo.functionStarts->dataoff);
    const uint8_t* startsEnd = starts + leInfo.functionStarts->datasize;

    uint64_t lastAddr    = (uint64_t)(long)this;
    uint64_t runningAddr = lastAddr;
    while (diag.noError()) {
        uint64_t value = read_uleb128(diag, starts, startsEnd);
        if ( value == 0 )
            break;
        lastAddr = runningAddr;
        runningAddr += value;
        //fprintf(stderr, "  addr=0x%08llX\n", runningAddr);
        if ( runningAddr > address ) {
            *functionStartAddress = lastAddr;
            return true;
        }
    };

    return false;
}

bool MachOLoaded::findClosestSymbol(uint64_t address, const char** symbolName, uint64_t* symbolAddr) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    if ( (leInfo.symTab == nullptr) || (leInfo.dynSymTab == nullptr) )
        return false;
    uint64_t targetUnslidAddress = address - leInfo.layout.slide;

    uint32_t               maxStringOffset  = leInfo.symTab->strsize;
    const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
    const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
    if ( is64() ) {
        const struct nlist_64* symbols64  = (struct nlist_64*)symbols;
        const struct nlist_64* bestSymbol = nullptr;
        // first walk all global symbols
        const struct nlist_64* const globalsStart = &symbols64[leInfo.dynSymTab->iextdefsym];
        const struct nlist_64* const globalsEnd   = &globalsStart[leInfo.dynSymTab->nextdefsym];
        for (const struct nlist_64* s = globalsStart; s < globalsEnd; ++s) {
            if ( (s->n_type & N_TYPE) == N_SECT ) {
                if ( bestSymbol == nullptr ) {
                    if ( s->n_value <= targetUnslidAddress )
                        bestSymbol = s;
                }
                else if ( (s->n_value <= targetUnslidAddress) && (bestSymbol->n_value < s->n_value) ) {
                    bestSymbol = s;
                }
            }
        }
        // next walk all local symbols
        const struct nlist_64* const localsStart = &symbols64[leInfo.dynSymTab->ilocalsym];
        const struct nlist_64* const localsEnd   = &localsStart[leInfo.dynSymTab->nlocalsym];
        for (const struct nlist_64* s = localsStart; s < localsEnd; ++s) {
             if ( ((s->n_type & N_TYPE) == N_SECT) && ((s->n_type & N_STAB) == 0) ) {
                if ( bestSymbol == nullptr ) {
                    if ( s->n_value <= targetUnslidAddress )
                        bestSymbol = s;
                }
                else if ( (s->n_value <= targetUnslidAddress) && (bestSymbol->n_value < s->n_value) ) {
                    bestSymbol = s;
                }
            }
        }
        if ( bestSymbol != NULL ) {
            *symbolAddr = bestSymbol->n_value + leInfo.layout.slide;
            if ( bestSymbol->n_un.n_strx < maxStringOffset )
                *symbolName = &stringPool[bestSymbol->n_un.n_strx];
            return true;
        }
    }
    else {
       const struct nlist* bestSymbol = nullptr;
        // first walk all global symbols
        const struct nlist* const globalsStart = &symbols[leInfo.dynSymTab->iextdefsym];
        const struct nlist* const globalsEnd   = &globalsStart[leInfo.dynSymTab->nextdefsym];
        for (const struct nlist* s = globalsStart; s < globalsEnd; ++s) {
            if ( (s->n_type & N_TYPE) == N_SECT ) {
                if ( bestSymbol == nullptr ) {
                    if ( s->n_value <= targetUnslidAddress )
                        bestSymbol = s;
                }
                else if ( (s->n_value <= targetUnslidAddress) && (bestSymbol->n_value < s->n_value) ) {
                    bestSymbol = s;
                }
            }
        }
        // next walk all local symbols
        const struct nlist* const localsStart = &symbols[leInfo.dynSymTab->ilocalsym];
        const struct nlist* const localsEnd   = &localsStart[leInfo.dynSymTab->nlocalsym];
        for (const struct nlist* s = localsStart; s < localsEnd; ++s) {
             if ( ((s->n_type & N_TYPE) == N_SECT) && ((s->n_type & N_STAB) == 0) ) {
                if ( bestSymbol == nullptr ) {
                    if ( s->n_value <= targetUnslidAddress )
                        bestSymbol = s;
                }
                else if ( (s->n_value <= targetUnslidAddress) && (bestSymbol->n_value < s->n_value) ) {
                    bestSymbol = s;
                }
            }
        }
        if ( bestSymbol != nullptr ) {
#if __arm__
            if ( bestSymbol->n_desc & N_ARM_THUMB_DEF )
                *symbolAddr = (bestSymbol->n_value | 1) + leInfo.layout.slide;
            else
                *symbolAddr = bestSymbol->n_value + leInfo.layout.slide;
#else
            *symbolAddr = bestSymbol->n_value + leInfo.layout.slide;
#endif
            if ( bestSymbol->n_un.n_strx < maxStringOffset )
                *symbolName = &stringPool[bestSymbol->n_un.n_strx];
            return true;
        }
    }

    return false;
}

const void* MachOLoaded::findSectionContent(const char* segName, const char* sectName, uint64_t& size) const
{
    __block const void* result = nullptr;
    forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( (strcmp(sectInfo.sectName, sectName) == 0) && (strcmp(sectInfo.segInfo.segName, segName) == 0) ) {
            size = sectInfo.sectSize;
            result = (void*)(sectInfo.sectAddr + getSlide());
        }
    });
    return result;
}


bool MachOLoaded::intersectsRange(uintptr_t start, uintptr_t length) const
{
    __block bool result = false;
    uintptr_t slide = getSlide();
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( (info.vmAddr+info.vmSize+slide >= start) && (info.vmAddr+slide < start+length) )
            result = true;
    });
    return result;
}

const uint8_t* MachOLoaded::trieWalk(Diagnostics& diag, const uint8_t* start, const uint8_t* end, const char* symbol)
{
    uint32_t visitedNodeOffsets[128];
    int visitedNodeOffsetCount = 0;
    visitedNodeOffsets[visitedNodeOffsetCount++] = 0;
    const uint8_t* p = start;
    while ( p < end ) {
        uint64_t terminalSize = *p++;
        if ( terminalSize > 127 ) {
            // except for re-export-with-rename, all terminal sizes fit in one byte
            --p;
            terminalSize = read_uleb128(diag, p, end);
            if ( diag.hasError() )
                return nullptr;
        }
        if ( (*symbol == '\0') && (terminalSize != 0) ) {
            return p;
        }
        const uint8_t* children = p + terminalSize;
        if ( children > end ) {
            //diag.error("malformed trie node, terminalSize=0x%llX extends past end of trie\n", terminalSize);
            return nullptr;
        }
        uint8_t childrenRemaining = *children++;
        p = children;
        uint64_t nodeOffset = 0;
        for (; childrenRemaining > 0; --childrenRemaining) {
            const char* ss = symbol;
            bool wrongEdge = false;
            // scan whole edge to get to next edge
            // if edge is longer than target symbol name, don't read past end of symbol name
            char c = *p;
            while ( c != '\0' ) {
                if ( !wrongEdge ) {
                    if ( c != *ss )
                        wrongEdge = true;
                    ++ss;
                }
                ++p;
                c = *p;
            }
            if ( wrongEdge ) {
                // advance to next child
                ++p; // skip over zero terminator
                // skip over uleb128 until last byte is found
                while ( (*p & 0x80) != 0 )
                    ++p;
                ++p; // skip over last byte of uleb128
                if ( p > end ) {
                    diag.error("malformed trie node, child node extends past end of trie\n");
                    return nullptr;
                }
            }
            else {
                 // the symbol so far matches this edge (child)
                // so advance to the child's node
                ++p;
                nodeOffset = read_uleb128(diag, p, end);
                if ( diag.hasError() )
                    return nullptr;
                if ( (nodeOffset == 0) || ( &start[nodeOffset] > end) ) {
                    diag.error("malformed trie child, nodeOffset=0x%llX out of range\n", nodeOffset);
                    return nullptr;
                }
                symbol = ss;
                break;
            }
        }
        if ( nodeOffset != 0 ) {
            if ( nodeOffset > (uint64_t)(end-start) ) {
                diag.error("malformed trie child, nodeOffset=0x%llX out of range\n", nodeOffset);
               return nullptr;
            }
            for (int i=0; i < visitedNodeOffsetCount; ++i) {
                if ( visitedNodeOffsets[i] == nodeOffset ) {
                    diag.error("malformed trie child, cycle to nodeOffset=0x%llX\n", nodeOffset);
                    return nullptr;
                }
            }
            visitedNodeOffsets[visitedNodeOffsetCount++] = (uint32_t)nodeOffset;
            if ( visitedNodeOffsetCount >= 128 ) {
                diag.error("malformed trie too deep\n");
                return nullptr;
            }
            p = &start[nodeOffset];
        }
        else
            p = end;
    }
    return nullptr;
}

bool MachOLoaded::cdHashOfCodeSignature(const void* codeSigStart, size_t codeSignLen, uint8_t cdHash[20]) const
{
    const CS_CodeDirectory* cd = (const CS_CodeDirectory*)findCodeDirectoryBlob(codeSigStart, codeSignLen);
    if ( cd == nullptr )
        return false;

    uint32_t cdLength = htonl(cd->length);
    if ( cd->hashType == CS_HASHTYPE_SHA384 ) {
        uint8_t digest[CC_SHA384_DIGEST_LENGTH];
        CC_SHA384(cd, cdLength, digest);
        // cd-hash of sigs that use SHA384 is the first 20 bytes of the SHA384 of the code digest
        memcpy(cdHash, digest, 20);
        return true;
    }
    else if ( (cd->hashType == CS_HASHTYPE_SHA256) || (cd->hashType == CS_HASHTYPE_SHA256_TRUNCATED) ) {
        uint8_t digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(cd, cdLength, digest);
        // cd-hash of sigs that use SHA256 is the first 20 bytes of the SHA256 of the code digest
        memcpy(cdHash, digest, 20);
        return true;
    }
    else if ( cd->hashType == CS_HASHTYPE_SHA1 ) {
        // compute hash directly into return buffer
        CC_SHA1(cd, cdLength, cdHash);
        return true;
    }

    return false;
}


// Note, this has to match the kernel
static const uint32_t hashPriorities[] = {
    CS_HASHTYPE_SHA1,
    CS_HASHTYPE_SHA256_TRUNCATED,
    CS_HASHTYPE_SHA256,
    CS_HASHTYPE_SHA384,
};

static unsigned int hash_rank(const CS_CodeDirectory *cd)
{
    uint32_t type = cd->hashType;
    for (uint32_t n = 0; n < sizeof(hashPriorities) / sizeof(hashPriorities[0]); ++n) {
        if (hashPriorities[n] == type)
            return n + 1;
    }

    /* not supported */
    return 0;
}


// Note, this has to match the kernel
static const uint32_t hashPriorities_watchOS[] = {
    CS_HASHTYPE_SHA1
};

static unsigned int hash_rank_watchOS(const CS_CodeDirectory *cd)
{
    uint32_t type = cd->hashType;
    for (uint32_t n = 0; n < sizeof(hashPriorities_watchOS) / sizeof(hashPriorities_watchOS[0]); ++n) {
        if (hashPriorities_watchOS[n] == type)
            return n + 1;
    }

    /* not supported */
    return 0;
}

const void* MachOLoaded::findCodeDirectoryBlob(const void* codeSigStart, size_t codeSignLen) const
{
    // verify min length of overall code signature
    if ( codeSignLen < sizeof(CS_SuperBlob) )
        return nullptr;

    // verify magic at start
    const CS_SuperBlob* codeSuperBlob = (CS_SuperBlob*)codeSigStart;
    if ( codeSuperBlob->magic != htonl(CSMAGIC_EMBEDDED_SIGNATURE) )
        return nullptr;

    // verify count of sub-blobs not too large
    uint32_t subBlobCount = htonl(codeSuperBlob->count);
    if ( (codeSignLen-sizeof(CS_SuperBlob))/sizeof(CS_BlobIndex) < subBlobCount )
        return nullptr;

    // Note: The kernel currently always uses sha1 for watchOS, even if other hashes are available.
    const bool isWatchOS = this->supportsPlatform(Platform::watchOS);
    auto hashRankFn = isWatchOS ? &hash_rank_watchOS : &hash_rank;

    // walk each sub blob, looking at ones with type CSSLOT_CODEDIRECTORY
    const CS_CodeDirectory* bestCd = nullptr;
    for (uint32_t i=0; i < subBlobCount; ++i) {
        if ( codeSuperBlob->index[i].type == htonl(CSSLOT_CODEDIRECTORY) ) {
            // Ok, this is the regular code directory
        } else if ( codeSuperBlob->index[i].type >= htonl(CSSLOT_ALTERNATE_CODEDIRECTORIES) && codeSuperBlob->index[i].type <= htonl(CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT)) {
            // Ok, this is the alternative code directory
        } else {
            continue;
        }
        uint32_t cdOffset = htonl(codeSuperBlob->index[i].offset);
        // verify offset is not out of range
        if ( cdOffset > (codeSignLen - sizeof(CS_CodeDirectory)) )
            continue;
        const CS_CodeDirectory* cd = (CS_CodeDirectory*)((uint8_t*)codeSuperBlob + cdOffset);
        uint32_t cdLength = htonl(cd->length);
        // verify code directory length not out of range
        if ( cdLength > (codeSignLen - cdOffset) )
            continue;
        if ( cd->magic == htonl(CSMAGIC_CODEDIRECTORY) ) {
            if ( !bestCd || (hashRankFn(cd) > hashRankFn(bestCd)) )
                bestCd = cd;
        }
    }
    return bestCd;
}


// Regular pointer which needs to fit in 51-bits of value.
// C++ RTTI uses the top bit, so we'll allow the whole top-byte
// and the signed-extended bottom 43-bits to be fit in to 51-bits.
uint64_t MachOLoaded::ChainedFixupPointerOnDisk::signExtend51(uint64_t value51)
{
    uint64_t top8Bits     = value51 & 0x007F80000000000ULL;
    uint64_t bottom43Bits = value51 & 0x000007FFFFFFFFFFULL;
    uint64_t newValue     = (top8Bits << 13) | (((intptr_t)(bottom43Bits << 21) >> 21) & 0x00FFFFFFFFFFFFFF);
    return newValue;
}

uint64_t MachOLoaded::ChainedFixupPointerOnDisk::PlainRebase::signExtendedTarget() const
{
    return signExtend51(this->target);
}

uint64_t MachOLoaded::ChainedFixupPointerOnDisk::PlainBind::signExtendedAddend() const
{
    uint64_t addend19     = this->addend;
    if ( addend19 & 0x40000 )
        return addend19 | 0xFFFFFFFFFFFC0000ULL;
    else
        return addend19;
}

const char* MachOLoaded::ChainedFixupPointerOnDisk::keyName(uint8_t keyBits)
{
    static const char* names[] = {
        "IA", "IB", "DA", "DB"
    };
    assert(keyBits < 4);
    return names[keyBits];
}

const char* MachOLoaded::ChainedFixupPointerOnDisk::AuthRebase::keyName() const
{
    return ChainedFixupPointerOnDisk::keyName(this->key);
}

const char* MachOLoaded::ChainedFixupPointerOnDisk::AuthBind::keyName() const
{
    return ChainedFixupPointerOnDisk::keyName(this->key);
}


uint64_t MachOLoaded::ChainedFixupPointerOnDisk::signPointer(void* loc, uint64_t target) const
{
#if __has_feature(ptrauth_calls)
    uint64_t discriminator = authBind.diversity;
    if ( authBind.addrDiv )
        discriminator = __builtin_ptrauth_blend_discriminator(loc, discriminator);
    switch ( authBind.key ) {
        case 0: // IA
            return (uint64_t)__builtin_ptrauth_sign_unauthenticated((void*)target, 0, discriminator);
        case 1: // IB
            return (uint64_t)__builtin_ptrauth_sign_unauthenticated((void*)target, 1, discriminator);
        case 2: // DA
            return (uint64_t)__builtin_ptrauth_sign_unauthenticated((void*)target, 2, discriminator);
        case 3: // DB
            return (uint64_t)__builtin_ptrauth_sign_unauthenticated((void*)target, 3, discriminator);
    }
#endif
    return target;
}



} // namespace dyld3

