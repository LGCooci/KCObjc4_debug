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


#ifndef CacheBuilder_h
#define CacheBuilder_h

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "ClosureFileSystem.h"
#include "DyldSharedCache.h"
#include "Diagnostics.h"
#include "MachOAnalyzer.h"



template <typename P> class LinkeditOptimizer;


class CacheBuilder {
public:
    CacheBuilder(const DyldSharedCache::CreateOptions& options, const dyld3::closure::FileSystem& fileSystem);

    struct InputFile {
        enum State {
            Unset,
            MustBeIncluded,
            MustBeIncludedForDependent,
            MustBeExcludedIfUnused
        };
        InputFile(const char* path, State state) : path(path), state(state) { }
        const char*     path;
        State           state = Unset;
        Diagnostics     diag;

        bool mustBeIncluded() const {
            return (state == MustBeIncluded) || (state == MustBeIncludedForDependent);
        }
    };

    // Contains a MachO which has been loaded from the file system and may potentially need to be unloaded later.
    struct LoadedMachO {
        DyldSharedCache::MappedMachO    mappedFile;
        dyld3::closure::LoadedFileInfo  loadedFileInfo;
        InputFile*                      inputFile;
    };

    void                                        build(std::vector<InputFile>& inputFiles,
                                                      std::vector<DyldSharedCache::FileAlias>& aliases);
    void                                        build(const std::vector<LoadedMachO>& dylibs,
                                                      const std::vector<LoadedMachO>& otherOsDylibsInput,
                                                      const std::vector<LoadedMachO>& osExecutables,
                                                      std::vector<DyldSharedCache::FileAlias>& aliases);
    void                                        build(const std::vector<DyldSharedCache::MappedMachO>&  dylibsToCache,
                                                      const std::vector<DyldSharedCache::MappedMachO>&  otherOsDylibs,
                                                      const std::vector<DyldSharedCache::MappedMachO>&  osExecutables,
                                                      std::vector<DyldSharedCache::FileAlias>& aliases);
    void                                        writeFile(const std::string& path);
    void                                        writeBuffer(uint8_t*& buffer, uint64_t& size);
    void                                        writeMapFile(const std::string& path);
    void                                        writeMapFileBuffer(uint8_t*& buffer, uint64_t& bufferSize);
    void                                        deleteBuffer();
    std::string                                 errorMessage();
    const std::set<std::string>                 warnings();
    const std::set<const dyld3::MachOAnalyzer*> evictions();
    const bool                                  agileSignature();
    const std::string                           cdHashFirst();
    const std::string                           cdHashSecond();

    void forEachCacheDylib(void (^callback)(const std::string& path));

    struct SegmentMappingInfo {
        const void*     srcSegment;
        const char*     segName;
        void*           dstSegment;
        uint64_t        dstCacheUnslidAddress;
        uint32_t        dstCacheFileOffset;
        uint32_t        dstCacheSegmentSize;
        uint32_t        copySegmentSize;
        uint32_t        srcSegmentIndex;
    };

    class ASLR_Tracker
    {
    public:
                ~ASLR_Tracker();

        void        setDataRegion(const void* rwRegionStart, size_t rwRegionSize);
        void        add(void* p);
        void        remove(void* p);
        bool        has(void* p);
        const bool* bitmap()        { return _bitmap; }
        unsigned    dataPageCount() { return _pageCount; }

    private:

        uint8_t*     _regionStart    = nullptr;
        uint8_t*     _endStart       = nullptr;
        bool*        _bitmap         = nullptr;
        unsigned     _pageCount      = 0;
        unsigned     _pageSize       = 4096;
    };

    typedef std::map<uint64_t, std::set<void*>> LOH_Tracker;

    struct Region
    {
        uint8_t*    buffer                 = nullptr;
        uint64_t    bufferSize             = 0;
        uint64_t    sizeInUse              = 0;
        uint64_t    unslidLoadAddress      = 0;
        uint64_t    cacheFileOffset        = 0;
    };

private:
    template <typename P>
    friend class LinkeditOptimizer;
    
    struct ArchLayout
    {
        uint64_t    sharedMemoryStart;
        uint64_t    sharedMemorySize;
        uint64_t    sharedRegionPadding;
        uint64_t    pointerDeltaMask;
        const char* archName;
        uint32_t    branchPoolTextSize;
        uint32_t    branchPoolLinkEditSize;
        uint32_t    branchReach;
        uint8_t     sharedRegionAlignP2;
        uint8_t     slideInfoBytesPerPage;
        bool        sharedRegionsAreDiscontiguous;
        bool        is64;
    };

    static const ArchLayout  _s_archLayout[];
    static const char* const _s_neverStubEliminate[];

    struct UnmappedRegion
    {
        uint8_t*    buffer                 = nullptr;
        uint64_t    bufferSize             = 0;
        uint64_t    sizeInUse              = 0;
    };

    struct DylibInfo
    {
        const LoadedMachO*              input;
        std::string                     runtimePath;
        std::vector<SegmentMappingInfo> cacheLocation;
    };

    void        makeSortedDylibs(const std::vector<LoadedMachO>& dylibs, const std::unordered_map<std::string, unsigned> sortOrder);
    void        assignSegmentAddresses();

    uint64_t    cacheOverflowAmount();
    size_t      evictLeafDylibs(uint64_t reductionTarget, std::vector<const LoadedMachO*>& overflowDylibs);

    void        fipsSign();
    void        codeSign();
    uint64_t    pathHash(const char* path);
    void        writeCacheHeader();
    void        copyRawSegments();
    void        adjustAllImagesForNewSegmentLocations();
    void        writeSlideInfoV1();
    void        writeSlideInfoV3(const bool bitmap[], unsigned dataPageCoun);
    uint16_t    pageStartV3(uint8_t* pageContent, uint32_t pageSize, const bool bitmap[]);
    void        findDylibAndSegment(const void* contentPtr, std::string& dylibName, std::string& segName);
    void        addImageArray();
    void        buildImageArray(std::vector<DyldSharedCache::FileAlias>& aliases);
    void        addOtherImageArray(const std::vector<LoadedMachO>&, std::vector<const LoadedMachO*>& overflowDylibs);
    void        addClosures(const std::vector<LoadedMachO>&);
    void        markPaddingInaccessible();

    bool        writeCache(void (^cacheSizeCallback)(uint64_t size), bool (^copyCallback)(const uint8_t* src, uint64_t size, uint64_t dstOffset));

    template <typename P> void writeSlideInfoV2(const bool bitmap[], unsigned dataPageCount);
    template <typename P> bool makeRebaseChainV2(uint8_t* pageContent, uint16_t lastLocationOffset, uint16_t newOffset, const struct dyld_cache_slide_info2* info);
    template <typename P> void addPageStartsV2(uint8_t* pageContent, const bool bitmap[], const struct dyld_cache_slide_info2* info,
                                             std::vector<uint16_t>& pageStarts, std::vector<uint16_t>& pageExtras);

    template <typename P> void writeSlideInfoV4(const bool bitmap[], unsigned dataPageCount);
    template <typename P> bool makeRebaseChainV4(uint8_t* pageContent, uint16_t lastLocationOffset, uint16_t newOffset, const struct dyld_cache_slide_info4* info);
    template <typename P> void addPageStartsV4(uint8_t* pageContent, const bool bitmap[], const struct dyld_cache_slide_info4* info,
                                             std::vector<uint16_t>& pageStarts, std::vector<uint16_t>& pageExtras);

    // implemented in AdjustDylibSegemnts.cpp
    void        adjustDylibSegments(const DylibInfo& dylib, Diagnostics& diag) const;

    // implemented in OptimizerLinkedit.cpp
    void        optimizeLinkedit(const std::vector<uint64_t>& branchPoolOffsets);

    // implemented in OptimizerObjC.cpp
    void        optimizeObjC();

    // implemented in OptimizerBranches.cpp
    void        optimizeAwayStubs(const std::vector<uint64_t>& branchPoolStartAddrs, uint64_t branchPoolsLinkEditStartAddr);


    typedef std::unordered_map<std::string, const dyld3::MachOAnalyzer*> InstallNameToMA;


    const DyldSharedCache::CreateOptions&       _options;
    const dyld3::closure::FileSystem&           _fileSystem;
    Region                                      _readExecuteRegion;
    Region                                      _readWriteRegion;
    Region                                      _readOnlyRegion;
    UnmappedRegion                              _localSymbolsRegion;
    UnmappedRegion                              _codeSignatureRegion;
    vm_address_t                                _fullAllocatedBuffer;
    uint64_t                                    _nonLinkEditReadOnlySize;
    Diagnostics                                 _diagnostics;
    std::set<const dyld3::MachOAnalyzer*>       _evictions;
    const ArchLayout*                           _archLayout;
    uint32_t                                    _aliasCount;
    uint64_t                                    _slideInfoFileOffset;
    uint64_t                                    _slideInfoBufferSizeAllocated;
    uint64_t                                    _allocatedBufferSize;
    std::vector<DylibInfo>                      _sortedDylibs;
    InstallNameToMA                             _installNameToCacheDylib;
    std::unordered_map<std::string, uint32_t>   _dataDirtySegsOrder;
    // Note this is mutable as the only parallel writes to it are done atomically to the bitmap
    mutable ASLR_Tracker                        _aslrTracker;
    std::map<void*, std::string>                _missingWeakImports;
    mutable LOH_Tracker                         _lohTracker;
    const dyld3::closure::ImageArray*           _imageArray;
    uint32_t                                    _sharedStringsPoolVmOffset;
    std::vector<uint64_t>                       _branchPoolStarts;
    uint64_t                                    _branchPoolsLinkEditStartAddr;
    uint8_t                                     _cdHashFirst[20];
    uint8_t                                     _cdHashSecond[20];
};




inline uint64_t align(uint64_t addr, uint8_t p2)
{
    uint64_t mask = (1 << p2);
    return (addr + mask - 1) & (-mask);
}



#endif /* CacheBuilder_h */
