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


#ifndef ClosureBuilder_h
#define ClosureBuilder_h


#include "Closure.h"
#include "ClosureFileSystem.h"
#include "ClosureWriter.h"
#include "PathOverrides.h"
#include "DyldSharedCache.h"
#include "MachOAnalyzer.h"
#include "Loading.h"



namespace dyld3 {


namespace closure {



class VIS_HIDDEN ClosureBuilder
{
public:

    struct LaunchErrorInfo
    {
        uintptr_t       kind;
        const char*     clientOfDylibPath;
        const char*     targetDylibPath;
        const char*     symbol;
    };

    struct ResolvedTargetInfo
    {
        const MachOLoaded* foundInDylib;
        const char*        requestedSymbolName;
        const char*        foundSymbolName;
        uint64_t           addend;
        bool               weakBindCoalese;
        bool               weakBindSameImage;
        bool               isWeakDef;
        int                libOrdinal;
    };

    typedef Image::PatchableExport::PatchLocation  PatchLocation;

    struct CacheDylibsBindingHandlers
    {
        struct PatchInfo
        {
            const char*             exportSymbolName;
            uint32_t                exportCacheOffset;
            uint32_t                usesCount;
            const PatchLocation*    usesArray;
        };

        void        (^rebase)(ImageNum, const MachOLoaded* imageToFix, uint32_t runtimeOffset);
        void        (^bind)(ImageNum, const MachOLoaded* imageToFix, uint32_t runtimeOffset, Image::ResolvedSymbolTarget target, const ResolvedTargetInfo& targetInfo);
        void        (^chainedBind)(ImageNum, const MachOLoaded*, const Array<uint64_t>& starts, const Array<Image::ResolvedSymbolTarget>& targets, const Array<ResolvedTargetInfo>& targetInfos);
        void        (^forEachExportsPatch)(ImageNum, void (^handler)(const PatchInfo&));
   };

    enum class AtPath { none, all, onlyInRPaths };

                                ClosureBuilder(uint32_t startImageNum, const FileSystem& fileSystem, const DyldSharedCache* dyldCache, bool dyldCacheIsLive,
                                               const PathOverrides& pathOverrides, AtPath atPathHandling=AtPath::all,
                                               LaunchErrorInfo* errorInfo=nullptr,
                                               const char* archName=MachOFile::currentArchName(), Platform platform=MachOFile::currentPlatform(),
                                               const CacheDylibsBindingHandlers* handlers=nullptr);
                                ~ClosureBuilder();
    Diagnostics&                diagnostics() { return _diag; }

    const LaunchClosure*        makeLaunchClosure(const LoadedFileInfo& fileInfo, bool allowInsertFailures);

    const LaunchClosure*        makeLaunchClosure(const char* mainPath,bool allowInsertFailures);


    static const DlopenClosure* sRetryDlopenClosure;
    const DlopenClosure*        makeDlopenClosure(const char* dylibPath, const LaunchClosure* mainClosure, const Array<LoadedImage>& loadedList,
                                                  closure::ImageNum callerImageNum, bool noLoad, bool canUseSharedCacheClosure,
                                                  closure::ImageNum* topImageNum);

    ImageNum                    nextFreeImageNum() const { return _startImageNum + _nextIndex; }


    struct PatchableExport
    {
        uint32_t                cacheOffsetOfImpl;
        uint32_t                cacheOffsetOfName;
        uint32_t                patchLocationsCount;
        const PatchLocation*    patchLocations;
    };

    struct CachedDylibInfo
    {
        LoadedFileInfo          fileInfo;
    };

    struct CachedDylibAlias
    {
        const char*             realPath;
        const char*             aliasPath;
    };

    const ImageArray*           makeDyldCacheImageArray(bool customerCache, const Array<CachedDylibInfo>& dylibs, const Array<CachedDylibAlias>& aliases);

    const ImageArray*           makeOtherDylibsImageArray(const Array<LoadedFileInfo>& otherDylibs, uint32_t cachedDylibsCount);

    static void                 buildLoadOrder(Array<LoadedImage>& loadedList, const Array<const ImageArray*>& imagesArrays, const Closure* toAdd);

private:


    struct InitInfo
    {
        uint32_t    initOrder;
        bool        danglingUpward;
        bool        visited;
    };

    struct BuilderLoadedImage
    {
        Array<Image::LinkedImage>   dependents;
        ImageNum                    imageNum;
        uint32_t                    unmapWhenDone      : 1,
                                    contentRebased     : 1,
                                    hasInits           : 1,
                                    markNeverUnload    : 1,
                                    rtldLocal          : 1,
                                    isBadImage         : 1,
                                    padding            : 14,
                                    overrideImageNum   : 12;
        LoadedFileInfo              loadedFileInfo;

        // Convenience method to get the information from the loadedFileInfo
        const MachOAnalyzer*        loadAddress() const { return (const MachOAnalyzer*)loadedFileInfo.fileContent; }
        const char*                 path() const { return loadedFileInfo.path; }
    };

    struct LoadedImageChain
    {
        LoadedImageChain*    previous;
        BuilderLoadedImage&  image;
    };


    void                    recursiveLoadDependents(LoadedImageChain& forImageChain);
    void                    loadDanglingUpwardLinks();
    const char*             resolvePathVar(const char* loadPath, const LoadedImageChain& forImageChain, bool implictRPath);
    bool                    findImage(const char* loadPath, const LoadedImageChain& forImageChain, BuilderLoadedImage*& foundImage, bool mustBeDylib, bool allowOther=true);
    void                    buildImage(ImageWriter& writer, BuilderLoadedImage& forImage);
    void                    addSegments(ImageWriter& writer, const MachOAnalyzer* mh);
    void                    addRebaseInfo(ImageWriter& writer, const MachOAnalyzer* mh);
    void                    addSynthesizedRebaseInfo(ImageWriter& writer, const MachOAnalyzer* mh);
    void                    addSynthesizedBindInfo(ImageWriter& writer, const MachOAnalyzer* mh);
    void                    addBindInfo(ImageWriter& writer, BuilderLoadedImage& forImage);
    void                    reportRebasesAndBinds(ImageWriter& writer, BuilderLoadedImage& forImage);
    void                    addChainedFixupInfo(ImageWriter& writer, const BuilderLoadedImage& forImage);
    void                    addInterposingTuples(LaunchClosureWriter& writer, const Image* image, const MachOAnalyzer* mh);
    void                    computeInitOrder(ImageWriter& writer, uint32_t loadIndex);
    void                    addCachePatchInfo(ImageWriter& writer, const BuilderLoadedImage& forImage);
    void                    addClosureInfo(LaunchClosureWriter& closureWriter);
    void                    depthFirstRecurseSetInitInfo(uint32_t loadIndex, InitInfo initInfos[], uint32_t& initOrder, bool& hasError);
    bool                    findSymbol(const BuilderLoadedImage& fromImage, int libraryOrdinal, const char* symbolName, bool weakImport, uint64_t addend,
                                       Image::ResolvedSymbolTarget& target, ResolvedTargetInfo& targetInfo);
    bool                    findSymbolInImage(const MachOAnalyzer* macho, const char* symbolName, uint64_t addend, bool followReExports, Image::ResolvedSymbolTarget& target, ResolvedTargetInfo& targetInfo);
    const MachOAnalyzer*    machOForImageNum(ImageNum imageNum);
    ImageNum                imageNumForMachO(const MachOAnalyzer* mh);
    const MachOAnalyzer*    findDependent(const MachOLoaded* mh, uint32_t depIndex);
    BuilderLoadedImage&     findLoadedImage(ImageNum imageNum);
    BuilderLoadedImage&     findLoadedImage(const MachOAnalyzer* mh);
    uint32_t                index(const BuilderLoadedImage&);
    bool                    expandAtLoaderPath(const char* loadPath, bool fromLCRPATH, const BuilderLoadedImage& loadedImage, char fixedPath[]);
    bool                    expandAtExecutablePath(const char* loadPath, bool fromLCRPATH, char fixedPath[]);
    void                    addMustBeMissingPath(const char* path);
    const char*             strdup_temp(const char* path);
    bool                    overridableDylib(const BuilderLoadedImage& forImage);
    void                    forEachBind(BuilderLoadedImage& forImage, void (^handler)(uint64_t runtimeOffset, Image::ResolvedSymbolTarget target, const ResolvedTargetInfo& targetInfo, bool& stop),
                                                                      void (^strongHandler)(const char* strongSymbolName));

    static bool             inLoadedImageArray(const Array<LoadedImage>& loadedList, ImageNum imageNum);
    static void             buildLoadOrderRecurse(Array<LoadedImage>& loadedList, const Array<const ImageArray*>& imagesArrays, const Image* toAdd);

    const FileSystem&                       _fileSystem;
    const DyldSharedCache* const            _dyldCache;
    const PathOverrides&                    _pathOverrides;
    const char* const                       _archName;
    Platform const                          _platform;
    uint32_t const                          _startImageNum;
    const ImageArray*                       _dyldImageArray        = nullptr;
    const CacheDylibsBindingHandlers*       _handlers              = nullptr;
    const Array<CachedDylibAlias>*          _aliases               = nullptr;
    const AtPath                            _atPathHandling        = AtPath::none;
    uint32_t                                _mainProgLoadIndex     = 0;
    Diagnostics                             _diag;
    LaunchErrorInfo*                        _launchErrorInfo       = nullptr;
    PathPool*                               _tempPaths             = nullptr;
    PathPool*                               _mustBeMissingPaths    = nullptr;
    uint32_t                                _nextIndex             = 0;
    OverflowSafeArray<BuilderLoadedImage>   _loadedImages;
    OverflowSafeArray<Image::LinkedImage,65536> _dependencies;                  // all dylibs in cache need ~20,000 edges
    OverflowSafeArray<InterposingTuple>     _interposingTuples;
    OverflowSafeArray<Closure::PatchEntry>  _weakDefCacheOverrides;
    OverflowSafeArray<const char*>          _weakDefsFromChainedBinds;
    uint32_t                                _alreadyInitedIndex    = 0;
    bool                                    _isLaunchClosure       = false;
    bool                                    _makingDyldCacheImages = false;
    bool                                    _dyldCacheIsLive       = false;    // means kernel is rebasing dyld cache content being viewed
    bool                                    _makingClosuresInCache = false;
    bool                                    _makingCustomerCache   = false;
    bool                                    _atPathUsed            = false;
    bool                                    _fallbackPathUsed      = false;
    ImageNum                                _libDyldImageNum       = 0;
    ImageNum                                _libSystemImageNum     = 0;
};





} //  namespace closure
} //  namespace dyld3


#endif /* ClosureBuilder_h */
