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


#include <stdint.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <limits.h>

#include "Closure.h"
#include "MachOFile.h"
#include "MachOLoaded.h"


namespace dyld {
    extern void log(const char* format, ...)  __attribute__((format(printf, 1, 2)));
}

namespace dyld3 {
namespace closure {


////////////////////////////  TypedBytes ////////////////////////////////////////

const void* TypedBytes::payload() const
{
    return (uint8_t*)this + sizeof(TypedBytes);
}

void* TypedBytes::payload()
{
    return (uint8_t*)this + sizeof(TypedBytes);
}


////////////////////////////  ContainerTypedBytes ////////////////////////////////////////

const TypedBytes* ContainerTypedBytes::first() const
{
    return (TypedBytes*)payload();
}

const TypedBytes* ContainerTypedBytes::next(const TypedBytes* p) const
{
    assert((p->payloadLength & 0x3) == 0);
    return (TypedBytes*)((uint8_t*)(p->payload()) + p->payloadLength);
}

void ContainerTypedBytes::forEachAttribute(void (^handler)(const TypedBytes* typedBytes, bool& stop)) const
{
    assert(((long)this & 0x3) == 0);
    const TypedBytes* end = next(this);
    bool stop = false;
    for (const TypedBytes* p = first(); p < end && !stop; p = next(p)) {
        handler(p, stop);
    }
}

void ContainerTypedBytes::forEachAttributePayload(Type requestedType, void (^handler)(const void* payload, uint32_t size, bool& stop)) const
{
    forEachAttribute(^(const TypedBytes* typedBytes, bool& stop) {
        if ( (Type)(typedBytes->type) != requestedType )
            return;
        handler(typedBytes->payload(), typedBytes->payloadLength, stop);
    });
}

const void* ContainerTypedBytes::findAttributePayload(Type requestedType, uint32_t* payloadSize) const
{
    assert(((long)this & 0x3) == 0);
    if ( payloadSize != nullptr )
        *payloadSize = 0;
    const TypedBytes* end = next(this);
    bool stop = false;
    for (const TypedBytes* p = first(); p < end && !stop; p = next(p)) {
        if ( (Type)(p->type) == requestedType ) {
            if ( payloadSize != nullptr )
                *payloadSize = p->payloadLength;
            return p->payload();
        }
    }
    return nullptr;
}


////////////////////////////  Image ////////////////////////////////////////

const Image::Flags& Image::getFlags() const
{
    return *(Flags*)((uint8_t*)this + 2*sizeof(TypedBytes));
}

bool Image::isInvalid() const
{
    return getFlags().isInvalid;
}

size_t Image::size() const
{
    return sizeof(TypedBytes) + this->payloadLength;
}

ImageNum Image::imageNum() const
{
    return getFlags().imageNum;
}

// returns true iff 'num' is this image's ImageNum, or this image overrides that imageNum (in dyld cache)
bool Image::representsImageNum(ImageNum num) const
{
    const Flags& flags = getFlags();
    if ( flags.imageNum == num )
        return true;
    if ( !flags.isDylib )
        return false;
    if ( flags.inDyldCache )
        return false;
    ImageNum cacheImageNum;
    if ( isOverrideOfDyldCacheImage(cacheImageNum) )
        return (cacheImageNum == num);
    return false;
}

uint32_t Image::maxLoadCount() const
{
    return getFlags().maxLoadCount;
}

bool Image::isBundle() const
{
    return getFlags().isBundle;
}

bool Image::isDylib() const
{
    return getFlags().isDylib;
}

bool Image::isExecutable() const
{
    return getFlags().isExecutable;
}

bool Image::hasObjC() const
{
    return getFlags().hasObjC;
}

bool Image::is64() const
{
    return getFlags().is64;
}

bool Image::hasWeakDefs() const
{
    return getFlags().hasWeakDefs;
}

bool Image::mayHavePlusLoads() const
{
    return getFlags().mayHavePlusLoads;
}

bool Image::neverUnload() const
{
    return getFlags().neverUnload;
}

bool Image::overridableDylib() const
{
    return getFlags().overridableDylib;
}

bool Image::inDyldCache() const
{
    return getFlags().inDyldCache;
}

const char* Image::path() const
{
    // might be multiple pathWithHash enties, first is canonical name
    const PathAndHash* result = (PathAndHash*)findAttributePayload(Type::pathWithHash);
    assert(result && "Image missing pathWithHash");
    return result->path;
}

const char* Image::leafName() const
{
    uint32_t size;
    // might be multiple pathWithHash enties, first is canonical name
    const PathAndHash* result = (PathAndHash*)findAttributePayload(Type::pathWithHash, &size);
    assert(result && "Image missing pathWithHash");
    for (const char* p=(char*)result + size; p > result->path; --p) {
        if ( *p == '/' )
            return p+1;
    }
    return result->path;
}

bool Image::hasFileModTimeAndInode(uint64_t& inode, uint64_t& mTime) const
{
    uint32_t size;
    const FileInfo* info = (FileInfo*)(findAttributePayload(Type::fileInodeAndTime, &size));
    if ( info != nullptr ) {
        assert(size == sizeof(FileInfo));
        inode = info->inode;
        mTime = info->modTime;
        return true;
    }
    return false;
}

bool Image::hasCdHash(uint8_t cdHash[20]) const
{
    uint32_t size;
    const uint8_t* bytes = (uint8_t*)(findAttributePayload(Type::cdHash, &size));
    if ( bytes != nullptr ) {
        assert(size == 20);
        memcpy(cdHash, bytes, 20);
        return true;
    }
    return false;
}

bool Image::getUuid(uuid_t uuid) const
{
    uint32_t size;
    const uint8_t* bytes = (uint8_t*)(findAttributePayload(Type::uuid, &size));
    if ( bytes == nullptr )
        return false;
    assert(size == 16);
    memcpy(uuid, bytes, 16);
    return true;
}

bool Image::hasCodeSignature(uint32_t& sigFileOffset, uint32_t& sigSize) const
{
    uint32_t sz;
    const Image::CodeSignatureLocation* sigInfo = (Image::CodeSignatureLocation*)(findAttributePayload(Type::codeSignLoc, &sz));
    if ( sigInfo != nullptr ) {
        assert(sz == sizeof(Image::CodeSignatureLocation));
        sigFileOffset = sigInfo->fileOffset;
        sigSize       = sigInfo->fileSize;
        return true;
    }
    return false;
}

bool Image::isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const
{
    uint32_t sz;
    const Image::FairPlayRange* fpInfo = (Image::FairPlayRange*)(findAttributePayload(Type::fairPlayLoc, &sz));
    if ( fpInfo != nullptr ) {
        assert(sz == sizeof(Image::FairPlayRange));
        textOffset = fpInfo->textStartPage * pageSize();
        size       = fpInfo->textPageCount * pageSize();
        return true;
    }
    return false;
}

const Array<Image::LinkedImage> Image::dependentsArray() const
{
    uint32_t size;
    LinkedImage* dependents = (LinkedImage*)findAttributePayload(Type::dependents, &size);
    assert((size % sizeof(LinkedImage)) == 0);
    uintptr_t count = size / sizeof(LinkedImage);
    return Array<Image::LinkedImage>(dependents, count, count);
}

void Image::forEachDependentImage(void (^handler)(uint32_t dependentIndex, LinkKind kind, ImageNum imageNum, bool& stop)) const
{
    uint32_t size;
    const LinkedImage* dependents = (LinkedImage*)findAttributePayload(Type::dependents, &size);
    assert((size % sizeof(LinkedImage)) == 0);
    const uint32_t count = size / sizeof(LinkedImage);
    bool stop = false;
    for (uint32_t i=0; (i < count) && !stop; ++i) {
        LinkKind kind     = dependents[i].kind();
        ImageNum imageNum = dependents[i].imageNum();
        // ignore missing weak links
        if ( (imageNum == kMissingWeakLinkedImage) && (kind == LinkKind::weak) )
            continue;
        handler(i, kind, imageNum, stop);
    }
}

ImageNum Image::dependentImageNum(uint32_t depIndex) const
{
    uint32_t size;
    const LinkedImage* dependents = (LinkedImage*)findAttributePayload(Type::dependents, &size);
    assert((size % sizeof(LinkedImage)) == 0);
    const uint32_t count = size / sizeof(LinkedImage);
    assert(depIndex < count);
    return dependents[depIndex].imageNum();
}


uint32_t Image::hashFunction(const char* str)
{
    uint32_t h = 0;
    for (const char* s=str; *s != '\0'; ++s)
        h = h*5 + *s;
    return h;
}

void Image::forEachAlias(void (^handler)(const char* aliasPath, bool& stop)) const
{
    __block bool foundFirst = false;
    forEachAttribute(^(const TypedBytes* typedBytes, bool& stopLoop) {
        if ( (Type)(typedBytes->type) != Type::pathWithHash )
            return;
        if ( foundFirst ) {
            const PathAndHash* aliasInfo = (PathAndHash*)typedBytes->payload();
            handler(aliasInfo->path, stopLoop);
        }
        else {
            foundFirst = true;
        }
    });
}

bool Image::hasPathWithHash(const char* path, uint32_t hash) const
{
    __block bool found = false;
    forEachAttribute(^(const TypedBytes* typedBytes, bool& stop) {
        if ( (Type)(typedBytes->type) != Type::pathWithHash )
            return;
        const PathAndHash* pathInfo = (PathAndHash*)typedBytes->payload();
        if ( (pathInfo->hash == hash) && (strcmp(path, pathInfo->path) == 0) ) {
            stop = true;
            found = true;
        }
    });
    return found;
}

void Image::forEachDiskSegment(void (^handler)(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop)) const
{
    uint32_t size;
    const DiskSegment* segments = (DiskSegment*)findAttributePayload(Type::diskSegment, &size);
    assert(segments != nullptr);
    assert((size % sizeof(DiskSegment)) == 0);
    const uint32_t  count        = size / sizeof(DiskSegment);
    const uint32_t  pageSz       = pageSize();
    uint32_t        segIndex     = 0;
    uint32_t        fileOffset   = 0;
    int64_t         vmOffset     = 0;
    // decrement vmOffset by all segments before TEXT (e.g. PAGEZERO)
    for (uint32_t i=0; i < count; ++i) {
        const DiskSegment* seg = &segments[i];
        if ( seg->filePageCount != 0 ) {
            break;
        }
        vmOffset -= (uint64_t)seg->vmPageCount * pageSz;
    }
    // walk each segment and call handler
    bool stop = false;
    for (uint32_t i=0; i < count && !stop; ++i) {
        const DiskSegment* seg = &segments[i];
        uint64_t vmSize   = (uint64_t)seg->vmPageCount * pageSz;
        uint32_t fileSize = seg->filePageCount * pageSz;
        if ( !seg->paddingNotSeg ) {
            handler(segIndex, ( fileSize == 0) ? 0 : fileOffset, fileSize, vmOffset, vmSize, seg->permissions, stop);
            ++segIndex;
        }
        vmOffset   += vmSize;
        fileOffset += fileSize;
    }
}

uint32_t Image::pageSize() const
{
    if ( getFlags().has16KBpages )
        return 0x4000;
    else
        return 0x1000;
}

uint32_t Image::cacheOffset() const
{
    uint32_t size;
    const DyldCacheSegment* segments = (DyldCacheSegment*)findAttributePayload(Type::cacheSegment, &size);
    assert(segments != nullptr);
    assert((size % sizeof(DyldCacheSegment)) == 0);
    return segments[0].cacheOffset;
}

void Image::forEachCacheSegment(void (^handler)(uint32_t segIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop)) const
{
    uint32_t size;
    const DyldCacheSegment* segments = (DyldCacheSegment*)findAttributePayload(Type::cacheSegment, &size);
    assert(segments != nullptr);
    assert((size % sizeof(DyldCacheSegment)) == 0);
    const uint32_t  count = size / sizeof(DyldCacheSegment);
    bool stop = false;
    for (uint32_t i=0; i < count; ++i) {
        uint64_t vmOffset    = segments[i].cacheOffset - segments[0].cacheOffset;
        uint64_t vmSize      = segments[i].size;
        uint8_t  permissions = segments[i].permissions;
        handler(i, vmOffset, vmSize, permissions, stop);
        if ( stop )
            break;
    }
}

uint64_t Image::textSize() const
{
    __block uint64_t result = 0;
    if ( inDyldCache() ) {
        forEachCacheSegment(^(uint32_t segIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
            result = vmSize;
            stop = true;
        });
    }
    else {
        forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
            if ( permissions != 0) {
                result = vmSize;
                stop = true;
            }
        });
    }
    return result;
}

bool Image::containsAddress(const void* addr, const void* imageLoadAddress, uint8_t* permsResult) const
{
    __block bool  result     = false;
    uint64_t      targetAddr = (uint64_t)addr;
    uint64_t      imageStart = (uint64_t)imageLoadAddress;
    if ( inDyldCache() ) {
        forEachCacheSegment(^(uint32_t segIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
            if ( (targetAddr >= imageStart+vmOffset) && (targetAddr < imageStart+vmOffset+vmSize) ) {
                result = true;
                if ( permsResult )
                    *permsResult = permissions;
                stop = true;
            }
        });
    }
    else {
        forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
            if ( (targetAddr >= imageStart+vmOffset) && (targetAddr < imageStart+vmOffset+vmSize) ) {
                result = true;
                if ( permsResult )
                    *permsResult = permissions;
                stop = true;
            }
        });
    }
    return result;
}

uint64_t Image::vmSizeToMap() const
{
    uint32_t size;
    const Image::MappingInfo* info = (Image::MappingInfo*)(findAttributePayload(Type::mappingInfo, &size));
    assert(info != nullptr);
    assert(size == sizeof(Image::MappingInfo));
    return info->totalVmPages * pageSize();
}

uint64_t Image::sliceOffsetInFile() const
{
    uint32_t size;
    const Image::MappingInfo* info = (Image::MappingInfo*)(findAttributePayload(Type::mappingInfo, &size));
    assert(info != nullptr);
    assert(size == sizeof(Image::MappingInfo));
    return info->sliceOffsetIn4K * 0x1000;
}

void Image::forEachInitializer(const void* imageLoadAddress, void (^handler)(const void* initializer)) const
{
    uint32_t size;
    const uint32_t* inits = (uint32_t*)findAttributePayload(Type::initOffsets, &size);
    if ( inits != nullptr ) {
        assert((size % sizeof(uint32_t)) == 0);
        const uint32_t count = size / sizeof(uint32_t);
        for (uint32_t i=0; i < count; ++i) {
            uint32_t offset = inits[i];
            const void* init = (void*)((uint8_t*)imageLoadAddress + offset);
            handler(init);
        }
    }
}

bool Image::hasInitializers() const
{
    uint32_t size;
    return ( findAttributePayload(Type::initOffsets, &size) != nullptr );
}

void Image::forEachDOF(const void* imageLoadAddress, void (^handler)(const void* dofSection)) const
{
    uint32_t size;
    const uint32_t* dofs = (uint32_t*)findAttributePayload(Type::dofOffsets, &size);
    if ( dofs != nullptr ) {
        assert((size % sizeof(uint32_t)) == 0);
        const uint32_t count = size / sizeof(uint32_t);
        for (uint32_t i=0; i < count; ++i) {
            uint32_t offset = dofs[i];
            const void* sect = (void*)((uint8_t*)imageLoadAddress + offset);
            handler(sect);
        }
    }
}

void Image::forEachPatchableExport(void (^handler)(uint32_t cacheOffsetOfImpl, const char* exportName)) const
{
    forEachAttributePayload(Type::cachePatchInfo, ^(const void* payload, uint32_t size, bool& stop) {
        const Image::PatchableExport* pe = (Image::PatchableExport*)payload;
        assert(size > (sizeof(Image::PatchableExport) + pe->patchLocationsCount*sizeof(PatchableExport::PatchLocation)));
        handler(pe->cacheOffsetOfImpl, (char*)(&pe->patchLocations[pe->patchLocationsCount]));
    });
}

void Image::forEachPatchableUseOfExport(uint32_t cacheOffsetOfImpl, void (^handler)(PatchableExport::PatchLocation patchLocation)) const
{
    forEachAttributePayload(Type::cachePatchInfo, ^(const void* payload, uint32_t size, bool& stop) {
        const Image::PatchableExport* pe = (Image::PatchableExport*)payload;
        assert(size > (sizeof(Image::PatchableExport) + pe->patchLocationsCount*sizeof(PatchableExport::PatchLocation)));
        if ( pe->cacheOffsetOfImpl != cacheOffsetOfImpl )
            return;
        const PatchableExport::PatchLocation* start = pe->patchLocations;
        const PatchableExport::PatchLocation* end   = &start[pe->patchLocationsCount];
        for (const PatchableExport::PatchLocation* p=start; p < end; ++p)
            handler(*p);
    });
}

uint32_t Image::patchableExportCount() const
{
    __block uint32_t count = 0;
    forEachAttributePayload(Type::cachePatchInfo, ^(const void* payload, uint32_t size, bool& stop) {
        ++count;
    });
    return count;
}

void Image::forEachFixup(void (^rebase)(uint64_t imageOffsetToRebase, bool& stop),
                         void (^bind)(uint64_t imageOffsetToBind, ResolvedSymbolTarget bindTarget, bool& stop),
                         void (^chainedFixupsStart)(uint64_t imageOffsetStart, const Array<ResolvedSymbolTarget>& targets, bool& stop)) const
{
    const uint32_t pointerSize = is64() ? 8 : 4;
	uint64_t curRebaseOffset = 0;
	bool stop = false;
    for (const Image::RebasePattern& rebasePat : rebaseFixups()) {
        //fprintf(stderr, " repeat=0x%04X, contig=%d, skip=%d\n", rebasePat.repeatCount, rebasePat.contigCount, rebasePat.skipCount);
        if ( rebasePat.contigCount == 0 ) {
            // note: contigCount==0 means this just advances location
            if ( (rebasePat.repeatCount == 0) && (rebasePat.skipCount == 0) ) {
                // all zeros is special pattern that means reset to rebase offset to zero
                curRebaseOffset = 0;
            }
            else {
                curRebaseOffset += rebasePat.repeatCount * rebasePat.skipCount;
            }
        }
        else {
            for (int r=0; r < rebasePat.repeatCount && !stop; ++r) {
                for (int i=0; i < rebasePat.contigCount && !stop; ++i) {
                    //fprintf(stderr, "  0x%08llX\n", curRebaseOffset);
                    rebase(curRebaseOffset, stop);
                    curRebaseOffset += pointerSize;
                }
                curRebaseOffset += pointerSize * rebasePat.skipCount;
            }
        }
        if ( stop )
            break;
    }
    if ( stop )
        return;

    for (const Image::BindPattern& bindPat : bindFixups()) {
        uint64_t curBindOffset = bindPat.startVmOffset;
        for (uint16_t i=0; i < bindPat.repeatCount; ++i) {
            bind(curBindOffset, bindPat.target, stop);
            curBindOffset += (pointerSize * (1 + bindPat.skipCount));
            if ( stop )
                break;
        }
        if ( stop )
            break;
    }

    const Array<Image::ResolvedSymbolTarget> targetsArray = chainedTargets();
    for (uint64_t start : chainedStarts()) {
        chainedFixupsStart(start, targetsArray, stop);
        if ( stop )
            break;
    }
}

void Image::forEachChainedFixup(void* imageLoadAddress, uint64_t imageOffsetChainStart, void (^callback)(uint64_t* fixUpLoc, ChainedFixupPointerOnDisk fixupInfo, bool& stop))
{
    bool stop = false;
    uint64_t* fixupLoc = (uint64_t*)((uint8_t*)imageLoadAddress + imageOffsetChainStart);
    do {
        // save off current entry as it will be overwritten in callback
        ChainedFixupPointerOnDisk info = *((ChainedFixupPointerOnDisk*)fixupLoc);
        callback(fixupLoc, info, stop);
        if ( info.plainRebase.next != 0 )
            fixupLoc += info.plainRebase.next;
        else
            stop = true;
    } while (!stop);
}

void Image::forEachTextReloc(void (^rebase)(uint32_t imageOffsetToRebase, bool& stop),
                             void (^bind)(uint32_t imageOffsetToBind, ResolvedSymbolTarget bindTarget, bool& stop)) const
{
    bool stop = false;
    const Array<Image::TextFixupPattern> f = textFixups();
    for (const Image::TextFixupPattern& pat : f) {
        uint32_t curOffset = pat.startVmOffset;
        for (uint16_t i=0; i < pat.repeatCount; ++i) {
            if ( pat.target.raw == 0 )
                rebase(curOffset, stop);
            else
                bind(curOffset, pat.target, stop);
            curOffset += pat.skipCount;
        }
    }
}

const Array<Image::RebasePattern> Image::rebaseFixups() const
{
    uint32_t rebaseFixupsSize;
    Image::RebasePattern* rebaseFixupsContent = (RebasePattern*)findAttributePayload(Type::rebaseFixups, &rebaseFixupsSize);
    uint32_t rebaseCount = rebaseFixupsSize/sizeof(RebasePattern);
    return Array<RebasePattern>(rebaseFixupsContent, rebaseCount, rebaseCount);
}

const Array<Image::BindPattern> Image::bindFixups() const
{
    uint32_t bindFixupsSize;
    BindPattern* bindFixupsContent = (BindPattern*)findAttributePayload(Type::bindFixups, &bindFixupsSize);
    uint32_t bindCount = bindFixupsSize/sizeof(BindPattern);
    return Array<BindPattern>(bindFixupsContent, bindCount, bindCount);
}

const Array<uint64_t> Image::chainedStarts() const
{
    uint32_t startsSize;
    uint64_t* starts = (uint64_t*)findAttributePayload(Type::chainedFixupsStarts, &startsSize);
    uint32_t count = startsSize/sizeof(uint64_t);
    return Array<uint64_t>(starts, count, count);
}

const Array<Image::ResolvedSymbolTarget> Image::chainedTargets() const
{
    uint32_t size;
    ResolvedSymbolTarget* targetsContent = (ResolvedSymbolTarget*)findAttributePayload(Type::chainedFixupsTargets, &size);
    uint32_t count = size/sizeof(ResolvedSymbolTarget);
    return Array<ResolvedSymbolTarget>(targetsContent, count, count);
}

const Array<Image::TextFixupPattern> Image::textFixups() const
{
    uint32_t fixupsSize;
    TextFixupPattern* fixupsContent = (TextFixupPattern*)findAttributePayload(Type::textFixups, &fixupsSize);
    uint32_t count = fixupsSize/sizeof(TextFixupPattern);
    return Array<TextFixupPattern>(fixupsContent, count, count);
}

bool Image::isOverrideOfDyldCacheImage(ImageNum& imageNum) const
{
	uint32_t size;
	const uint32_t* content = (uint32_t*)findAttributePayload(Type::imageOverride, &size);
	if ( content != nullptr ) {
        assert(size == sizeof(uint32_t));
        imageNum = *content;
        return true;
    }
    return false;
}

void Image::forEachImageToInitBefore(void (^handler)(ImageNum imageToInit, bool& stop)) const
{
    uint32_t size;
    const ImageNum* initBefores = (ImageNum*)findAttributePayload(Type::initBefores, &size);
    if ( initBefores != nullptr ) {
        assert((size % sizeof(ImageNum)) == 0);
        const uint32_t count = size / sizeof(ImageNum);
        bool stop = false;
        for (uint32_t i=0; (i < count) && !stop; ++i) {
            handler(initBefores[i], stop);
        }
    }
}

const char* Image::PatchableExport::PatchLocation::keyName() const
{
    return MachOLoaded::ChainedFixupPointerOnDisk::keyName(this->key);
}

Image::PatchableExport::PatchLocation::PatchLocation(size_t cacheOff, uint64_t ad)
    : cacheOffset(cacheOff), addend(ad), authenticated(0), usesAddressDiversity(0), key(0), discriminator(0)
{
    int64_t signedAddend = (int64_t)ad;
    assert(((signedAddend << 52) >> 52) == signedAddend);
}

Image::PatchableExport::PatchLocation::PatchLocation(size_t cacheOff, uint64_t ad, dyld3::MachOLoaded::ChainedFixupPointerOnDisk authInfo)
    : cacheOffset(cacheOff), addend(ad), authenticated(authInfo.authBind.auth), usesAddressDiversity(authInfo.authBind.addrDiv), key(authInfo.authBind.key), discriminator(authInfo.authBind.diversity)
{
    int64_t signedAddend = (int64_t)ad;
    assert(((signedAddend << 52) >> 52) == signedAddend);
}

////////////////////////////  ImageArray ////////////////////////////////////////

size_t ImageArray::size() const
{
    return sizeof(TypedBytes) + this->payloadLength;
}

size_t ImageArray::startImageNum() const
{
    return firstImageNum;
}

uint32_t ImageArray::imageCount() const
{
    return count;
}

void ImageArray::forEachImage(void (^callback)(const Image* image, bool& stop)) const
{
    bool stop = false;
    for (uint32_t i=0; i < count && !stop; ++i) {
        const Image* image = (Image*)((uint8_t*)payload() + offsets[i]);
        callback(image, stop);
        if (stop)
            break;
    }
}

bool ImageArray::hasPath(const char* path, ImageNum& num) const
{
    const uint32_t hash = Image::hashFunction(path);
    __block bool found = false;
    forEachImage(^(const Image* image, bool& stop) {
        if ( image->hasPathWithHash(path, hash) ) {
            num   = image->imageNum();
            found = true;
            stop  = true;
        }
    });
    return found;
}

const Image* ImageArray::imageForNum(ImageNum num) const
{
    if ( num < firstImageNum )
        return nullptr;

    uint32_t index = num - firstImageNum;
    if ( index >= count )
        return nullptr;

    return (Image*)((uint8_t*)payload() + offsets[index]);
}

const Image* ImageArray::findImage(const Array<const ImageArray*> imagesArrays, ImageNum imageNum)
{
   for (const ImageArray* ia : imagesArrays) {
        if ( const Image* result = ia->imageForNum(imageNum) )
            return result;
    }
    return nullptr;
}

////////////////////////////  Closure ////////////////////////////////////////

size_t Closure::size() const
{
    return sizeof(TypedBytes) + this->payloadLength;
}

const ImageArray* Closure::images() const
{
    __block const TypedBytes* result = nullptr;
    forEachAttribute(^(const TypedBytes* typedBytes, bool& stop) {
        if ( (Type)(typedBytes->type) == Type::imageArray ) {
            result = typedBytes;
            stop = true;
        }
    });

    return (ImageArray*)result;
}

ImageNum Closure::topImage() const
{
    uint32_t size;
    const ImageNum* top = (ImageNum*)findAttributePayload(Type::topImage, &size);
    assert(top != nullptr);
    assert(size == sizeof(ImageNum));
    return *top;
}

void Closure::forEachPatchEntry(void (^handler)(const PatchEntry& entry)) const
{
	forEachAttributePayload(Type::cacheOverrides, ^(const void* payload, uint32_t size, bool& stop) {
        assert((size % sizeof(Closure::PatchEntry)) == 0);
        const PatchEntry* patches    = (PatchEntry*)payload;
        const PatchEntry* patchesEnd = (PatchEntry*)((char*)payload + size);
        for (const PatchEntry* p=patches; p < patchesEnd; ++p)
            handler(*p);
	});
}

void Closure::deallocate() const
{
    ::vm_deallocate(mach_task_self(), (long)this, size());
}

////////////////////////////  LaunchClosure ////////////////////////////////////////

void LaunchClosure::forEachMustBeMissingFile(void (^handler)(const char* path, bool& stop)) const
{
    uint32_t size;
    const char* paths = (const char*)findAttributePayload(Type::missingFiles, &size);
    bool stop = false;
    for (const char* s=paths; s < &paths[size]; ++s) {
        if ( *s != '\0' )
            handler(s, stop);
        if ( stop )
            break;
        s += strlen(s);
    }
}

bool LaunchClosure::builtAgainstDyldCache(uuid_t cacheUUID) const
{
    uint32_t size;
    const uint8_t* uuidBytes = (uint8_t*)findAttributePayload(Type::dyldCacheUUID, &size);
    if ( uuidBytes == nullptr )
        return false;
    assert(size == sizeof(uuid_t));
    memcpy(cacheUUID, uuidBytes, sizeof(uuid_t));
    return true;
}

const char* LaunchClosure::bootUUID() const
{
    uint32_t size;
    return (char*)findAttributePayload(Type::bootUUID, &size);
}

void LaunchClosure::forEachEnvVar(void (^handler)(const char* keyEqualValue, bool& stop)) const
{
    forEachAttributePayload(Type::envVar, ^(const void* payload, uint32_t size, bool& stop) {
        handler((char*)payload, stop);
    });
}

ImageNum LaunchClosure::libSystemImageNum() const
{
    uint32_t size;
    const ImageNum* num = (ImageNum*)findAttributePayload(Type::libSystemNum, &size);
    assert(num != nullptr);
    assert(size == sizeof(ImageNum));
    return *num;
}

void LaunchClosure::libDyldEntry(Image::ResolvedSymbolTarget& loc) const
{
    uint32_t size;
    const Image::ResolvedSymbolTarget* data = (Image::ResolvedSymbolTarget*)findAttributePayload(Type::libDyldEntry, &size);
    assert(data != nullptr);
    assert(size == sizeof(Image::ResolvedSymbolTarget));
    loc = *data;
}

bool LaunchClosure::mainEntry(Image::ResolvedSymbolTarget& mainLoc) const
{
    uint32_t size;
    const Image::ResolvedSymbolTarget* data = (Image::ResolvedSymbolTarget*)findAttributePayload(Type::mainEntry, &size);
    if ( data == nullptr )
        return false;
    assert(size == sizeof(Image::ResolvedSymbolTarget));
    mainLoc = *data;
    return true;
}

bool LaunchClosure::startEntry(Image::ResolvedSymbolTarget& startLoc) const
{
    uint32_t size;
    const Image::ResolvedSymbolTarget* data = (Image::ResolvedSymbolTarget*)findAttributePayload(Type::startEntry, &size);
    if ( data == nullptr )
        return false;
    assert(size == sizeof(Image::ResolvedSymbolTarget));
    startLoc = *data;
    return true;
}

const LaunchClosure::Flags& LaunchClosure::getFlags() const
{
    uint32_t size;
    const Flags* flags = (Flags*)findAttributePayload(Type::closureFlags, &size);
    assert(flags != nullptr && "Closure missing Flags");
    return *flags;
}

uint32_t LaunchClosure::initialLoadCount() const
{
    return getFlags().initImageCount;
}

bool LaunchClosure::usedAtPaths() const
{
    return getFlags().usedAtPaths;
}

bool LaunchClosure::usedFallbackPaths() const
{
	return getFlags().usedFallbackPaths;
}

void LaunchClosure::forEachInterposingTuple(void (^handler)(const InterposingTuple& tuple, bool& stop)) const
{
	forEachAttributePayload(Type::interposeTuples, ^(const void* payload, uint32_t size, bool& stop) {
        assert((size % sizeof(InterposingTuple)) == 0);
        uintptr_t count = size / sizeof(InterposingTuple);
        const InterposingTuple* tuples = (InterposingTuple*)payload;
        for (uint32_t i=0; i < count && !stop; ++i) {
            handler(tuples[i], stop);
        }
	});
}



} // namespace closure
} // namespace dyld3



