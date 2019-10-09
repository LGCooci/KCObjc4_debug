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


#ifndef Closures_h
#define Closures_h


#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <mach/mach.h>
#include <mach-o/loader.h>

#include "Diagnostics.h"
#include "Array.h"
#include "MachOLoaded.h"
#include "SupportedArchs.h"

namespace dyld3 {
namespace closure {



// bump this number each time binary format changes
enum  { kFormatVersion = 10 };


typedef uint32_t ImageNum;

const ImageNum kFirstDyldCacheImageNum      = 0x00000001;
const ImageNum kLastDyldCacheImageNum       = 0x00000FFF;
const ImageNum kFirstOtherOSImageNum        = 0x00001001;
const ImageNum kLastOtherOSImageNum         = 0x00001FFF;
const ImageNum kFirstLaunchClosureImageNum  = 0x00002000;
const ImageNum kMissingWeakLinkedImage      = 0x0FFFFFFF;


//
//  Generic typed range of bytes (similar to load commands)
//  Must be 4-byte aligned
//
struct VIS_HIDDEN TypedBytes
{
    uint32_t     type          : 8,
                 payloadLength : 24;

    enum class Type {
        // containers which have an overall length and TypedBytes inside their content
        launchClosure    =  1, // contains TypedBytes of closure attributes including imageArray
        imageArray       =  2, // sizeof(ImageArray) + sizeof(uint32_t)*count + size of all images
        image            =  3, // contains TypedBytes of image attributes
        dlopenClosure    =  4, // contains TypedBytes of closure attributes including imageArray

        // attributes for Images
        imageFlags       =  7, // sizeof(Image::Flags)
        pathWithHash     =  8, // len = uint32_t + length path + 1, use multiple entries for aliases
        fileInodeAndTime =  9, // sizeof(FileInfo)
        cdHash           = 10, // 20
        uuid             = 11, // 16
        mappingInfo      = 12, // sizeof(MappingInfo)
        diskSegment      = 13, // sizeof(DiskSegment) * count
        cacheSegment     = 14, // sizeof(DyldCacheSegment) * count
        dependents       = 15, // sizeof(LinkedImage) * count
        initOffsets      = 16, // sizeof(uint32_t) * count
        dofOffsets       = 17, // sizeof(uint32_t) * count
        codeSignLoc      = 18, // sizeof(CodeSignatureLocation)
        fairPlayLoc      = 19, // sizeof(FairPlayRange)
        rebaseFixups     = 20, // sizeof(RebasePattern) * count
        bindFixups       = 21, // sizeof(BindPattern) * count
        cachePatchInfo   = 22, // sizeof(PatchableExport) + count*sizeof(PatchLocation) + strlen(name) // only in dyld cache Images
        textFixups       = 23, // sizeof(TextFixupPattern) * count
        imageOverride    = 24, // sizeof(ImageNum)
        initBefores      = 25, // sizeof(ImageNum) * count
        chainedFixupsStarts  = 26, // sizeof(uint64_t) * count
        chainedFixupsTargets = 27, // sizeof(ResolvedSymbolTarget) * count

        // attributes for Closures (launch or dlopen)
        closureFlags     = 32,  // sizeof(Closure::Flags)
        dyldCacheUUID    = 33,  // 16
        missingFiles     = 34,
        envVar           = 35,  // "DYLD_BLAH=stuff"
        topImage         = 36,  // sizeof(ImageNum)
        libDyldEntry     = 37,  // sizeof(ResolvedSymbolTarget)
        libSystemNum     = 38,  // sizeof(ImageNum)
        bootUUID         = 39,  // c-string 40
        mainEntry        = 40,  // sizeof(ResolvedSymbolTarget)
        startEntry       = 41,  // sizeof(ResolvedSymbolTarget)     // used by programs built with crt1.o
        cacheOverrides   = 42,  // sizeof(PatchEntry) * count       // used if process uses interposing or roots (cached dylib overrides)
        interposeTuples  = 43,  // sizeof(InterposingTuple) * count
   };

    const void*     payload() const;
    void*           payload();
};


//
//  A TypedBytes which is a bag of other TypedBytes
//
struct VIS_HIDDEN ContainerTypedBytes : TypedBytes
{
    void                forEachAttribute(void (^callback)(const TypedBytes* typedBytes, bool& stop)) const;
    void                forEachAttributePayload(Type requestedType, void (^handler)(const void* payload, uint32_t size, bool& stop)) const;
    const void*         findAttributePayload(Type requestedType, uint32_t* payloadSize=nullptr) const;
private:
    const TypedBytes*   first() const;
    const TypedBytes*   next(const TypedBytes*) const;
};


//
//  Information about a mach-o file
//
struct VIS_HIDDEN Image : ContainerTypedBytes
{
    enum class LinkKind { regular=0, weak=1, upward=2, reExport=3 };

    size_t              size() const;
    ImageNum            imageNum() const;
    bool                representsImageNum(ImageNum num) const;  // imageNum() or isOverrideOfDyldCacheImage()
    uint32_t            maxLoadCount() const;
    const char*         path() const;
    const char*         leafName() const;
    bool                getUuid(uuid_t) const;
    bool                isInvalid() const;
    bool                inDyldCache() const;
    bool                hasObjC() const;
    bool                hasInitializers() const;
    bool                isBundle() const;
    bool                isDylib() const;
    bool                isExecutable() const;
    bool                hasWeakDefs() const;
    bool                mayHavePlusLoads() const;
    bool                is64() const;
    bool                neverUnload() const;
    bool                cwdMustBeThisDir() const;
    bool                isPlatformBinary() const;
    bool                overridableDylib() const;
    bool                hasFileModTimeAndInode(uint64_t& inode, uint64_t& mTime) const;
    bool                hasCdHash(uint8_t cdHash[20]) const;
    void                forEachAlias(void (^handler)(const char* aliasPath, bool& stop)) const;
    void                forEachDependentImage(void (^handler)(uint32_t dependentIndex, LinkKind kind, ImageNum imageNum, bool& stop)) const;
    ImageNum            dependentImageNum(uint32_t depIndex) const;
    bool                containsAddress(const void* addr, const void* imageLoadAddress, uint8_t* permissions=nullptr) const;
    void                forEachInitializer(const void* imageLoadAddress, void (^handler)(const void* initializer)) const;
    void                forEachImageToInitBefore(void (^handler)(ImageNum imageToInit, bool& stop)) const;
    void                forEachDOF(const void* imageLoadAddress, void (^handler)(const void* initializer)) const;
    bool                hasPathWithHash(const char* path, uint32_t hash) const;
    bool                isOverrideOfDyldCacheImage(ImageNum& cacheImageNum) const;
    uint64_t            textSize() const;

	union ResolvedSymbolTarget
    {
        enum Kinds { kindRebase, kindSharedCache, kindImage, kindAbsolute };

        struct Rebase {
            uint64_t    kind            :  2,       // kindRebase
                        unused          : 62;       // all zeros
        };
        struct SharedCache {
            uint64_t    kind            :  2,       // kindSharedCache
                        offset          : 62;
        };
        struct Image {
            uint64_t    kind            :  2,       // kindImage
                        imageNum        : 22,       // ImageNum
                        offset          : 40;
        };
        struct Absolute {
            uint64_t    kind            :  2,       // kindAbsolute
                        value           : 62;       // sign extended
        };
        Rebase          rebase;
        SharedCache     sharedCache;
        Image           image;
        Absolute        absolute;
        uint64_t        raw;

        bool operator==(const ResolvedSymbolTarget& rhs) const {
            return (raw == rhs.raw);
        }
        bool operator!=(const ResolvedSymbolTarget& rhs) const {
            return (raw != rhs.raw);
        }
     };

    typedef MachOLoaded::ChainedFixupPointerOnDisk ChainedFixupPointerOnDisk;

    // the following are only valid if inDyldCache() returns true
    uint32_t            cacheOffset() const;
    uint32_t            patchStartIndex() const;
    uint32_t            patchCount() const;
    void                forEachCacheSegment(void (^handler)(uint32_t segIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop)) const;


    // the following are only valid if inDyldCache() returns false
    uint64_t            vmSizeToMap() const;
    uint64_t            sliceOffsetInFile() const;
    bool                hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const;
    bool                isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const;
    void                forEachDiskSegment(void (^handler)(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop)) const;
    void                forEachFixup(void (^rebase)(uint64_t imageOffsetToRebase, bool& stop),
                                     void (^bind)(uint64_t imageOffsetToBind, ResolvedSymbolTarget bindTarget, bool& stop),
                                     void (^chainedFixupStart)(uint64_t imageOffsetStart, const Array<ResolvedSymbolTarget>& targets, bool& stop)) const;
    void                forEachTextReloc(void (^rebase)(uint32_t imageOffsetToRebase, bool& stop),
                                         void (^bind)(uint32_t imageOffsetToBind, ResolvedSymbolTarget bindTarget, bool& stop)) const;
    static void         forEachChainedFixup(void* imageLoadAddress, uint64_t imageOffsetChainStart,
                                        void (^chainedFixupStart)(uint64_t* fixupLoc, ChainedFixupPointerOnDisk fixupInfo, bool& stop));

 	static_assert(sizeof(ResolvedSymbolTarget) == 8, "Overflow in size of SymbolTargetLocation");

    static uint32_t     hashFunction(const char*);


	// only in Image for cached dylibs
    struct PatchableExport
    {
        struct PatchLocation
        {
            uint64_t    cacheOffset             : 32,
                        addend                  : 12,    // +/- 2048
                        authenticated           : 1,
                        usesAddressDiversity    : 1,
                        key                     : 2,
                        discriminator           : 16;

                        PatchLocation(size_t cacheOffset, uint64_t addend);
                        PatchLocation(size_t cacheOffset, uint64_t addend, dyld3::MachOLoaded::ChainedFixupPointerOnDisk authInfo);

            uint64_t getAddend() const {
                uint64_t unsingedAddend = addend;
                int64_t signedAddend = (int64_t)unsingedAddend;
                signedAddend = (signedAddend << 52) >> 52;
                return (uint64_t)signedAddend;
            }
                        
            const char* keyName() const;
            bool operator==(const PatchLocation& other) const {
                return this->cacheOffset == other.cacheOffset;
            }
        };

        uint32_t       cacheOffsetOfImpl;
        uint32_t       patchLocationsCount;
        PatchLocation  patchLocations[];
        // export name
    };
    uint32_t            patchableExportCount() const;
    void                forEachPatchableExport(void (^handler)(uint32_t cacheOffsetOfImpl, const char* exportName)) const;
    void                forEachPatchableUseOfExport(uint32_t cacheOffsetOfImpl, void (^handler)(PatchableExport::PatchLocation patchLocation)) const;

private:
    friend struct Closure;
    friend class ImageWriter;
    friend class ClosureBuilder;
    friend class LaunchClosureWriter;

    uint32_t             pageSize() const;

    struct Flags
    {
        uint64_t        imageNum         : 16,
                        maxLoadCount     : 12,
                        isInvalid        : 1,       // an error occurred creating the info for this image
                        has16KBpages     : 1,
                        is64             : 1,
                        hasObjC          : 1,
                        mayHavePlusLoads : 1,
                        isEncrypted      : 1,       // image is DSMOS or FairPlay encrypted
                        hasWeakDefs      : 1,
                        neverUnload      : 1,
                        cwdSameAsThis    : 1,       // dylibs use file system relative paths, cwd must be main's dir
                        isPlatformBinary : 1,       // part of OS - can be loaded into LV process
                        isBundle         : 1,
                        isDylib          : 1,
                        isExecutable     : 1,
                        overridableDylib : 1,       // only applicable to cached dylibs
                        inDyldCache      : 1,
                        padding          : 21;
    };

    const Flags&        getFlags() const;

    struct PathAndHash
    {
        uint32_t    hash;
        char        path[];
    };

    // In disk based images, all segments are multiples of page size
    // This struct just tracks the size (disk and vm) of each segment.
    // This is compact for most every image which have contiguous segments.
    // If the image does not have contiguous segments (rare), an extra
    // DiskSegment is inserted with the paddingNotSeg bit set.
    struct DiskSegment
    {
        uint64_t    filePageCount   : 30,
                    vmPageCount     : 30,
                    permissions     : 3,
                    paddingNotSeg   : 1;
    };


    // In cache DATA_DIRTY is not page aligned or sized
    // This struct allows segments with any alignment and up to 256MB in size
    struct DyldCacheSegment
    {
        uint64_t    cacheOffset : 32,
                    size        : 28,
                    permissions : 4;
    };

    struct CodeSignatureLocation
    {
        uint32_t     fileOffset;
        uint32_t     fileSize;
    };

    struct FileInfo
    {
        uint64_t     inode;
        uint64_t     modTime;
    };

    struct FairPlayRange
    {
        uint32_t     textPageCount : 28,
                     textStartPage : 4;
    };

    struct MappingInfo
    {
        uint32_t     totalVmPages;
        uint32_t     sliceOffsetIn4K;
    };

    struct LinkedImage {

        LinkedImage() : imgNum(0), linkKind(0) {
        }
        LinkedImage(LinkKind k, ImageNum num) : imgNum(num), linkKind((uint32_t)k) {
            assert((num & 0xC0000000) == 0);
        }

        LinkKind    kind()  const      { return (LinkKind)linkKind; }
        ImageNum    imageNum() const   { return imgNum; }
        void        clearKind()        { linkKind = 0; }

        bool operator==(const LinkedImage& rhs) const {
            return (linkKind == rhs.linkKind) && (imgNum == rhs.imgNum);
        }
        bool operator!=(const LinkedImage& rhs) const {
            return (linkKind != rhs.linkKind) || (imgNum != rhs.imgNum);
        }
	private:
        uint32_t     imgNum         :  30,
                     linkKind       :   2;     // LinkKind
    };

    const Array<LinkedImage> dependentsArray() const;

    struct RebasePattern
    {
        uint32_t    repeatCount   : 20,     
                    contigCount   :  8, // how many contiguous pointers neeed rebasing
                    skipCount     :  4; // how many pointers to skip between contig groups
        // If contigCount == 0, then there are no rebases for this entry,
        // instead it advances the rebase location by repeatCount*skipCount.
        // If all fields are zero, then the rebase position is reset to the start.
        // This is to support old binaries with some non-monotonically-increasing rebases.
    };
    const Array<RebasePattern> rebaseFixups() const;

    struct BindPattern
    {
        Image::ResolvedSymbolTarget     target;
        uint64_t                        startVmOffset : 40, // max 1TB offset
                                        skipCount     :  8, 
                                        repeatCount   : 16;
    };
    const Array<BindPattern> bindFixups() const;

    struct TextFixupPattern
    {
        Image::ResolvedSymbolTarget     target;
        uint32_t                        startVmOffset;
        uint16_t                        repeatCount;
        uint16_t                        skipCount;
    };
    const Array<TextFixupPattern> textFixups() const;

    // for use with chained fixups
    const Array<uint64_t>                     chainedStarts() const;
    const Array<Image::ResolvedSymbolTarget>  chainedTargets() const;

};

/*
     Dyld cache patching notes:

     The dyld cache needs to be patched to support interposing and dylib "roots".

     For cached dylibs overrides:
         Closure build time:
             1) LoadedImages will contain the new dylib, so all symbol look ups
                will naturally find new impl.  Only dyld cache needs special help.
             2) LoadedImages entry will have flag for images that override cache.
             3) When setting Closure attributes, if flag is set, builder will
                iterate PatchableExport entries in Image* from cache and create
                a PatchEntry for each.
         Runtime:
             1) [lib]dyld will iterate PatchEntry in closure and patch cache

     For interposing:
         Closure build time:
             1) After Images built, if __interpose section(s) exist, builder will
                build InterposingTuple entries for closure
             2) For being-built closure and launch closure, apply any InterposingTuple
                to modify Image fixups before Image finalized.
             3) Builder will find PatchableExport entry that matchs stock Impl
                and add PatchEntry to closure for it.
         Runtime:
             1) When closure is loaded (launch or dlopen) PatchEntries are
                applied (exactly once) to whole cache.
             2) For each DlopenClosure loaded, any InterposeTuples in *launch* closure
                are applied to all new images in new DlopenClosure.

     For weak-def coalesing:
          Closure build time:
             1) weak_bind entries are turned into -3 ordinal lookup which search through images
                in load order for first def (like flat). This fixups up all images not in cache.
             2) When processing -3 ordinals, it continues past first found and if any images
                past it are in dyld cache and export that same symbol, a PatchEntry is added to
                closure to fix up all cached uses of that symbol.
             3) If a weak_bind has strong bit set (no fixup, just def), all images from the dyld
                cache are checked to see if the export that symbol, if so, a PatchEntry is added
                to the closure.
          Runtime:
             1) When closure is loaded (launch or dlopen) PatchEntries are
                applied (exactly once) to whole cache.

*/


//
// An array (accessible by index) list of Images
//
struct VIS_HIDDEN ImageArray : public TypedBytes
{
    size_t              size() const;
    size_t              startImageNum() const;
    uint32_t            imageCount() const;
    void                forEachImage(void (^callback)(const Image* image, bool& stop)) const;
    bool                hasPath(const char* path, ImageNum& num) const;
    const Image*        imageForNum(ImageNum) const;

    static const Image* findImage(const Array<const ImageArray*> imagesArrays, ImageNum imageNum);

private:
    friend class ImageArrayWriter;
    
    uint32_t        firstImageNum;
    uint32_t        count;
    uint32_t        offsets[];
    // Image data
};


struct InterposingTuple
{
    Image::ResolvedSymbolTarget    stockImplementation;
    Image::ResolvedSymbolTarget    newImplementation;
};


//
// Describes how dyld should load a set of mach-o files
//
struct VIS_HIDDEN Closure : public ContainerTypedBytes
{
    size_t              size() const;
    const ImageArray*   images() const;
    ImageNum            topImage() const;
    void                deallocate() const;

    friend class ClosureWriter;

    struct PatchEntry
    {
        ImageNum                    overriddenDylibInCache;
        uint32_t                    exportCacheOffset;
        Image::ResolvedSymbolTarget replacement;
    };
    void                forEachPatchEntry(void (^handler)(const PatchEntry& entry)) const;
};


//
// Describes how dyld should launch a main executable
//
struct VIS_HIDDEN LaunchClosure : public Closure
{
    bool                builtAgainstDyldCache(uuid_t cacheUUID) const;
    const char*         bootUUID() const;
    void                forEachMustBeMissingFile(void (^handler)(const char* path, bool& stop)) const;
    void                forEachEnvVar(void (^handler)(const char* keyEqualValue, bool& stop)) const;
    ImageNum            libSystemImageNum() const;
    void                libDyldEntry(Image::ResolvedSymbolTarget& loc) const;
    bool                mainEntry(Image::ResolvedSymbolTarget& mainLoc) const;
    bool                startEntry(Image::ResolvedSymbolTarget& startLoc) const;
    uint32_t            initialLoadCount() const;
    void                forEachInterposingTuple(void (^handler)(const InterposingTuple& tuple, bool& stop)) const;
    bool                usedAtPaths() const;
    bool                usedFallbackPaths() const;


private:
    friend class LaunchClosureWriter;

    struct Flags
    {
        uint32_t        usedAtPaths              :  1,
                        usedFallbackPaths        :  1,
                        initImageCount           : 16,
                        padding                  : 14;
    };
    const Flags&        getFlags() const;
};



//
// Describes how dyld should dlopen() a mach-o file
//
struct VIS_HIDDEN DlopenClosure : public Closure
{

};



} //  namespace closure
} //  namespace dyld3


#endif // Closures_h


