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


#ifndef __ALL_IMAGES_H__
#define __ALL_IMAGES_H__

#include <mach-o/loader.h>
#include <pthread.h>
#include <os/lock_private.h>

#include "dyld_priv.h"

#include "Closure.h"
#include "Loading.h"
#include "MachOLoaded.h"
#include "DyldSharedCache.h"


#if __MAC_OS_X_VERSION_MIN_REQUIRED
// only in macOS and deprecated 
struct VIS_HIDDEN OFIInfo
{
    const char*                     path; //        = nullptr;
    const void*                     memSource; //   = nullptr;
    size_t                          memLength; //   = 0;
    const dyld3::MachOLoaded*       loadAddress; // = nullptr;
    uint64_t                        imageNum; //    = 0;
};
#endif

namespace dyld3 {

class VIS_HIDDEN AllImages
{
public:
    typedef void                (*NotifyFunc)(const mach_header* mh, intptr_t slide);
    typedef void                (*LoadNotifyFunc)(const mach_header* mh, const char* path, bool unloadable);

    void                        init(const closure::LaunchClosure* closure, const DyldSharedCache* dyldCacheLoadAddress, const char* dyldCachePath,
                                     const Array<LoadedImage>& initialImages);
    void                        setRestrictions(bool allowAtPaths, bool allowEnvPaths);
    void                        setMainPath(const char* path);
    void                        applyInitialImages();

    void                        addImages(const Array<LoadedImage>& newImages);
    void                        removeImages(const Array<LoadedImage>& unloadImages);
    void                        runImageNotifiers(const Array<LoadedImage>& newImages);
    void                        applyInterposingToDyldCache(const closure::Closure* closure);
    void                        runStartupInitialzers();
    void                        runInitialzersBottomUp(const closure::Image* topImage);
    void                        runLibSystemInitializer(const LoadedImage& libSystem);

    uint32_t                    count() const;

    void                        forEachImage(void (^handler)(const LoadedImage& loadedImage, bool& stop)) const;
    const MachOLoaded*          findDependent(const MachOLoaded* mh, uint32_t depIndex);
    void                        visitDependentsTopDown(const LoadedImage& start, void (^handler)(const LoadedImage& aLoadedImage, bool& stop)) const;
    void                        infoForImageMappedAt(const void* addr, void (^handler)(const LoadedImage& foundImage, uint8_t permissions)) const;
    bool                        infoForImageMappedAt(const void* addr, const MachOLoaded** ml, uint64_t* textSize, const char** path) const;
    void                        infoForNonCachedImageMappedAt(const void* addr, void (^handler)(const LoadedImage& foundImage, uint8_t permissions)) const;
    void                        infoForImageWithLoadAddress(const MachOLoaded*, void (^handler)(const LoadedImage& foundImage)) const;
    const char*                 pathForImageMappedAt(const void* addr) const;
    const char*                 imagePathByIndex(uint32_t index) const;
    const mach_header*          imageLoadAddressByIndex(uint32_t index) const;
    bool                        immutableMemory(const void* addr, size_t length) const;

    bool                        isRestricted() const;
    const MachOLoaded*          mainExecutable() const;
    const closure::Image*       mainExecutableImage() const;
    const void*                 cacheLoadAddress() const { return _dyldCacheAddress; }
    const char*                 dyldCachePath() const { return _dyldCachePath; }
    bool                        dyldCacheHasPath(const char* path) const;
    const char*                 imagePath(const closure::Image*) const;
    dyld_platform_t             platform() const;

    const Array<const closure::ImageArray*>& imagesArrays();

    void                        incRefCount(const mach_header* loadAddress);
    void                        decRefCount(const mach_header* loadAddress);

    void                        addLoadNotifier(NotifyFunc);
    void                        addUnloadNotifier(NotifyFunc);
    void                        setObjCNotifiers(_dyld_objc_notify_mapped, _dyld_objc_notify_init, _dyld_objc_notify_unmapped);
    void                        notifyObjCUnmap(const char* path, const struct mach_header* mh);
    void                        addLoadNotifier(LoadNotifyFunc);


    void                        setOldAllImageInfo(dyld_all_image_infos* old) { _oldAllImageInfos = old; }
    dyld_all_image_infos*       oldAllImageInfo() const { return _oldAllImageInfos;}
    void                        notifyMonitorMain();
    void                        notifyMonitorLoads(const Array<LoadedImage>& newImages);
    void                        notifyMonitorUnloads(const Array<LoadedImage>& unloadingImages);

#if __MAC_OS_X_VERSION_MIN_REQUIRED
    NSObjectFileImage           addNSObjectFileImage(const OFIInfo&);
    void                        removeNSObjectFileImage(NSObjectFileImage);
    bool                        forNSObjectFileImage(NSObjectFileImage imageHandle,
                                                     void (^handler)(OFIInfo& image));
#endif

    const MachOLoaded*          dlopen(Diagnostics& diag, const char* path, bool rtldNoLoad, bool rtldLocal, bool rtldNoDelete, bool fromOFI, const void* callerAddress);

    struct ProgramVars
    {
        const void*        mh;
        int*               NXArgcPtr;
        const char***      NXArgvPtr;
        const char***      environPtr;
        const char**       __prognamePtr;
    };
    void                    setProgramVars(ProgramVars* vars);

private:
    friend class Reaper;
    
    struct DlopenCount {
        const mach_header*  loadAddress;
        uintptr_t           refCount;
    };


    typedef void (*Initializer)(int argc, const char* argv[], char* envp[], const char* apple[], const ProgramVars* vars);
    typedef const Array<LoadedImage> StartImageArray;
    
    void                        runInitialzersInImage(const mach_header* imageLoadAddress, const closure::Image* image);
    void                        mirrorToOldAllImageInfos();
    void                        garbageCollectImages();
    void                        breadthFirstRecurseDependents(Array<closure::ImageNum>& visited, const LoadedImage& nodeLi, bool& stop, void (^handler)(const LoadedImage& aLoadedImage, bool& stop)) const;
    void                        appendToImagesArray(const closure::ImageArray* newArray);
    void                        withReadLock(void (^work)()) const;
    void                        withWriteLock(void (^work)());
    void                        withNotifiersLock(void (^work)()) const;
    bool                        findImage(const mach_header* loadAddress, LoadedImage& foundImage) const;
    bool                        findImageNum(closure::ImageNum imageNum, LoadedImage& foundImage) const;
    LoadedImage                 findImageNum(closure::ImageNum num, uint32_t& indexHint);
    bool                        swapImageState(closure::ImageNum num, uint32_t& indexHint, LoadedImage::State expectedCurrentState, LoadedImage::State newState);
    void                        runAllInitializersInImage(const closure::Image* image, const MachOLoaded* ml);
    void                        recomputeBounds();

    void                        constructMachPorts(int slot);
    void                        teardownMachPorts(int slot);
    void                        forEachPortSlot(void (^callback)(int slot));
    void                        sendMachMessage(int slot, mach_msg_id_t msg_id, mach_msg_header_t* msg_buffer, mach_msg_size_t msg_size);
    void                        notifyMonitoringDyld(bool unloading, const Array<LoadedImage>& images);

    typedef closure::ImageArray  ImageArray;

    const closure::LaunchClosure*           _mainClosure         = nullptr;
    const DyldSharedCache*                  _dyldCacheAddress    = nullptr;
    const char*                             _dyldCachePath       = nullptr;
    uint64_t                                _dyldCacheSlide      = 0;
    StartImageArray*                        _initialImages       = nullptr;
    const char*                             _mainExeOverridePath = nullptr;
    _dyld_objc_notify_mapped                _objcNotifyMapped    = nullptr;
    _dyld_objc_notify_init                  _objcNotifyInit      = nullptr;
    _dyld_objc_notify_unmapped              _objcNotifyUnmapped  = nullptr;
    ProgramVars*                            _programVars         = nullptr;
    dyld_all_image_infos*                   _oldAllImageInfos    = nullptr;
    dyld_image_info*                        _oldAllImageArray    = nullptr;
    dyld_uuid_info*                         _oldUUIDArray        = nullptr;
    dyld_platform_t                         _platform            = 0;
    uint32_t                                _oldArrayAllocCount  = 0;
    uint32_t                                _oldUUIDAllocCount   = 0;
    closure::ImageNum                       _nextImageNum        = 0;
    int32_t                                 _gcCount             = 0;
    bool                                    _processDOFs         = false;
    bool                                    _allowAtPaths        = false;
    bool                                    _allowEnvPaths       = false;
    uintptr_t                               _lowestNonCached     = 0;
    uintptr_t                               _highestNonCached    = UINTPTR_MAX;
#ifdef OS_UNFAIR_RECURSIVE_LOCK_INIT
    mutable os_unfair_recursive_lock        _loadImagesLock      = OS_UNFAIR_RECURSIVE_LOCK_INIT;
    mutable os_unfair_recursive_lock        _notifiersLock       = OS_UNFAIR_RECURSIVE_LOCK_INIT;
#else
    mutable pthread_mutex_t                 _loadImagesLock      = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
    mutable pthread_mutex_t                 _notifiersLock       = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#endif
    GrowableArray<const ImageArray*, 4, 4>  _imagesArrays;
    GrowableArray<NotifyFunc, 4, 4>         _loadNotifiers;
    GrowableArray<NotifyFunc, 4, 4>         _unloadNotifiers;
    GrowableArray<LoadNotifyFunc, 4, 4>     _loadNotifiers2;
    GrowableArray<DlopenCount, 4, 4>        _dlopenRefCounts;
    GrowableArray<LoadedImage, 16>          _loadedImages;
#if __MAC_OS_X_VERSION_MIN_REQUIRED
    uint64_t                                 _nextObjectFileImageNum = 0;
    GrowableArray<OFIInfo, 4, 1>             _objectFileImages;
#endif
};

extern AllImages gAllImages;


} // dyld3


#endif // __ALL_IMAGES_H__
