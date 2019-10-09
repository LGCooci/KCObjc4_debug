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

#ifndef MachOLoaded_h
#define MachOLoaded_h

#include <stdint.h>

#include "MachOFile.h"


class CacheBuilder;

namespace dyld3 {


// A mach-o mapped into memory with zero-fill expansion
// Can be used in dyld at runtime or during closure building
struct VIS_HIDDEN MachOLoaded : public MachOFile
{
	typedef const MachOLoaded* (^DependentToMachOLoaded)(const MachOLoaded* image, uint32_t depIndex);

    // for dlsym()
	bool                hasExportedSymbol(const char* symbolName, DependentToMachOLoaded finder, void** result,
                                          bool* resultPointsToInstructions) const;

    // for DYLD_PRINT_SEGMENTS
    const char*         segmentName(uint32_t segIndex) const;

    // used to see if main executable overlaps shared region
    bool                intersectsRange(uintptr_t start, uintptr_t length) const;

    // for _dyld_get_image_slide()
    intptr_t            getSlide() const;

    // quick check if image has been incorporated into the dyld cache
    bool                   inDyldCache() const { return (this->flags & 0x80000000); }

    // for dladdr()
    bool                findClosestSymbol(uint64_t unSlidAddr, const char** symbolName, uint64_t* symbolUnslidAddr) const;

    // for _dyld_find_unwind_sections()
    const void*         findSectionContent(const char* segName, const char* sectName, uint64_t& size) const;

    // used at runtime to validate loaded image matches closure
    bool                cdHashOfCodeSignature(const void* codeSigStart, size_t codeSignLen, uint8_t cdHash[20]) const;

    // used by DyldSharedCache to find closure
    static const uint8_t*   trieWalk(Diagnostics& diag, const uint8_t* start, const uint8_t* end, const char* symbol);

    // used by cache builder during error handling in chain bind processing
    const char*             dependentDylibLoadPath(uint32_t depIndex) const;

    // used by closure builder to find the offset and size of the trie.
    bool                    hasExportTrie(uint32_t& runtimeOffset, uint32_t& size) const;


    // For use with new rebase/bind scheme were each fixup location on disk contains info on what
    // fix up it needs plus the offset to the next fixup.
    union ChainedFixupPointerOnDisk
    {
        struct PlainRebase
        {
            uint64_t    target   : 51,
                        next     : 11,
                        bind     :  1,    // 0
                        auth     :  1;    // 0
            uint64_t    signExtendedTarget() const;
        };
        struct PlainBind
        {
            uint64_t    ordinal   : 16,
                        zero      : 16,
                        addend    : 19,
                        next      : 11,
                        bind      :  1,    // 1
                        auth      :  1;    // 0
            uint64_t    signExtendedAddend() const;
        };
        struct AuthRebase
        {
            uint64_t    target    : 32,
                        diversity : 16,
                        addrDiv   :  1,
                        key       :  2,
                        next      : 11,
                        bind      :  1,    // 0
                        auth      :  1;    // 1
            const char* keyName() const;
        };
        struct AuthBind
        {
            uint64_t    ordinal   : 16,
                        zero      : 16,
                        diversity : 16,
                        addrDiv   :  1,
                        key       :  2,
                        next      : 11,
                        bind      :  1,    // 1
                        auth      :  1;    // 1
            const char* keyName() const;
        };

        uint64_t        raw;
        AuthRebase      authRebase;
        AuthBind        authBind;
        PlainRebase     plainRebase;
        PlainBind       plainBind;

        static const char*  keyName(uint8_t keyBits);
        static uint64_t     signExtend51(uint64_t);
        uint64_t            signPointer(void* loc, uint64_t target) const;
     };

protected:
    friend CacheBuilder;

    struct FoundSymbol {
        enum class Kind { headerOffset, absolute, resolverOffset };
        Kind                kind;
        bool                isThreadLocal;
        bool                isWeakDef;
        const MachOLoaded*  foundInDylib;
        uint64_t            value;
        uint32_t            resolverFuncOffset;
        const char*         foundSymbolName;
    };

     struct LayoutInfo {
        uintptr_t    slide;
        uintptr_t    textUnslidVMAddr;
        uintptr_t    linkeditUnslidVMAddr;
        uint32_t     linkeditFileOffset;
        uint32_t     linkeditFileSize;
        uint32_t     linkeditSegIndex;
    };

    struct LinkEditInfo
    {
        const dyld_info_command*     dyldInfo;
        const symtab_command*        symTab;
        const dysymtab_command*      dynSymTab;
        const linkedit_data_command* splitSegInfo;
        const linkedit_data_command* functionStarts;
        const linkedit_data_command* dataInCode;
        const linkedit_data_command* codeSig;
        LayoutInfo                   layout;
    };

    bool                    findExportedSymbol(Diagnostics& diag, const char* symbolName, FoundSymbol& foundInfo, DependentToMachOLoaded finder) const;
    void                    getLinkEditPointers(Diagnostics& diag, LinkEditInfo&) const;
    void                    getLinkEditLoadCommands(Diagnostics& diag, LinkEditInfo& result) const;
    void                    getLayoutInfo(LayoutInfo&) const;
    const uint8_t*          getLinkEditContent(const LayoutInfo& info, uint32_t fileOffset) const;
    void                    forEachGlobalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const;
    void                    forEachLocalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const;
    uint32_t                dependentDylibCount() const;
    bool                    findClosestFunctionStart(uint64_t address, uint64_t* functionStartAddress) const;

    const void*             findCodeDirectoryBlob(const void* codeSigStart, size_t codeSignLen) const;

};

} // namespace dyld3

#endif /* MachOLoaded_h */
