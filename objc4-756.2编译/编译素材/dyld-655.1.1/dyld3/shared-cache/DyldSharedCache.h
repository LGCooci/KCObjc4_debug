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


#ifndef DyldSharedCache_h
#define DyldSharedCache_h

#include <set>
#include <string>
#include <vector>
#include <unordered_map>
#include <uuid/uuid.h>

#include "dyld_cache_format.h"
#include "Diagnostics.h"
#include "MachOAnalyzer.h"
#include "Closure.h"


class VIS_HIDDEN DyldSharedCache
{
public:

    enum CodeSigningDigestMode
    {
        SHA256only = 0,
        SHA1only   = 1,
        Agile      = 2
    };

    struct CreateOptions
    {
        std::string                                 outputFilePath;
        std::string                                 outputMapFilePath;
        std::string                                 archName;
        dyld3::Platform                             platform;
        bool                                        excludeLocalSymbols;
        bool                                        optimizeStubs;
        bool                                        optimizeObjC;
        CodeSigningDigestMode                       codeSigningDigestMode;
        bool                                        dylibsRemovedDuringMastering;
        bool                                        inodesAreSameAsRuntime;
        bool                                        cacheSupportsASLR;
        bool                                        forSimulator;
        bool                                        isLocallyBuiltCache;
        bool                                        verbose;
        bool                                        evictLeafDylibsOnOverflow;
        std::unordered_map<std::string, unsigned>   dylibOrdering;
        std::unordered_map<std::string, unsigned>   dirtyDataSegmentOrdering;
        std::vector<std::string>                    pathPrefixes;
        std::string                                 loggingPrefix;
    };

    struct MappedMachO
    {
                                    MappedMachO()
                                            : mh(nullptr), length(0), isSetUID(false), protectedBySIP(false), sliceFileOffset(0), modTime(0), inode(0) { }
                                    MappedMachO(const std::string& path, const dyld3::MachOAnalyzer* p, size_t l, bool isu, bool sip, uint64_t o, uint64_t m, uint64_t i)
                                            : runtimePath(path), mh(p), length(l), isSetUID(isu), protectedBySIP(sip), sliceFileOffset(o), modTime(m), inode(i) { }

        std::string                 runtimePath;
        const dyld3::MachOAnalyzer* mh;
        size_t                      length;
        uint64_t                    isSetUID        :  1,
                                    protectedBySIP  :  1,
                                    sliceFileOffset : 62;
        uint64_t                    modTime;                // only recorded if inodesAreSameAsRuntime
        uint64_t                    inode;                  // only recorded if inodesAreSameAsRuntime
    };

    struct CreateResults
    {
        std::string                             errorMessage;
        std::set<std::string>                   warnings;
        std::set<const dyld3::MachOAnalyzer*>   evictions;
        bool                                    agileSignature  = false;
        std::string                             cdHashFirst;
        std::string                             cdHashSecond;
    };


    struct FileAlias
    {
        std::string             realPath;
        std::string             aliasPath;
    };


    // This function verifies the set of dylibs that will go into the cache are self contained.  That the depend on no dylibs
    // outset the set.  It will call back the loader function to try to find any mising dylibs.
    static bool verifySelfContained(std::vector<MappedMachO>& dylibsToCache, MappedMachO (^loader)(const std::string& runtimePath), std::vector<std::pair<DyldSharedCache::MappedMachO, std::set<std::string>>>& excluded);


    //
    // This function is single threaded and creates a shared cache. The cache file is created in-memory.
    //
    // Inputs:
    //      options:        various per-platform flags
    //      dylibsToCache:  a list of dylibs to include in the cache
    //      otherOsDylibs:  a list of other OS dylibs and bundle which should have load info added to the cache
    //      osExecutables:  a list of main executables which should have closures created in the cache
    //
    // Returns:
    //    On success:
    //         cacheContent: start of the allocated cache buffer which must be vm_deallocated after the caller writes out the buffer.
    //         cacheLength:  size of the allocated cache buffer
    //         cdHash:       hash of the code directory of the code blob of the created cache
    //         warnings:     all warning messsages generated during the creation of the cache
    //
    //    On failure:
    //         cacheContent: nullptr
    //         errorMessage: the string describing why the cache could not be created
    //         warnings:     all warning messsages generated before the failure
    //
    static CreateResults create(const CreateOptions&             options,
                                const std::vector<MappedMachO>&  dylibsToCache,
                                const std::vector<MappedMachO>&  otherOsDylibs,
                                const std::vector<MappedMachO>&  osExecutables);


    //
    // Returns a text "map" file as a big string
    //
    std::string         mapFile() const;


    //
    // Returns the architecture name of the shared cache, e.g. "arm64"
    //
    const char*         archName() const;


    //
    // Returns the platform the cache is for
    //
    dyld3::Platform    platform() const;


    //
    // Iterates over each dylib in the cache
    //
    void                forEachImage(void (^handler)(const mach_header* mh, const char* installName)) const;


    //
    // Searches cache for dylib with specified path
    //
    bool                hasImagePath(const char* dylibPath, uint32_t& imageIndex) const;


    //
    // Searches cache for dylib with specified mach_header
    //
    bool                findMachHeaderImageIndex(const mach_header* mh, uint32_t& imageIndex) const;

   //
    // Iterates over each dylib in the cache
    //
    void                forEachImageEntry(void (^handler)(const char* path, uint64_t mTime, uint64_t inode)) const;


    //
    // Iterates over each dylib in the cache
    //
    const mach_header*  getIndexedImageEntry(uint32_t index, uint64_t& mTime, uint64_t& node) const;


    //
    // Iterates over each dylib in the cache
    //
    void                forEachImageTextSegment(void (^handler)(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const uuid_t dylibUUID, const char* installName, bool& stop)) const;


    //
    // Iterates over each of the three regions in the cache
    //
    void                forEachRegion(void (^handler)(const void* content, uint64_t vmAddr, uint64_t size, uint32_t permissions)) const;


    //
    // Returns if an address range is in this cache, and if so if in a read-only area
    //
    bool                inCache(const void* addr, size_t length, bool& readOnly) const;


    //
    // returns address the cache would load at if unslid
    //
    uint64_t            unslidLoadAddress() const;


    //
    // returns UUID of cache
    //
    void                getUUID(uuid_t uuid) const;


    //
    // returns the vm size required to map cache
    //
    uint64_t            mappedSize() const;


    //
    // searches cache for pre-built closure for program
    //
    const dyld3::closure::LaunchClosure* findClosure(const char* executablePath) const;


    //
    // iterates all pre-built closures for program
    //
    void forEachLaunchClosure(void (^handler)(const char* executableRuntimePath, const dyld3::closure::LaunchClosure* closure)) const;


    //
    // iterates all pre-built Image* for OS dylibs/bundles not in dyld cache
    //
    void forEachDlopenImage(void (^handler)(const char* runtimePath, const dyld3::closure::Image* image)) const;


    //
    // returns the ImageArray pointer to Images in dyld shared cache
    //
    const dyld3::closure::ImageArray*  cachedDylibsImageArray() const;


    //
    // returns the ImageArray pointer to Images in OS with pre-build dlopen closure
    //
    const dyld3::closure::ImageArray*  otherOSImageArray() const;


    //
    // searches cache for pre-built dlopen closure for OS dylib/bundle
    //
    const dyld3::closure::Image* findDlopenOtherImage(const char* path) const;


    //
    // returns true if the offset is in the TEXT of some cached dylib and sets *index to the dylib index
    //
    bool              addressInText(uint32_t cacheOffset, uint32_t* index) const;



    dyld_cache_header header;
};








#endif /* DyldSharedCache_h */
