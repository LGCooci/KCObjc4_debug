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


#include <bitset>

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <mach/mach.h>
#include <sys/stat.h> 
#include <sys/types.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <sys/dtrace.h>
#include <sys/errno.h>
#include <unistd.h>
#include <System/sys/mman.h>
#include <System/sys/csr.h>
#include <System/machine/cpu_capabilities.h>
#include <bootstrap.h>
#include <CommonCrypto/CommonDigest.h>
#include <sandbox.h>
#include <sandbox/private.h>
#include <dispatch/dispatch.h>
#include <mach/vm_page_size.h>

#include "MachOFile.h"
#include "MachOLoaded.h"
#include "Logging.h"
#include "Loading.h"
#include "Tracing.h"
#include "dyld.h"
#include "dyld_cache_format.h"


namespace dyld {
    void log(const char* m, ...);
}


namespace {

// utility to track a set of ImageNum's in use
class VIS_HIDDEN ImageNumSet
{
public:
    void    add(dyld3::closure::ImageNum num);
    bool    contains(dyld3::closure::ImageNum num) const;

private:
    std::bitset<5120>                                   _bitmap;
    dyld3::OverflowSafeArray<dyld3::closure::ImageNum>  _overflowArray;
};

void ImageNumSet::add(dyld3::closure::ImageNum num)
{
    if ( num < 5120 )
        _bitmap.set(num);
    else
        _overflowArray.push_back(num);
}

bool ImageNumSet::contains(dyld3::closure::ImageNum num) const
{
    if ( num < 5120 )
        return _bitmap.test(num);

    for (dyld3::closure::ImageNum existingNum : _overflowArray) {
        if ( existingNum == num )
            return true;
    }
    return false;
}
} // namespace anonymous


namespace dyld3 {

Loader::Loader(Array<LoadedImage>& storage, const void* cacheAddress, const Array<const dyld3::closure::ImageArray*>& imagesArrays,
               LogFunc logLoads, LogFunc logSegments, LogFunc logFixups, LogFunc logDofs)
       : _allImages(storage), _imagesArrays(imagesArrays), _dyldCacheAddress(cacheAddress),
         _logLoads(logLoads), _logSegments(logSegments), _logFixups(logFixups), _logDofs(logDofs)
{
}

void Loader::addImage(const LoadedImage& li)
{
    _allImages.push_back(li);
}

LoadedImage* Loader::findImage(closure::ImageNum targetImageNum)
{
    for (LoadedImage& info : _allImages) {
        if ( info.image()->representsImageNum(targetImageNum) )
            return &info;
    }
    return nullptr;
}

uintptr_t Loader::resolveTarget(closure::Image::ResolvedSymbolTarget target)
{
    const LoadedImage* info;
    switch ( target.sharedCache.kind ) {
        case closure::Image::ResolvedSymbolTarget::kindSharedCache:
            assert(_dyldCacheAddress != nullptr);
            return (uintptr_t)_dyldCacheAddress + (uintptr_t)target.sharedCache.offset;

        case closure::Image::ResolvedSymbolTarget::kindImage:
            info = findImage(target.image.imageNum);
            assert(info != nullptr);
            return (uintptr_t)(info->loadedAddress()) + (uintptr_t)target.image.offset;

        case closure::Image::ResolvedSymbolTarget::kindAbsolute:
            if ( target.absolute.value & (1ULL << 62) )
                return (uintptr_t)(target.absolute.value | 0xC000000000000000ULL);
            else
                return (uintptr_t)target.absolute.value;
   }
    assert(0 && "malformed ResolvedSymbolTarget");
    return 0;
}


void Loader::completeAllDependents(Diagnostics& diag, uintptr_t topIndex)
{
    // accumulate all image overrides
    STACK_ALLOC_ARRAY(ImageOverride, overrides, _allImages.maxCount());
    for (const auto anArray : _imagesArrays) {
        // ignore prebuilt Image* in dyld cache
        if ( anArray->startImageNum() < dyld3::closure::kFirstLaunchClosureImageNum )
            continue;
        anArray->forEachImage(^(const dyld3::closure::Image* image, bool& stop) {
            ImageOverride overrideEntry;
            if ( image->isOverrideOfDyldCacheImage(overrideEntry.inCache) ) {
                overrideEntry.replacement = image->imageNum();
                overrides.push_back(overrideEntry);
            }
        });
    }

    // make cache for fast lookup of already loaded images
    __block ImageNumSet alreadyLoaded;
    for (int i=0; i <= topIndex; ++i) {
        alreadyLoaded.add(_allImages[i].image()->imageNum());
    }

    // for each image in _allImages, starting at topIndex, make sure its depenents are in _allImages
    uintptr_t index = topIndex;
    while ( (index < _allImages.count()) && diag.noError() ) {
        const closure::Image* image = _allImages[index].image();
        //fprintf(stderr, "completeAllDependents(): looking at dependents of %s\n", image->path());
        image->forEachDependentImage(^(uint32_t depIndex, closure::Image::LinkKind kind, closure::ImageNum depImageNum, bool& stop) {
            // check if imageNum needs to be changed to an override
            for (const ImageOverride& entry : overrides) {
                if ( entry.inCache == depImageNum ) {
                    depImageNum = entry.replacement;
                    break;
                }
            }
            // check if this dependent is already loaded
            if ( !alreadyLoaded.contains(depImageNum) ) {
                // if not, look in imagesArrays
                const closure::Image* depImage = closure::ImageArray::findImage(_imagesArrays, depImageNum);
                if ( depImage != nullptr ) {
                    //dyld::log("  load imageNum=0x%05X, image path=%s\n", depImageNum, depImage->path());
                     if ( _allImages.freeCount() == 0 ) {
                         diag.error("too many initial images");
                         stop = true;
                     }
                     else {
                         _allImages.push_back(LoadedImage::make(depImage));
                     }
                    alreadyLoaded.add(depImageNum);
                }
                else {
                    diag.error("unable to locate imageNum=0x%04X, depIndex=%d of %s", depImageNum, depIndex, image->path());
                    stop = true;
                }
            }
        });
        ++index;
    }
}

void Loader::mapAndFixupAllImages(Diagnostics& diag, bool processDOFs, bool fromOFI, uintptr_t topIndex)
{
    // scan array and map images not already loaded
    for (uintptr_t i=topIndex; i < _allImages.count(); ++i) {
        LoadedImage& info = _allImages[i];
        if ( info.loadedAddress() != nullptr ) {
            // log main executable's segments
            if ( (info.loadedAddress()->filetype == MH_EXECUTE) && (info.state() == LoadedImage::State::mapped) ) {
                if ( _logSegments("dyld: mapped by kernel %s\n", info.image()->path()) ) {
                    info.image()->forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
                        uint64_t start = (long)info.loadedAddress() + vmOffset;
                        uint64_t end   = start+vmSize-1;
                        if ( (segIndex == 0) && (permissions == 0) ) {
                            start = 0;
                        }
                        _logSegments("%14s (%c%c%c) 0x%012llX->0x%012llX \n", info.loadedAddress()->segmentName(segIndex),
                                    (permissions & PROT_READ) ? 'r' : '.', (permissions & PROT_WRITE) ? 'w' : '.', (permissions & PROT_EXEC) ? 'x' : '.' ,
                                    start, end);
                    });
                }
            }
            // skip over ones already loaded
            continue;
        }
        if ( info.image()->inDyldCache() ) {
            if ( info.image()->overridableDylib() ) {
                struct stat statBuf;
                if ( stat(info.image()->path(), &statBuf) == 0 ) {
                    // verify file has not changed since closure was built
                    uint64_t inode;
                    uint64_t mtime;
                    if ( info.image()->hasFileModTimeAndInode(inode, mtime) ) {
                        if ( (statBuf.st_mtime != mtime) || (statBuf.st_ino != inode) ) {
                            diag.error("dylib file mtime/inode changed since closure was built for '%s'", info.image()->path());
                        }
                    }
                    else {
                        diag.error("dylib file not expected on disk, must be a root '%s'", info.image()->path());
                    }
                }
                else if ( (_dyldCacheAddress != nullptr) && ((dyld_cache_header*)_dyldCacheAddress)->dylibsExpectedOnDisk ) {
                    diag.error("dylib file missing, was in dyld shared cache '%s'", info.image()->path());
                }
            }
            if ( diag.noError() ) {
                info.setLoadedAddress((MachOLoaded*)((uintptr_t)_dyldCacheAddress + info.image()->cacheOffset()));
                info.setState(LoadedImage::State::fixedUp);
                if ( _logSegments("dyld: Using from dyld cache %s\n", info.image()->path()) ) {
                    info.image()->forEachCacheSegment(^(uint32_t segIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool &stop) {
                        _logSegments("%14s (%c%c%c) 0x%012lX->0x%012lX \n", info.loadedAddress()->segmentName(segIndex),
                                        (permissions & PROT_READ) ? 'r' : '.', (permissions & PROT_WRITE) ? 'w' : '.', (permissions & PROT_EXEC) ? 'x' : '.' ,
                                        (long)info.loadedAddress()+(long)vmOffset, (long)info.loadedAddress()+(long)vmOffset+(long)vmSize-1);
                     });
                }
            }
        }
        else {
            mapImage(diag, info, fromOFI);
            if ( diag.hasError() )
                break; // out of for loop
        }

    }
    if ( diag.hasError() )  {
        // bummer, need to clean up by unmapping any images just mapped
        for (LoadedImage& info : _allImages) {
            if ( (info.state() == LoadedImage::State::mapped) && !info.image()->inDyldCache() && !info.leaveMapped() ) {
                _logSegments("dyld: unmapping %s\n", info.image()->path());
                unmapImage(info);
            }
        }
        return;
    }

    // apply fixups
    for (uintptr_t i=topIndex; i < _allImages.count(); ++i) {
        LoadedImage& info = _allImages[i];
        // images in shared cache do not need fixups applied
        if ( info.image()->inDyldCache() )
            continue;
        // previously loaded images were previously fixed up
        if ( info.state() < LoadedImage::State::fixedUp ) {
            applyFixupsToImage(diag, info);
            if ( diag.hasError() )
                break;
            info.setState(LoadedImage::State::fixedUp);
        }
    }

    // find and register dtrace DOFs
    if ( processDOFs ) {
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(DOFInfo, dofImages, _allImages.count());
        for (uintptr_t i=topIndex; i < _allImages.count(); ++i) {
            LoadedImage& info = _allImages[i];
            info.image()->forEachDOF(info.loadedAddress(), ^(const void* section) {
                DOFInfo dofInfo;
                dofInfo.dof            = section;
                dofInfo.imageHeader    = info.loadedAddress();
                dofInfo.imageShortName = info.image()->leafName();
                dofImages.push_back(dofInfo);
            });
        }
        registerDOFs(dofImages);
    }
}

bool Loader::sandboxBlocked(const char* path, const char* kind)
{
#if TARGET_IPHONE_SIMULATOR
    // sandbox calls not yet supported in dyld_sim
    return false;
#else
    sandbox_filter_type filter = (sandbox_filter_type)(SANDBOX_FILTER_PATH | SANDBOX_CHECK_NO_REPORT);
    return ( sandbox_check(getpid(), kind, filter, path) > 0 );
#endif
}

bool Loader::sandboxBlockedMmap(const char* path)
{
    return sandboxBlocked(path, "file-map-executable");
}

bool Loader::sandboxBlockedOpen(const char* path)
{
    return sandboxBlocked(path, "file-read-data");
}

bool Loader::sandboxBlockedStat(const char* path)
{
    return sandboxBlocked(path, "file-read-metadata");
}

void Loader::mapImage(Diagnostics& diag, LoadedImage& info, bool fromOFI)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_MAP_IMAGE, info.image()->path(), 0, 0);

    const closure::Image*   image         = info.image();
    uint64_t                sliceOffset   = image->sliceOffsetInFile();
    const uint64_t          totalVMSize   = image->vmSizeToMap();
    uint32_t                codeSignFileOffset;
    uint32_t                codeSignFileSize;
    bool                    isCodeSigned  = image->hasCodeSignature(codeSignFileOffset, codeSignFileSize);

    // open file
    int fd = ::open(info.image()->path(), O_RDONLY, 0);
    if ( fd == -1 ) {
        int openErr = errno;
        if ( (openErr == EPERM) && sandboxBlockedOpen(image->path()) )
            diag.error("file system sandbox blocked open(\"%s\", O_RDONLY)", image->path());
        else
            diag.error("open(\"%s\", O_RDONLY) failed with errno=%d", image->path(), openErr);
        return;
    }

    // get file info
    struct stat statBuf;
#if TARGET_IPHONE_SIMULATOR
    if ( stat(image->path(), &statBuf) != 0 ) {
#else
    if ( fstat(fd, &statBuf) != 0 ) {
#endif
        int statErr = errno;
        if ( (statErr == EPERM) && sandboxBlockedStat(image->path()) )
            diag.error("file system sandbox blocked stat(\"%s\")", image->path());
        else
            diag.error("stat(\"%s\") failed with errno=%d", image->path(), statErr);
        close(fd);
        return;
    }

    // verify file has not changed since closure was built
    uint64_t inode;
    uint64_t mtime;
    if ( image->hasFileModTimeAndInode(inode, mtime) ) {
        if ( (statBuf.st_mtime != mtime) || (statBuf.st_ino != inode) ) {
            diag.error("file mtime/inode changed since closure was built for '%s'", image->path());
            close(fd);
            return;
        }
    }

    // handle case on iOS where sliceOffset in closure is wrong because file was thinned after cache was built
    if ( (_dyldCacheAddress != nullptr) && !(((dyld_cache_header*)_dyldCacheAddress)->dylibsExpectedOnDisk) ) {
        if ( sliceOffset != 0 ) {
            if ( round_page_kernel(codeSignFileOffset+codeSignFileSize) == round_page_kernel(statBuf.st_size) ) {
                // file is now thin
                sliceOffset = 0;
            }
        }
    }

    // register code signature
    uint64_t coveredCodeLength  = UINT64_MAX;
    if ( isCodeSigned ) {
        auto sigTimer = ScopedTimer(DBG_DYLD_TIMING_ATTACH_CODESIGNATURE, 0, 0, 0);
        fsignatures_t siginfo;
        siginfo.fs_file_start  = sliceOffset;                           // start of mach-o slice in fat file
        siginfo.fs_blob_start  = (void*)(long)(codeSignFileOffset);     // start of CD in mach-o file
        siginfo.fs_blob_size   = codeSignFileSize;                      // size of CD
        int result = fcntl(fd, F_ADDFILESIGS_RETURN, &siginfo);
        if ( result == -1 ) {
            int errnoCopy = errno;
            if ( (errnoCopy == EPERM) || (errnoCopy == EBADEXEC) ) {
                diag.error("code signature invalid (errno=%d) sliceOffset=0x%08llX, codeBlobOffset=0x%08X, codeBlobSize=0x%08X for '%s'",
                            errnoCopy, sliceOffset, codeSignFileOffset, codeSignFileSize, image->path());
            }
            else {
                diag.error("fcntl(fd, F_ADDFILESIGS_RETURN) failed with errno=%d, sliceOffset=0x%08llX, codeBlobOffset=0x%08X, codeBlobSize=0x%08X for '%s'",
                            errnoCopy, sliceOffset, codeSignFileOffset, codeSignFileSize, image->path());
            }
            close(fd);
            return;
        }
        coveredCodeLength = siginfo.fs_file_start;
        if ( coveredCodeLength < codeSignFileOffset ) {
            diag.error("code signature does not cover entire file up to signature");
            close(fd);
            return;
        }
    }

    // <rdar://problem/41015217> dyld should use F_CHECK_LV even on unsigned binaries
    {
        // <rdar://problem/32684903> always call F_CHECK_LV to preflight
        fchecklv checkInfo;
        char  messageBuffer[512];
        messageBuffer[0] = '\0';
        checkInfo.lv_file_start = sliceOffset;
        checkInfo.lv_error_message_size = sizeof(messageBuffer);
        checkInfo.lv_error_message = messageBuffer;
        int res = fcntl(fd, F_CHECK_LV, &checkInfo);
        if ( res == -1 ) {
             diag.error("code signature in (%s) not valid for use in process: %s", image->path(), messageBuffer);
             close(fd);
             return;
        }
    }

    // reserve address range
    vm_address_t loadAddress = 0;
    kern_return_t r = vm_allocate(mach_task_self(), &loadAddress, (vm_size_t)totalVMSize, VM_FLAGS_ANYWHERE);
    if ( r != KERN_SUCCESS ) {
        diag.error("vm_allocate(size=0x%0llX) failed with result=%d", totalVMSize, r);
        close(fd);
        return;
    }

    if ( sliceOffset != 0 )
        _logSegments("dyld: Mapping %s (slice offset=%llu)\n", image->path(), sliceOffset);
    else
        _logSegments("dyld: Mapping %s\n", image->path());

    // map each segment
    __block bool mmapFailure = false;
    __block const uint8_t* codeSignatureStartAddress = nullptr;
    __block const uint8_t* linkeditEndAddress = nullptr;
    __block bool mappedFirstSegment = false;
    image->forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
        // <rdar://problem/32363581> Mapping zero filled segments fails with mmap of size 0
        if ( fileSize == 0 )
            return;
        void* segAddress = mmap((void*)(loadAddress+vmOffset), fileSize, permissions, MAP_FIXED | MAP_PRIVATE, fd, sliceOffset+fileOffset);
        int mmapErr = errno;
        if ( segAddress == MAP_FAILED ) {
            if ( mmapErr == EPERM ) {
                if ( sandboxBlockedMmap(image->path()) )
                    diag.error("file system sandbox blocked mmap() of '%s'", image->path());
                else
                    diag.error("code signing blocked mmap() of '%s'", image->path());
            }
            else {
                diag.error("mmap(addr=0x%0llX, size=0x%08X) failed with errno=%d for %s", loadAddress+vmOffset, fileSize, mmapErr, image->path());
            }
            mmapFailure = true;
            stop = true;
        }
        else if ( codeSignFileOffset > fileOffset ) {
            codeSignatureStartAddress = (uint8_t*)segAddress + (codeSignFileOffset-fileOffset);
            linkeditEndAddress = (uint8_t*)segAddress + vmSize;
        }
        // sanity check first segment is mach-o header
        if ( (segAddress != MAP_FAILED) && !mappedFirstSegment ) {
            mappedFirstSegment = true;
            const MachOFile* mf = (MachOFile*)segAddress;
            if ( !mf->isMachO(diag, fileSize) ) {
                mmapFailure = true;
                stop = true;
            }
        }
        if ( !mmapFailure ) {
            const MachOLoaded* lmo = (MachOLoaded*)loadAddress;
            _logSegments("%14s (%c%c%c) 0x%012lX->0x%012lX \n", lmo->segmentName(segIndex),
                         (permissions & PROT_READ) ? 'r' : '.', (permissions & PROT_WRITE) ? 'w' : '.', (permissions & PROT_EXEC) ? 'x' : '.' ,
                         (long)segAddress, (long)segAddress+(long)vmSize-1);
        }
    });
    if ( mmapFailure ) {
        ::vm_deallocate(mach_task_self(), loadAddress, (vm_size_t)totalVMSize);
        ::close(fd);
        return;
    }

    // close file
    close(fd);

#if BUILDING_LIBDYLD
    // verify file has not changed since closure was built by checking code signature has not changed
    uint8_t cdHashExpected[20];
    if ( image->hasCdHash(cdHashExpected) ) {
        if ( codeSignatureStartAddress == nullptr ) {
            diag.error("code signature missing");
        }
        else if ( codeSignatureStartAddress+codeSignFileSize > linkeditEndAddress ) {
            diag.error("code signature extends beyond end of __LINKEDIT");
        }
        else {
            uint8_t cdHashFound[20];
            const MachOLoaded* lmo = (MachOLoaded*)loadAddress;
            if ( lmo->cdHashOfCodeSignature(codeSignatureStartAddress, codeSignFileSize, cdHashFound) ) {
                if ( ::memcmp(cdHashFound, cdHashExpected, 20) != 0 )
                    diag.error("code signature changed since closure was built");
            }
            else {
                diag.error("code signature format invalid");
            }
        }
        if ( diag.hasError() ) {
            ::vm_deallocate(mach_task_self(), loadAddress, (vm_size_t)totalVMSize);
            return;
        }
    }
#endif

#if __IPHONE_OS_VERSION_MIN_REQUIRED && !TARGET_IPHONE_SIMULATOR
    // tell kernel about fairplay encrypted regions
    uint32_t fpTextOffset;
    uint32_t fpSize;
    if ( image->isFairPlayEncrypted(fpTextOffset, fpSize) ) {
        const mach_header* mh = (mach_header*)loadAddress;
        int result = ::mremap_encrypted(((uint8_t*)mh) + fpTextOffset, fpSize, 1, mh->cputype, mh->cpusubtype);
        if ( result != 0 ) {
            diag.error("could not register fairplay decryption, mremap_encrypted() => %d", result);
            ::vm_deallocate(mach_task_self(), loadAddress, (vm_size_t)totalVMSize);
            return;
        }
    }
#endif

    _logLoads("dyld: load %s\n", image->path());

    timer.setData4((uint64_t)loadAddress);
    info.setLoadedAddress((MachOLoaded*)loadAddress);
    info.setState(LoadedImage::State::mapped);
}

void Loader::unmapImage(LoadedImage& info)
{
    assert(info.loadedAddress() != nullptr);
    ::vm_deallocate(mach_task_self(), (vm_address_t)info.loadedAddress(), (vm_size_t)(info.image()->vmSizeToMap()));
    info.setLoadedAddress(nullptr);
}

void Loader::registerDOFs(const Array<DOFInfo>& dofs)
{
    if ( dofs.empty() )
        return;

    int fd = open("/dev/" DTRACEMNR_HELPER, O_RDWR);
    if ( fd < 0 ) {
        _logDofs("can't open /dev/" DTRACEMNR_HELPER " to register dtrace DOF sections\n");
    }
    else {
        // allocate a buffer on the stack for the variable length dof_ioctl_data_t type
        uint8_t buffer[sizeof(dof_ioctl_data_t) + dofs.count()*sizeof(dof_helper_t)];
        dof_ioctl_data_t* ioctlData = (dof_ioctl_data_t*)buffer;

        // fill in buffer with one dof_helper_t per DOF section
        ioctlData->dofiod_count = dofs.count();
        for (unsigned int i=0; i < dofs.count(); ++i) {
            strlcpy(ioctlData->dofiod_helpers[i].dofhp_mod, dofs[i].imageShortName, DTRACE_MODNAMELEN);
            ioctlData->dofiod_helpers[i].dofhp_dof = (uintptr_t)(dofs[i].dof);
            ioctlData->dofiod_helpers[i].dofhp_addr = (uintptr_t)(dofs[i].dof);
        }

        // tell kernel about all DOF sections en mas
        // pass pointer to ioctlData because ioctl() only copies a fixed size amount of data into kernel
        user_addr_t val = (user_addr_t)(unsigned long)ioctlData;
        if ( ioctl(fd, DTRACEHIOC_ADDDOF, &val) != -1 ) {
            // kernel returns a unique identifier for each section in the dofiod_helpers[].dofhp_dof field.
            // Note, the closure marked the image as being never unload, so we don't need to keep the ID around
            // or support unregistering it later.
            for (unsigned int i=0; i < dofs.count(); ++i) {
                _logDofs("dyld: registering DOF section %p in %s with dtrace, ID=0x%08X\n",
                         dofs[i].dof, dofs[i].imageShortName, (int)(ioctlData->dofiod_helpers[i].dofhp_dof));
            }
        }
        else {
            _logDofs("dyld: ioctl to register dtrace DOF section failed\n");
        }
        close(fd);
    }
}

bool Loader::dtraceUserProbesEnabled()
{
#if __IPHONE_OS_VERSION_MIN_REQUIRED && !TARGET_IPHONE_SIMULATOR
    int    dof_mode;
    size_t dof_mode_size = sizeof(dof_mode);
    if ( sysctlbyname("kern.dtrace.dof_mode", &dof_mode, &dof_mode_size, nullptr, 0) == 0 ) {
        return ( dof_mode != 0 );
    }
    return false;
#else
    // dtrace is always available for macOS and simulators
    return true;
#endif
}


void Loader::vmAccountingSetSuspended(bool suspend, LogFunc logger)
{
#if __arm__ || __arm64__
    // <rdar://problem/29099600> dyld should tell the kernel when it is doing fix-ups caused by roots
    logger("vm.footprint_suspend=%d\n", suspend);
    int newValue = suspend ? 1 : 0;
    int oldValue = 0;
    size_t newlen = sizeof(newValue);
    size_t oldlen = sizeof(oldValue);
    sysctlbyname("vm.footprint_suspend", &oldValue, &oldlen, &newValue, newlen);
#endif
}

void Loader::applyFixupsToImage(Diagnostics& diag, LoadedImage& info)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_APPLY_FIXUPS, (uint64_t)info.loadedAddress(), 0, 0);
    closure::ImageNum       cacheImageNum;
    const char*             leafName         = info.image()->leafName();
    const closure::Image*   image            = info.image();
    const uint8_t*          imageLoadAddress = (uint8_t*)info.loadedAddress();
    uintptr_t               slide            = info.loadedAddress()->getSlide();
    bool                    overrideOfCache  = info.image()->isOverrideOfDyldCacheImage(cacheImageNum);
    if ( overrideOfCache )
        vmAccountingSetSuspended(true, _logFixups);
    image->forEachFixup(^(uint64_t imageOffsetToRebase, bool &stop) {
        uintptr_t* fixUpLoc = (uintptr_t*)(imageLoadAddress + imageOffsetToRebase);
        *fixUpLoc += slide;
        _logFixups("dyld: fixup: %s:%p += %p\n", leafName, fixUpLoc, (void*)slide);
    },
    ^(uint64_t imageOffsetToBind, closure::Image::ResolvedSymbolTarget bindTarget, bool &stop) {
        uintptr_t* fixUpLoc = (uintptr_t*)(imageLoadAddress + imageOffsetToBind);
        uintptr_t value = resolveTarget(bindTarget);
        _logFixups("dyld: fixup: %s:%p = %p\n", leafName, fixUpLoc, (void*)value);
        *fixUpLoc = value;
    },
    ^(uint64_t imageOffsetStart, const Array<closure::Image::ResolvedSymbolTarget>& targets, bool& stop) {
        // walk each fixup in the chain
        image->forEachChainedFixup((void*)imageLoadAddress, imageOffsetStart, ^(uint64_t* fixupLoc, MachOLoaded::ChainedFixupPointerOnDisk fixupInfo, bool& stopChain) {
            if ( fixupInfo.authRebase.auth ) {
    #if  __has_feature(ptrauth_calls)
                if ( fixupInfo.authBind.bind ) {
                    closure::Image::ResolvedSymbolTarget bindTarget = targets[fixupInfo.authBind.ordinal];
                    uint64_t targetAddr = resolveTarget(bindTarget);
                    // Don't sign missing weak imports.
                    if (targetAddr != 0)
                        targetAddr = fixupInfo.signPointer(fixupLoc, targetAddr);
                    _logFixups("dyld: fixup: *%p = %p (JOP: diversity 0x%04X, addr-div=%d, key=%s)\n",
                               fixupLoc, (void*)targetAddr, fixupInfo.authBind.diversity, fixupInfo.authBind.addrDiv, fixupInfo.authBind.keyName());
                    *fixupLoc = targetAddr;
                }
                else {
                    uint64_t targetAddr = (uint64_t)imageLoadAddress + fixupInfo.authRebase.target;
                    targetAddr = fixupInfo.signPointer(fixupLoc, targetAddr);
                    _logFixups("dyld: fixup: *%p = %p (JOP: diversity 0x%04X, addr-div=%d, key=%s)\n",
                               fixupLoc, (void*)targetAddr, fixupInfo.authRebase.diversity, fixupInfo.authRebase.addrDiv, fixupInfo.authRebase.keyName());
                    *fixupLoc = targetAddr;
                }
    #else
                diag.error("malformed chained pointer");
                stop = true;
                stopChain = true;
    #endif
            }
            else {
                if ( fixupInfo.plainRebase.bind ) {
                    closure::Image::ResolvedSymbolTarget bindTarget = targets[fixupInfo.plainBind.ordinal];
                    uint64_t targetAddr = resolveTarget(bindTarget) + fixupInfo.plainBind.signExtendedAddend();
                    _logFixups("dyld: fixup: %s:%p = %p\n", leafName, fixupLoc, (void*)targetAddr);
                    *fixupLoc = targetAddr;
                }
                else {
                    uint64_t targetAddr = fixupInfo.plainRebase.signExtendedTarget() + slide;
                    _logFixups("dyld: fixup: %s:%p += %p\n", leafName, fixupLoc, (void*)slide);
                    *fixupLoc = targetAddr;
                }
            }
        });
    });

#if __i386__
    __block bool segmentsMadeWritable = false;
    image->forEachTextReloc(^(uint32_t imageOffsetToRebase, bool& stop) {
        if ( !segmentsMadeWritable )
            setSegmentProtects(info, true);
        uintptr_t* fixUpLoc = (uintptr_t*)(imageLoadAddress + imageOffsetToRebase);
        *fixUpLoc += slide;
        _logFixups("dyld: fixup: %s:%p += %p\n", leafName, fixUpLoc, (void*)slide);
     },
    ^(uint32_t imageOffsetToBind, closure::Image::ResolvedSymbolTarget bindTarget, bool& stop) {
        // FIXME
    });
    if ( segmentsMadeWritable )
        setSegmentProtects(info, false);
#endif

    if ( overrideOfCache )
        vmAccountingSetSuspended(false, _logFixups);
}

#if __i386__
void Loader::setSegmentProtects(const LoadedImage& info, bool write)
{
    info.image()->forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t protections, bool& segStop) {
        if ( protections & VM_PROT_WRITE )
            return;
        uint32_t regionProt = protections;
        if ( write )
            regionProt = VM_PROT_WRITE | VM_PROT_READ;
        kern_return_t r = vm_protect(mach_task_self(), ((uintptr_t)info.loadedAddress())+(uintptr_t)vmOffset, (uintptr_t)vmSize, false, regionProt);
        assert( r == KERN_SUCCESS );
    });
}
#endif

#if BUILDING_DYLD
void forEachLineInFile(const char* buffer, size_t bufferLen, void (^lineHandler)(const char* line, bool& stop))
{
    bool stop = false;
    const char* const eof = &buffer[bufferLen];
    for (const char* s = buffer; s < eof; ++s) {
        char lineBuffer[MAXPATHLEN];
        char* t = lineBuffer;
        char* tEnd = &lineBuffer[MAXPATHLEN];
        while ( (s < eof) && (t != tEnd) ) {
            if ( *s == '\n' )
            break;
            *t++ = *s++;
        }
        *t = '\0';
        lineHandler(lineBuffer, stop);
        if ( stop )
        break;
    }
}

void forEachLineInFile(const char* path, void (^lineHandler)(const char* line, bool& stop))
{
    int fd = dyld::my_open(path, O_RDONLY, 0);
    if ( fd != -1 ) {
        struct stat statBuf;
        if ( fstat(fd, &statBuf) == 0 ) {
            const char* lines = (const char*)mmap(nullptr, (size_t)statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if ( lines != MAP_FAILED ) {
                forEachLineInFile(lines, (size_t)statBuf.st_size, lineHandler);
                munmap((void*)lines, (size_t)statBuf.st_size);
            }
        }
        close(fd);
    }
}


bool internalInstall()
{
#if TARGET_IPHONE_SIMULATOR
    return false;
#elif __IPHONE_OS_VERSION_MIN_REQUIRED
    uint32_t devFlags = *((uint32_t*)_COMM_PAGE_DEV_FIRM);
    return ( (devFlags & 1) == 1 );
#else
    return ( csr_check(CSR_ALLOW_APPLE_INTERNAL) == 0 );
#endif
}

/* Checks to see if there are any args that impact dyld. These args
 * can be set sevaral ways. These will only be honored on development
 * and Apple Internal builds.
 *
 * First the existence of a file is checked for:
 *    /S/L/C/com.apple.dyld/dyld-bootargs
 * If it exists it will be mapped and scanned line by line. If the executable
 * exists in the file then the arguments on its line will be applied. "*" may
 * be used a wildcard to represent all apps. First matching line will be used,
 * the wild card must be one the last line. Additionally, lines must end with
 * a "\n"
 *
 *
 * SAMPLE FILE:

 /bin/ls:force_dyld2=1
 /usr/bin/sw_vers:force_dyld2=1
*:force_dyld3=1
EOL

 If no file exists then the kernel boot-args will be scanned.
 */
bool bootArgsContains(const char* arg)
{
    //FIXME: Use strnstr(). Unfortunately we are missing an imp libc.
#if TARGET_IPHONE_SIMULATOR
    return false;
#else
    // don't check for boot-args on customer installs
    if ( !internalInstall() )
        return false;

    char pathBuffer[MAXPATHLEN+1];
#if __IPHONE_OS_VERSION_MIN_REQUIRED
    strlcpy(pathBuffer, IPHONE_DYLD_SHARED_CACHE_DIR, sizeof(IPHONE_DYLD_SHARED_CACHE_DIR));
#else
    strlcpy(pathBuffer, MACOSX_DYLD_SHARED_CACHE_DIR, sizeof(MACOSX_DYLD_SHARED_CACHE_DIR));
#endif
    strlcat(pathBuffer, "dyld-bootargs", MAXPATHLEN+1);
    __block bool result = false;
    forEachLineInFile(pathBuffer, ^(const char* line, bool& stop) {
        const char* delim = strchr(line, ':');
        if ( delim == nullptr )
            return;
        char binary[MAXPATHLEN];
        char options[MAXPATHLEN];
        strlcpy(binary, line, MAXPATHLEN);
        binary[delim-line] = '\0';
        strlcpy(options, delim+1, MAXPATHLEN);
        if ( (strcmp(dyld::getExecutablePath(), binary) == 0) || (strcmp("*", binary) == 0) ) {
            result = (strstr(options, arg) != nullptr);
            return;
        }
    });

    // get length of full boot-args string
    size_t len;
    if ( sysctlbyname("kern.bootargs", NULL, &len, NULL, 0) != 0 )
        return false;

    // get copy of boot-args string
    char bootArgsBuffer[len];
    if ( sysctlbyname("kern.bootargs", bootArgsBuffer, &len, NULL, 0) != 0 )
        return false;

    // return true if 'arg' is a sub-string of boot-args
    return (strstr(bootArgsBuffer, arg) != nullptr);
#endif
}
#endif

#if BUILDING_LIBDYLD
// hack because libdyld.dylib should not link with libc++.dylib
extern "C" void __cxa_pure_virtual() __attribute__((visibility("hidden")));
void __cxa_pure_virtual()
{
    abort();
}
#endif

} // namespace dyld3





