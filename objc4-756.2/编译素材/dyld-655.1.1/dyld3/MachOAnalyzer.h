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

#ifndef MachOAnalyzer_h
#define MachOAnalyzer_h


#define BIND_SPECIAL_DYLIB_WEAK_DEF_COALESCE (-3)

#include "MachOLoaded.h"
#include "ClosureFileSystem.h"

namespace dyld3 {

// Extra functionality on loaded mach-o files only used during closure building
struct VIS_HIDDEN MachOAnalyzer : public MachOLoaded
{
    static closure::LoadedFileInfo load(Diagnostics& diag, const closure::FileSystem& fileSystem, const char* logicalPath, const char* reqArchName, Platform reqPlatform);
    static const MachOAnalyzer*  validMainExecutable(Diagnostics& diag, const mach_header* mh, const char* path, uint64_t sliceLength, const char* reqArchName, Platform reqPlatform);

    bool                validMachOForArchAndPlatform(Diagnostics& diag, size_t mappedSize, const char* path, const char* reqArchName, Platform reqPlatform) const;
    uint64_t            mappedSize() const;
    bool                hasObjC() const;
    bool                hasPlusLoadMethod(Diagnostics& diag) const;
    uint64_t            preferredLoadAddress() const;
    void                forEachLocalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const;
    void                forEachRPath(void (^callback)(const char* rPath, bool& stop)) const;

    bool                isEncrypted() const;
    bool                getCDHash(uint8_t cdHash[20]) const;
    bool                hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const;
    bool                usesLibraryValidation() const;
    bool                isRestricted() const;
    bool                getEntry(uint32_t& offset, bool& usesCRT) const;
    bool                isSlideable() const;
    bool                hasInitializer(Diagnostics& diag, bool contentRebased, const void* dyldCache=nullptr) const;
    void                forEachInitializer(Diagnostics& diag, bool contentRebased, void (^callback)(uint32_t offset), const void* dyldCache=nullptr) const;
    void                forEachDOFSection(Diagnostics& diag, void (^callback)(uint32_t offset)) const;
    uint32_t            segmentCount() const;
    void                forEachExportedSymbol(Diagnostics diag, void (^callback)(const char* symbolName, uint64_t imageOffset, bool isReExport, bool& stop)) const;
    void                forEachRebase(Diagnostics& diag, void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, bool& stop)) const;
    void                forEachWeakDef(Diagnostics& diag, void (^callback)(bool strongDef, uint32_t dataSegIndex, uint64_t dataSegOffset,
                                                    uint64_t addend, const char* symbolName, bool& stop)) const;
    void                forEachIndirectPointer(Diagnostics& diag, void (^handler)(uint64_t pointerAddress, bool bind, int bindLibOrdinal,
                                                                                  const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& stop)) const;
    void                forEachInterposingSection(Diagnostics& diag, void (^handler)(uint64_t vmOffset, uint64_t vmSize, bool& stop)) const;
    const void*         content(uint64_t vmOffset);
    void                forEachLocalReloc(void (^handler)(uint64_t runtimeOffset, bool& stop)) const;
    void                forEachExternalReloc(void (^handler)(uint64_t runtimeOffset, int libOrdinal, const char* symbolName, bool& stop)) const;

    const void*         getRebaseOpcodes(uint32_t& size) const;
    const void*         getBindOpcodes(uint32_t& size) const;
    const void*         getLazyBindOpcodes(uint32_t& size) const;
    uint64_t            segAndOffsetToRuntimeOffset(uint8_t segIndex, uint64_t segOffset) const;
    bool                hasLazyPointers(uint32_t& runtimeOffset, uint32_t& size) const;
    void                forEachRebase(Diagnostics& diag, bool ignoreLazyPointer, void (^callback)(uint64_t runtimeOffset, bool& stop)) const;
    void                forEachTextRebase(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, bool& stop)) const;
    void                forEachBind(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, int libOrdinal, const char* symbolName,
                                                                        bool weakImport, uint64_t addend, bool& stop),
                                                       void (^strongHandler)(const char* symbolName)) const;
    void                forEachChainedFixupTarget(Diagnostics& diag, void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop)) const;
    void                forEachChainedFixupStart(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, bool& stop)) const;
    bool                canHavePrecomputedDlopenClosure(const char* path, void (^failureReason)(const char*)) const;
    bool                canBePlacedInDyldCache(const char* path, void (^failureReason)(const char*)) const;
 
#if DEBUG
    void                validateDyldCacheDylib(Diagnostics& diag, const char* path) const;
#endif

    const MachOAnalyzer*    remapIfZeroFill(Diagnostics& diag, const closure::FileSystem& fileSystem, closure::LoadedFileInfo& info) const;

    // protected members of subclass promoted to public here
    using MachOLoaded::SegmentInfo;
    using MachOLoaded::SectionInfo;
    using MachOLoaded::forEachSegment;
    using MachOLoaded::forEachSection;
    using MachOLoaded::forEachDependentDylib;
    using MachOLoaded::getDylibInstallName;
    using MachOLoaded::FoundSymbol;
    using MachOLoaded::findExportedSymbol;
    
private:

    struct SegmentStuff
    {
        uint64_t    fileOffset;
        uint64_t    fileSize;
        uint64_t    writable          :  1,
                    executable        :  1,
                    textRelocsAllowed :  1,  // segment supports text relocs (i386 only)
                    segSize           : 61;
	};

    enum class Malformed { linkeditOrder, linkeditAlignment, dyldInfoAndlocalRelocs };
    bool                    enforceFormat(Malformed) const;

    const uint8_t*          getContentForVMAddr(const LayoutInfo& info, uint64_t vmAddr) const;

    bool                    validLoadCommands(Diagnostics& diag, const char* path, size_t fileLen) const;
    bool                    validEmbeddedPaths(Diagnostics& diag, const char* path) const;
    bool                    validSegments(Diagnostics& diag, const char* path, size_t fileLen) const;
    bool                    validLinkedit(Diagnostics& diag, const char* path) const;
    bool                    validLinkeditLayout(Diagnostics& diag, const char* path) const;
    bool                    validRebaseInfo(Diagnostics& diag, const char* path) const;
    bool                    validBindInfo(Diagnostics& diag, const char* path) const;
    bool                    validMain(Diagnostics& diag, const char* path) const;
    bool                    validChainedFixupsInfo(Diagnostics& diag, const char* path) const;

    bool                    invalidRebaseState(Diagnostics& diag, const char* opcodeName, const char* path, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                                              bool segIndexSet, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type) const;
    bool                    invalidBindState(Diagnostics& diag, const char* opcodeName, const char* path, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                                              bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint32_t pointerSize,
                                              uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, const char* symbolName) const;
    bool                    doLocalReloc(Diagnostics& diag, uint32_t r_address, bool& stop, void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, bool& stop)) const;
    uint8_t                 relocPointerType() const;
    int                     libOrdinalFromDesc(uint16_t n_desc) const;
    bool                    doExternalReloc(Diagnostics& diag, uint32_t r_address, uint32_t r_symbolnum, LinkEditInfo& leInfo, bool& stop,
                                            void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, int libOrdinal,
                                                             uint64_t addend, const char* symbolName, bool weakImport, bool lazy, bool& stop)) const;

    void                    getAllSegmentsInfos(Diagnostics& diag, SegmentInfo segments[]) const;
    bool                    segmentHasTextRelocs(uint32_t segIndex) const;
    uint64_t                relocBaseAddress(const SegmentInfo segmentsInfos[], uint32_t segCount) const;
    void                    forEachRebase(Diagnostics& diag, void (^handler)(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                                                                             bool segIndexSet, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, bool& stop)) const;
    bool                    segIndexAndOffsetForAddress(uint64_t addr, const SegmentInfo segmentsInfos[], uint32_t segCount, uint32_t& segIndex, uint64_t& segOffset) const;
    void                    forEachBind(Diagnostics& diag, void (^handler)(const char* opcodeName, const LinkEditInfo& leInfo, const SegmentInfo segments[],
                                                                           bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                                                           uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                                                           uint8_t type, const char* symbolName, bool weakImport, uint64_t addend, bool& stop),
                                                           void (^strongHandler)(const char* symbolName)) const;
    void                    forEachChainedFixup(Diagnostics& diag, void (^targetCount)(uint32_t totalTargets, bool& stop),
                                                                   void (^addTarget)(const LinkEditInfo& leInfo, const SegmentInfo segments[], bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop),
                                                                   void (^addChainStart)(const LinkEditInfo& leInfo, const SegmentInfo segments[], uint8_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, bool& stop)) const;
    bool                    contentIsRegularStub(const uint8_t* helperContent) const;
    uint64_t                entryAddrFromThreadCmd(const thread_command* cmd) const;

};

} // namespace dyld3

#endif /* MachOAnalyzer_h */
