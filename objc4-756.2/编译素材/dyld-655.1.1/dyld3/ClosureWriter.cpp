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
#include <mach/vm_page_size.h>

#include "ClosureWriter.h"
#include "MachOFile.h"


namespace dyld3 {
namespace closure {


////////////////////////////  ContainerTypedBytesWriter ////////////////////////////////////////


void ContainerTypedBytesWriter::setContainerType(TypedBytes::Type containerType)
{
    assert(_vmAllocationStart == 0);
    _vmAllocationSize = 1024 * 1024;
    vm_address_t allocationAddr;
    ::vm_allocate(mach_task_self(), &allocationAddr, _vmAllocationSize, VM_FLAGS_ANYWHERE);
    assert(allocationAddr != 0);
    _vmAllocationStart = (void*)allocationAddr;
    _containerTypedBytes =  (TypedBytes*)_vmAllocationStart;
    _containerTypedBytes->type = (uint32_t)containerType;
    _containerTypedBytes->payloadLength = 0;
    _end = (uint8_t*)_vmAllocationStart + sizeof(TypedBytes);
}

void* ContainerTypedBytesWriter::append(TypedBytes::Type t, const void* payload, uint32_t payloadSize)
{
    assert((payloadSize & 0x3) == 0);
    if ( (uint8_t*)_end + payloadSize >= (uint8_t*)_vmAllocationStart + _vmAllocationSize ) {
        // if current buffer too small, grow it
        size_t growth = _vmAllocationSize;
        if ( growth < payloadSize )
            growth = _vmAllocationSize*((payloadSize/_vmAllocationSize)+1);
        vm_address_t newAllocationAddr;
        size_t newAllocationSize = _vmAllocationSize+growth;
        ::vm_allocate(mach_task_self(), &newAllocationAddr, newAllocationSize, VM_FLAGS_ANYWHERE);
	    assert(newAllocationAddr != 0);
        size_t currentInUse = (char*)_end - (char*)_vmAllocationStart;
        memcpy((void*)newAllocationAddr, _vmAllocationStart, currentInUse);
        ::vm_deallocate(mach_task_self(), (vm_address_t)_vmAllocationStart, _vmAllocationSize);
        _end                 = (void*)(newAllocationAddr + currentInUse);
        _vmAllocationStart   = (void*)newAllocationAddr;
        _containerTypedBytes = (TypedBytes*)_vmAllocationStart;
        _vmAllocationSize    = newAllocationSize;
    }
    assert( (uint8_t*)_end + payloadSize < (uint8_t*)_vmAllocationStart + _vmAllocationSize);
    TypedBytes* tb = (TypedBytes*)_end;
    tb->type   = (uint32_t)t;
    tb->payloadLength = payloadSize;
    if ( payload != nullptr )
        ::memcpy(tb->payload(), payload, payloadSize);
    _end = (uint8_t*)_end + sizeof(TypedBytes) + payloadSize;
    _containerTypedBytes->payloadLength += sizeof(TypedBytes) + payloadSize;
    return tb->payload();
}

const void* ContainerTypedBytesWriter::finalizeContainer()
{
    // trim vm allocation down to just what is needed
    uintptr_t bufferStart = (uintptr_t)_vmAllocationStart;
    uintptr_t used = round_page((uintptr_t)_end - bufferStart);
    if ( used < _vmAllocationSize ) {
        uintptr_t deallocStart = bufferStart + used;
        ::vm_deallocate(mach_task_self(), deallocStart, _vmAllocationSize - used);
        _end = nullptr;
        _vmAllocationSize = used;
    }
    // mark vm region read-only
    ::vm_protect(mach_task_self(), bufferStart, used, false, VM_PROT_READ);
    return (void*)_vmAllocationStart;
}

const void* ContainerTypedBytesWriter::currentTypedBytes()
{
    return (void*)_vmAllocationStart;
}

void ContainerTypedBytesWriter::deallocate()
{
    ::vm_deallocate(mach_task_self(), (long)_vmAllocationStart, _vmAllocationSize);
}

////////////////////////////  ImageWriter ////////////////////////////////////////


const Image* ImageWriter::finalize()
{
    return (Image*)finalizeContainer();
}

const Image* ImageWriter::currentImage()
{
    return (Image*)currentTypedBytes();
}

void ImageWriter::addPath(const char* path)
{
    uint32_t roundedPathLen = ((uint32_t)strlen(path) + 1 + 3) & (-4);
    Image::PathAndHash* ph = (Image::PathAndHash*)append(TypedBytes::Type::pathWithHash, nullptr, sizeof(Image::PathAndHash)+roundedPathLen);
    ph->hash = Image::hashFunction(path);
    strcpy(ph->path, path);
}

Image::Flags& ImageWriter::getFlags()
{
    if ( _flagsOffset == -1 ) {
        setContainerType(TypedBytes::Type::image);
        Image::Flags flags;
        ::bzero(&flags, sizeof(flags));
        uint8_t* p = (uint8_t*)append(TypedBytes::Type::imageFlags, &flags, sizeof(flags));
        _flagsOffset = (int)(p - (uint8_t*)currentTypedBytes());
    }
    return *((Image::Flags*)((uint8_t*)currentTypedBytes() + _flagsOffset));
}

void ImageWriter::setImageNum(ImageNum num)
{
   getFlags().imageNum = num;
}

void ImageWriter::setHasObjC(bool value)
{
    getFlags().hasObjC = value;
}

void ImageWriter::setIs64(bool value)
{
    getFlags().is64 = value;
}

void ImageWriter::setHasPlusLoads(bool value)
{
    getFlags().mayHavePlusLoads = value;
}

void ImageWriter::setIsBundle(bool value)
{
    getFlags().isBundle = value;
}

void ImageWriter::setIsDylib(bool value)
{
    getFlags().isDylib = value;
}

void ImageWriter::setIsExecutable(bool value)
{
    getFlags().isExecutable = value;
}

void ImageWriter::setHasWeakDefs(bool value)
{
    getFlags().hasWeakDefs = value;
}

void ImageWriter::setUses16KPages(bool value)
{
    getFlags().has16KBpages = value;
}

void ImageWriter::setOverridableDylib(bool value)
{
    getFlags().overridableDylib = value;
}

void ImageWriter::setInvalid()
{
    getFlags().isInvalid = true;
}

void ImageWriter::setInDyldCache(bool value)
{
    getFlags().inDyldCache = value;
}

void ImageWriter::setNeverUnload(bool value)
{
    getFlags().neverUnload = value;
}

void ImageWriter::setUUID(const uuid_t uuid)
{
    append(TypedBytes::Type::uuid, uuid, sizeof(uuid_t));
}

void ImageWriter::setCDHash(const uint8_t cdHash[20])
{
    append(TypedBytes::Type::cdHash, cdHash, 20);
}

void ImageWriter::setDependents(const Array<Image::LinkedImage>& deps)
{
    append(TypedBytes::Type::dependents, deps.begin(), (uint32_t)deps.count()*sizeof(Image::LinkedImage));
}

void ImageWriter::setDofOffsets(const Array<uint32_t>& dofSectionOffsets)
{
    append(TypedBytes::Type::dofOffsets, &dofSectionOffsets[0], (uint32_t)dofSectionOffsets.count()*sizeof(uint32_t));
}

void ImageWriter::setInitOffsets(const uint32_t initOffsets[], uint32_t count)
{
    append(TypedBytes::Type::initOffsets, initOffsets, count*sizeof(uint32_t));
}

void ImageWriter::setDiskSegments(const Image::DiskSegment segs[], uint32_t count)
{
    append(TypedBytes::Type::diskSegment, segs, count*sizeof(Image::DiskSegment));
}

void ImageWriter::setCachedSegments(const Image::DyldCacheSegment segs[], uint32_t count)
{
    append(TypedBytes::Type::cacheSegment, segs, count*sizeof(Image::DyldCacheSegment));
}

void ImageWriter::setCodeSignatureLocation(uint32_t fileOffset, uint32_t size)
{
    Image::CodeSignatureLocation loc;
    loc.fileOffset = fileOffset;
    loc.fileSize   = size;
    append(TypedBytes::Type::codeSignLoc, &loc, sizeof(loc));
}

void ImageWriter::setFairPlayEncryptionRange(uint32_t fileOffset, uint32_t size)
{
    const uint32_t pageSize = getFlags().has16KBpages ? 0x4000 : 0x1000;
    assert((fileOffset % pageSize) == 0);
    assert((size % pageSize) == 0);
    Image::FairPlayRange loc;
    loc.textStartPage = fileOffset/pageSize;
    loc.textPageCount = size/pageSize;
    append(TypedBytes::Type::fairPlayLoc, &loc, sizeof(loc));
}

void ImageWriter::setMappingInfo(uint64_t sliceOffset, uint64_t vmSize)
{
    const uint32_t pageSize = getFlags().has16KBpages ? 0x4000 : 0x1000;
    Image::MappingInfo info;
    info.sliceOffsetIn4K = (uint32_t)(sliceOffset / 0x1000);
    info.totalVmPages    = (uint32_t)(vmSize / pageSize);
    append(TypedBytes::Type::mappingInfo, &info, sizeof(info));
}

void ImageWriter::setFileInfo(uint64_t inode, uint64_t mTime)
{
    Image::FileInfo info = { inode, mTime };
    append(TypedBytes::Type::fileInodeAndTime, &info, sizeof(info));
}

void ImageWriter::setRebaseInfo(const Array<Image::RebasePattern>& fixups)
{
    append(TypedBytes::Type::rebaseFixups, fixups.begin(), (uint32_t)fixups.count()*sizeof(Image::RebasePattern));
}

void ImageWriter::setTextRebaseInfo(const Array<Image::TextFixupPattern>& fixups)
{
    append(TypedBytes::Type::textFixups, fixups.begin(), (uint32_t)fixups.count()*sizeof(Image::TextFixupPattern));
}

void ImageWriter::setBindInfo(const Array<Image::BindPattern>& fixups)
{
    append(TypedBytes::Type::bindFixups, fixups.begin(), (uint32_t)fixups.count()*sizeof(Image::BindPattern));
}

void ImageWriter::setChainedFixups(const Array<uint64_t>& starts, const Array<Image::ResolvedSymbolTarget>& targets)
{
    append(TypedBytes::Type::chainedFixupsStarts,  starts.begin(),  (uint32_t)starts.count()*sizeof(uint64_t));
    append(TypedBytes::Type::chainedFixupsTargets, targets.begin(), (uint32_t)targets.count()*sizeof(Image::ResolvedSymbolTarget));
}

void ImageWriter::addExportPatchInfo(uint32_t implCacheOff, const char* name, uint32_t locCount, const Image::PatchableExport::PatchLocation* locs)
{
    uint32_t roundedNameLen = ((uint32_t)strlen(name) + 1 + 3) & (-4);
    uint32_t payloadSize = sizeof(Image::PatchableExport) + locCount*sizeof(Image::PatchableExport::PatchLocation) + roundedNameLen;
    Image::PatchableExport* buffer = (Image::PatchableExport*)append(TypedBytes::Type::cachePatchInfo, nullptr, payloadSize);
    buffer->cacheOffsetOfImpl   = implCacheOff;
    buffer->patchLocationsCount = locCount;
    memcpy(&buffer->patchLocations[0], locs, locCount*sizeof(Image::PatchableExport::PatchLocation));
    strcpy((char*)(&buffer->patchLocations[locCount]), name);
}

void ImageWriter::setAsOverrideOf(ImageNum imageNum)
{
    uint32_t temp = imageNum;
    append(TypedBytes::Type::imageOverride, &temp, sizeof(temp));
}

void ImageWriter::setInitsOrder(const ImageNum images[], uint32_t count)
{
    append(TypedBytes::Type::initBefores, images, count*sizeof(ImageNum));
}


////////////////////////////  ImageArrayWriter ////////////////////////////////////////


ImageArrayWriter::ImageArrayWriter(ImageNum startImageNum, unsigned count) : _index(0)
{
    setContainerType(TypedBytes::Type::imageArray);
    _end = (void*)((uint8_t*)_end + sizeof(ImageArray) - sizeof(TypedBytes) + sizeof(uint32_t)*count);
    _containerTypedBytes->payloadLength = sizeof(ImageArray) - sizeof(TypedBytes) + sizeof(uint32_t)*count;
    ImageArray* ia = (ImageArray*)_containerTypedBytes;
    ia->firstImageNum   = startImageNum;
    ia->count           = count;
}

void ImageArrayWriter::appendImage(const Image* image)
{
    ImageArray* ia = (ImageArray*)_containerTypedBytes;
    ia->offsets[_index++] = _containerTypedBytes->payloadLength;
    append(TypedBytes::Type::image, image->payload(), image->payloadLength);
}

const ImageArray* ImageArrayWriter::finalize()
{
    return (ImageArray*)finalizeContainer();
}


////////////////////////////  ClosureWriter ////////////////////////////////////////

void ClosureWriter::setTopImageNum(ImageNum imageNum)
{
    append(TypedBytes::Type::topImage, &imageNum, sizeof(ImageNum));
}

void ClosureWriter::addCachePatches(const Array<Closure::PatchEntry>& patches)
{
    append(TypedBytes::Type::cacheOverrides, patches.begin(), (uint32_t)patches.count()*sizeof(Closure::PatchEntry));
}


////////////////////////////  LaunchClosureWriter ////////////////////////////////////////

LaunchClosureWriter::LaunchClosureWriter(const ImageArray* images)
{
    setContainerType(TypedBytes::Type::launchClosure);
    append(TypedBytes::Type::imageArray, images->payload(), images->payloadLength);
}

const LaunchClosure* LaunchClosureWriter::finalize()
{
    return (LaunchClosure*)finalizeContainer();
}

void LaunchClosureWriter::setLibSystemImageNum(ImageNum imageNum)
{
    append(TypedBytes::Type::libSystemNum, &imageNum, sizeof(ImageNum));
}

void LaunchClosureWriter::setLibDyldEntry(Image::ResolvedSymbolTarget entry)
{
    append(TypedBytes::Type::libDyldEntry, &entry, sizeof(entry));
}

void LaunchClosureWriter::setMainEntry(Image::ResolvedSymbolTarget main)
{
    append(TypedBytes::Type::mainEntry, &main, sizeof(main));
}

void LaunchClosureWriter::setStartEntry(Image::ResolvedSymbolTarget start)
{
    append(TypedBytes::Type::startEntry, &start, sizeof(start));
}

void LaunchClosureWriter::setUsedFallbackPaths(bool value)
{
    getFlags().usedFallbackPaths = value;
}

void LaunchClosureWriter::setUsedAtPaths(bool value)
{
    getFlags().usedAtPaths = value;
}

void LaunchClosureWriter::setInitImageCount(uint32_t count)
{
    getFlags().initImageCount = count;
}

LaunchClosure::Flags& LaunchClosureWriter::getFlags()
{
    if ( _flagsOffset == -1 ) {
        LaunchClosure::Flags flags;
        ::bzero(&flags, sizeof(flags));
        uint8_t* p = (uint8_t*)append(TypedBytes::Type::closureFlags, &flags, sizeof(flags));
        _flagsOffset = (int)(p - (uint8_t*)currentTypedBytes());
    }
    return *((LaunchClosure::Flags*)((uint8_t*)currentTypedBytes() + _flagsOffset));
}

void LaunchClosureWriter::setMustBeMissingFiles(const Array<const char*>& paths)
{
    uint32_t totalSize = 0;
    for (const char* s : paths)
        totalSize += (strlen(s) +1);
    totalSize = (totalSize + 3) & (-4); // align

    char* buffer = (char*)append(TypedBytes::Type::missingFiles, nullptr, totalSize);
    char* t = buffer;
    for (const char* path : paths) {
        for (const char* s=path; *s != '\0'; ++s)
            *t++ = *s;
        *t++ = '\0';
    }
    while (t < &buffer[totalSize])
        *t++ = '\0';
}

void LaunchClosureWriter::addEnvVar(const char* envVar)
{
    unsigned len = (unsigned)strlen(envVar);
    char temp[len+8];
    strcpy(temp, envVar);
    unsigned paddedSize = len+1;
    while ( (paddedSize % 4) != 0 )
        temp[paddedSize++] = '\0';
    append(TypedBytes::Type::envVar, temp, paddedSize);
}

void LaunchClosureWriter::addInterposingTuples(const Array<InterposingTuple>& tuples)
{
    append(TypedBytes::Type::interposeTuples, tuples.begin(), (uint32_t)tuples.count()*sizeof(InterposingTuple));
}

void LaunchClosureWriter::setDyldCacheUUID(const uuid_t uuid)
{
    append(TypedBytes::Type::dyldCacheUUID, uuid, sizeof(uuid_t));
}

void LaunchClosureWriter::setBootUUID(const char* uuid)
{
    unsigned len = (unsigned)strlen(uuid);
    char temp[len+8];
    strcpy(temp, uuid);
    unsigned paddedSize = len+1;
    while ( (paddedSize % 4) != 0 )
        temp[paddedSize++] = '\0';
    append(TypedBytes::Type::bootUUID, temp, paddedSize);
}

void LaunchClosureWriter::applyInterposing()
{
    const LaunchClosure* currentClosure = (LaunchClosure*)currentTypedBytes();
	const ImageArray*    images         = currentClosure->images();
	currentClosure->forEachInterposingTuple(^(const InterposingTuple& tuple, bool&) {
        images->forEachImage(^(const dyld3::closure::Image* image, bool&) {
            for (const Image::BindPattern& bindPat : image->bindFixups()) {
                if ( (bindPat.target == tuple.stockImplementation) && (tuple.newImplementation.image.imageNum != image->imageNum()) ) {
                    Image::BindPattern* writePat = const_cast<Image::BindPattern*>(&bindPat);
                    writePat->target = tuple.newImplementation;
                }
            }

            // Chained fixups may also be interposed.  We can't change elements in the chain, but we can change
            // the target list.
            for (const Image::ResolvedSymbolTarget& symbolTarget : image->chainedTargets()) {
                if ( (symbolTarget == tuple.stockImplementation) && (tuple.newImplementation.image.imageNum != image->imageNum()) ) {
                    Image::ResolvedSymbolTarget* writeTarget = const_cast<Image::ResolvedSymbolTarget*>(&symbolTarget);
                    *writeTarget = tuple.newImplementation;
                }
            }
        });
    });
}

////////////////////////////  DlopenClosureWriter ////////////////////////////////////////

DlopenClosureWriter::DlopenClosureWriter(const ImageArray* images)
{
    setContainerType(TypedBytes::Type::dlopenClosure);
    append(TypedBytes::Type::imageArray, images->payload(), images->payloadLength);
}

const DlopenClosure* DlopenClosureWriter::finalize()
{
    return (DlopenClosure*)finalizeContainer();
}


} // namespace closure
} // namespace dyld3



