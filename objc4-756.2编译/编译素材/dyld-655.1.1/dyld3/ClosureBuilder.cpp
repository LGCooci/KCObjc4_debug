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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
 #include <sys/types.h>
 #include <sys/sysctl.h>

#include "mach-o/dyld_priv.h"

#include "ClosureWriter.h"
#include "ClosureBuilder.h"
#include "MachOAnalyzer.h"
#include "libdyldEntryVector.h"
#include "Tracing.h"

namespace dyld3 {
namespace closure {

const DlopenClosure* ClosureBuilder::sRetryDlopenClosure = (const DlopenClosure*)(-1);

ClosureBuilder::ClosureBuilder(uint32_t startImageNum, const FileSystem& fileSystem, const DyldSharedCache* dyldCache, bool dyldCacheIsLive,
                               const PathOverrides& pathOverrides, AtPath atPathHandling, LaunchErrorInfo* errorInfo,
                               const char* archName, Platform platform,
                               const CacheDylibsBindingHandlers* handlers)
    : _fileSystem(fileSystem), _dyldCache(dyldCache), _pathOverrides(pathOverrides), _archName(archName), _platform(platform), _startImageNum(startImageNum),
      _handlers(handlers), _atPathHandling(atPathHandling), _launchErrorInfo(errorInfo), _dyldCacheIsLive(dyldCacheIsLive)
{
    if ( dyldCache != nullptr ) {
        _dyldImageArray = dyldCache->cachedDylibsImageArray();
        if ( (dyldCache->header.otherImageArrayAddr != 0) && (dyldCache->header.progClosuresSize == 0) )
            _makingClosuresInCache = true;
    }
}


ClosureBuilder::~ClosureBuilder() {
    if ( _tempPaths != nullptr )
        PathPool::deallocate(_tempPaths);
    if ( _mustBeMissingPaths != nullptr )
        PathPool::deallocate(_mustBeMissingPaths);
}

bool ClosureBuilder::findImage(const char* loadPath, const LoadedImageChain& forImageChain, BuilderLoadedImage*& foundImage, bool staticLinkage, bool allowOther)
{
    __block bool result = false;

    _pathOverrides.forEachPathVariant(loadPath, ^(const char* possiblePath, bool isFallbackPath, bool& stop) {
        bool                  unmapWhenDone    = false;
        bool                  contentRebased   = false;
        bool                  hasInits         = false;
        bool                  fileFound        = false;
        bool                  markNeverUnload  = staticLinkage ? forImageChain.image.markNeverUnload : false;
        ImageNum              overrideImageNum = 0;
        ImageNum              foundImageNum    = 0;
        const MachOAnalyzer*  mh               = nullptr;
        const char*           filePath         = nullptr;
        LoadedFileInfo        loadedFileInfo;

        // This check is within forEachPathVariant() to let DYLD_LIBRARY_PATH override LC_RPATH
        bool isRPath = (strncmp(possiblePath, "@rpath/", 7) == 0);

        // passing a leaf name to dlopen() allows rpath searching for it
        bool implictRPath = !staticLinkage && (loadPath[0] != '/') && (loadPath == possiblePath) && (_atPathHandling != AtPath::none);

        // expand @ paths
        const char* prePathVarExpansion = possiblePath;
        possiblePath = resolvePathVar(possiblePath, forImageChain, implictRPath);
        if ( prePathVarExpansion != possiblePath )
            _atPathUsed = true;

        // look at already loaded images
        const char* leafName = strrchr(possiblePath, '/');
        for (BuilderLoadedImage& li: _loadedImages) {
            if ( strcmp(li.path(), possiblePath) == 0 ) {
                foundImage = &li;
                result = true;
                stop = true;
                return;
            }
            else if ( isRPath ) {
                // Special case @rpath/ because name in li.fileInfo.path is full path.
                // Getting installName is expensive, so first see if an already loaded image
                // has same leaf name and if so see if its installName matches request @rpath
                if (const char* aLeaf = strrchr(li.path(), '/')) {
                    if ( strcmp(aLeaf, leafName) == 0 ) {
                        if ( li.loadAddress()->isDylib() && (strcmp(loadPath, li.loadAddress()->installName()) == 0) ) {
                            foundImage = &li;
                            result = true;
                            stop = true;
                            return;
                        }
                    }
                }
            }
        }

        // look to see if image already loaded via a different symlink
        if ( _fileSystem.fileExists(possiblePath, &loadedFileInfo.inode, &loadedFileInfo.mtime) ) {
            fileFound = true;
            for (BuilderLoadedImage& li: _loadedImages) {
                if ( (li.loadedFileInfo.inode == loadedFileInfo.inode) && (li.loadedFileInfo.mtime == loadedFileInfo.mtime) )  {
                    foundImage = &li;
                    result = true;
                    stop = true;
                    return;
                }
            }
        }

        // look in dyld cache
        filePath = possiblePath;
        char realPath[MAXPATHLEN];
        if ( _dyldImageArray != nullptr && (_dyldCache->header.formatVersion == dyld3::closure::kFormatVersion) ) {
            uint32_t dyldCacheImageIndex;
            bool foundInCache =  _dyldCache->hasImagePath(possiblePath, dyldCacheImageIndex);
            if ( !foundInCache && fileFound ) {
                // see if this is an OS dylib/bundle with a pre-built dlopen closure
                if ( allowOther ) {
                    if (const dyld3::closure::Image* otherImage = _dyldCache->findDlopenOtherImage(possiblePath) ) {
                        uint64_t expectedInode;
                        uint64_t expectedModTime;
                        if ( !otherImage->isInvalid()  ) {
                            bool hasInodeInfo = otherImage->hasFileModTimeAndInode(expectedInode, expectedModTime);
                            // use pre-built Image if it does not have mtime/inode or it does and it has matches current file info
                            if ( !hasInodeInfo || ((expectedInode == loadedFileInfo.inode) && (expectedModTime == loadedFileInfo.mtime)) ) {
                                loadedFileInfo = MachOAnalyzer::load(_diag, _fileSystem, possiblePath, _archName, _platform);
                                if ( _diag.noError() ) {
                                    mh = (const MachOAnalyzer*)loadedFileInfo.fileContent;
                                    foundImageNum = otherImage->imageNum();
                                    unmapWhenDone = true;
                                    contentRebased = false;
                                    hasInits = otherImage->hasInitializers() || otherImage->mayHavePlusLoads();
                                }
                            }
                        }
                    }
                }
                // if not found in cache, may be a symlink to something in cache
                if ( mh == nullptr ) {
                    if ( _fileSystem.getRealPath(possiblePath, realPath) ) {
                        foundInCache = _dyldCache->hasImagePath(realPath, dyldCacheImageIndex);
                        if ( foundInCache ) {
                            filePath = realPath;
    #if BUILDING_LIBDYLD
                            // handle case where OS dylib was updated after this process launched
                            if ( foundInCache ) {
                                for (BuilderLoadedImage& li: _loadedImages) {
                                    if ( strcmp(li.path(), realPath) == 0 ) {
                                        foundImage = &li;
                                        result = true;
                                        stop = true;
                                        return;
                                    }
                                }
                            }
    #endif
                        }
                    }
                }
            }

            // if using a cached dylib, look to see if there is an override
            if ( foundInCache ) {
                ImageNum dyldCacheImageNum = dyldCacheImageIndex + 1;
                bool useCache = true;
                markNeverUnload = true; // dylibs in cache, or dylibs that override cache should not be unloaded at runtime
                const Image* image = _dyldImageArray->imageForNum(dyldCacheImageNum);
                if ( image->overridableDylib() ) {
                    if ( fileFound && (_platform == MachOFile::currentPlatform()) ) {
                        uint64_t expectedInode;
                        uint64_t expectedModTime;
                        if ( image->hasFileModTimeAndInode(expectedInode, expectedModTime) ) {
                            // macOS where dylibs remain on disk.  only use cache if mtime and inode have not changed
                            useCache = ( (loadedFileInfo.inode == expectedInode) && (loadedFileInfo.mtime == expectedModTime) );
                        }
                        else if ( _makingClosuresInCache ) {
                            // during iOS cache build, don't look at files on disk, use ones in cache
                            useCache = true;
                        }
                        else {
                            // iOS internal build. Any disk on cache overrides cache
                            useCache = false;
                        }
                    }
                    if ( !useCache )
                        overrideImageNum = dyldCacheImageNum;
                }
                if ( useCache ) {
                    foundImageNum = dyldCacheImageNum;
                    mh = (MachOAnalyzer*)_dyldCache->getIndexedImageEntry(foundImageNum-1, loadedFileInfo.mtime, loadedFileInfo.inode);
                    unmapWhenDone = false;
                    // if we are building ImageArray in dyld cache, content is not rebased
                    contentRebased = !_makingDyldCacheImages && _dyldCacheIsLive;
                    hasInits = image->hasInitializers() || image->mayHavePlusLoads();
                }
            }
        }

        // If we are building the cache, and don't find an image, then it might be weak so just return
        if (_makingDyldCacheImages) {
            addMustBeMissingPath(possiblePath);
            return;
        }

         // if not found yet, mmap file
        if ( mh == nullptr ) {
            loadedFileInfo = MachOAnalyzer::load(_diag, _fileSystem, filePath, _archName, _platform);
            mh = (const MachOAnalyzer*)loadedFileInfo.fileContent;
            if ( mh == nullptr ) {
                // Don't add must be missing paths for dlopen as we don't cache dlopen closures
                if (_isLaunchClosure) {
                    addMustBeMissingPath(possiblePath);
                }
                return;
            }
            if ( staticLinkage ) {
                // LC_LOAD_DYLIB can only link with dylibs
                if ( !mh->isDylib() ) {
                    _diag.error("not a dylib");
                    return;
                }
            }
            else if ( mh->isMainExecutable() ) {
                // when dlopen()ing a main executable, it must be dynamic Position Independent Executable
                if ( !mh->isPIE() || !mh->isDynamicExecutable() ) {
                    _diag.error("not PIE");
                    return;
                }
            }
            foundImageNum = _startImageNum + _nextIndex++;
            unmapWhenDone = true;
        } else {
            loadedFileInfo.fileContent = mh;
        }

        // if path is not original path
        if ( filePath != loadPath ) {
            // possiblePath may be a temporary (stack) string, since we found file at that path, make it permanent
            filePath = strdup_temp(filePath);
            // check if this overrides what would have been found in cache
            if ( overrideImageNum == 0 ) {
                if ( _dyldImageArray != nullptr )  {
                    uint32_t dyldCacheImageIndex;
                    if ( _dyldCache->hasImagePath(loadPath, dyldCacheImageIndex) ) {
                        ImageNum possibleOverrideNum = dyldCacheImageIndex+1;
                        if ( possibleOverrideNum != foundImageNum )
                            overrideImageNum = possibleOverrideNum;
                    }
                }
            }
        }

        if ( !markNeverUnload ) {
            // If the parent didn't force us to be never unload, other conditions still may
            if ( mh->hasThreadLocalVariables() ) {
                markNeverUnload = true;
            } else if ( mh->hasObjC() && mh->isDylib() ) {
                markNeverUnload = true;
            } else {
                // record if image has DOF sections
                __block bool hasDOFs = false;
                mh->forEachDOFSection(_diag, ^(uint32_t offset) {
                    hasDOFs = true;
                });
                if ( hasDOFs )
                    markNeverUnload = true;
            }
        }

        // Set the path again just in case it was strdup'ed.
        loadedFileInfo.path = filePath;

        // add new entry
        BuilderLoadedImage entry;
        entry.loadedFileInfo   = loadedFileInfo;
        entry.imageNum         = foundImageNum;
        entry.unmapWhenDone    = unmapWhenDone;
        entry.contentRebased   = contentRebased;
        entry.hasInits         = hasInits;
        entry.markNeverUnload  = markNeverUnload;
        entry.rtldLocal        = false;
        entry.isBadImage       = false;
        entry.overrideImageNum = overrideImageNum;
        _loadedImages.push_back(entry);
        foundImage = &_loadedImages.back();
        if ( isFallbackPath )
            _fallbackPathUsed = true;
        stop = true;
        result = true;
    }, _platform);

    return result;
}

bool ClosureBuilder::expandAtLoaderPath(const char* loadPath, bool fromLCRPATH, const BuilderLoadedImage& loadedImage, char fixedPath[])
{
    switch ( _atPathHandling ) {
        case AtPath::none:
            return false;
        case AtPath::onlyInRPaths:
            if ( !fromLCRPATH ) {
                // <rdar://42360708> allow @loader_path in LC_LOAD_DYLIB during dlopen()
                if ( _isLaunchClosure )
                    return false;
            }
            break;
        case AtPath::all:
            break;
    }
    if ( strncmp(loadPath, "@loader_path/", 13) != 0 )
        return false;

    strlcpy(fixedPath, loadedImage.path(), PATH_MAX);
    char* lastSlash = strrchr(fixedPath, '/');
    if ( lastSlash != nullptr ) {
        strcpy(lastSlash+1, &loadPath[13]);
        return true;
    }
    return false;
}

bool ClosureBuilder::expandAtExecutablePath(const char* loadPath, bool fromLCRPATH, char fixedPath[])
{
    switch ( _atPathHandling ) {
        case AtPath::none:
            return false;
        case AtPath::onlyInRPaths:
            if ( !fromLCRPATH )
                return false;
            break;
        case AtPath::all:
            break;
    }
    if ( strncmp(loadPath, "@executable_path/", 17) != 0 )
        return false;

    if ( _atPathHandling != AtPath::all )
        return false;

    strlcpy(fixedPath, _loadedImages[_mainProgLoadIndex].path(), PATH_MAX);
    char* lastSlash = strrchr(fixedPath, '/');
    if ( lastSlash != nullptr ) {
        strcpy(lastSlash+1, &loadPath[17]);
        return true;
    }
    return false;
}

const char* ClosureBuilder::resolvePathVar(const char* loadPath, const LoadedImageChain& forImageChain, bool implictRPath)
{
    // don't expand @ path if disallowed
    if ( (_atPathHandling == AtPath::none) && (loadPath[0] == '@') )
        return loadPath;

    // quick out if not @ path or not implicit rpath
    if ( !implictRPath && (loadPath[0] != '@') )
        return loadPath;

    // expand @loader_path
    BLOCK_ACCCESSIBLE_ARRAY(char, tempPath, PATH_MAX);  // read as:  char tempPath[PATH_MAX];
    if ( expandAtLoaderPath(loadPath, false, forImageChain.image, tempPath) )
        return strdup_temp(tempPath);

    // expand @executable_path
    if ( expandAtExecutablePath(loadPath, false, tempPath) )
        return strdup_temp(tempPath);

    // expand @rpath
    const char* rpathTail = nullptr;
    char implicitRpathBuffer[PATH_MAX];
    if ( strncmp(loadPath, "@rpath/", 7) == 0 ) {
        // note: rpathTail starts with '/'
        rpathTail = &loadPath[6];
    }
    else if ( implictRPath ) {
        // make rpathTail starts with '/'
        strlcpy(implicitRpathBuffer, "/", PATH_MAX);
        strlcat(implicitRpathBuffer, loadPath, PATH_MAX);
        rpathTail = implicitRpathBuffer;
    }
    if ( rpathTail != nullptr ) {
        // rpath is expansion is technically a stack of rpath dirs built starting with main executable and pushing
        // LC_RPATHS from each dylib as they are recursively loaded.  Our imageChain represents that stack.
        __block const char* result = nullptr;
        for (const LoadedImageChain* link = &forImageChain; (link != nullptr) && (result == nullptr); link = link->previous) {
            link->image.loadAddress()->forEachRPath(^(const char* rPath, bool& stop) {
                // fprintf(stderr, "LC_RPATH %s from %s\n", rPath, link->image.fileInfo.path);
                if ( expandAtLoaderPath(rPath, true, link->image, tempPath) || expandAtExecutablePath(rPath, true, tempPath) ) {
                    strlcat(tempPath, rpathTail, PATH_MAX);
                }
                else {
                    strlcpy(tempPath, rPath, PATH_MAX);
                    strlcat(tempPath, rpathTail, PATH_MAX);
                }
                if ( _fileSystem.fileExists(tempPath) ) {
                    stop = true;
                    result = strdup_temp(tempPath);
                }
                else {
                    // Don't add must be missing paths for dlopen as we don't cache dlopen closures
                    if (_isLaunchClosure) {
                        addMustBeMissingPath(tempPath);
                    }
                }
            });
        }
        if ( result != nullptr )
            return result;
    }

    return loadPath;
}

const char* ClosureBuilder::strdup_temp(const char* path)
{
    if ( _tempPaths == nullptr )
        _tempPaths = PathPool::allocate();
    return _tempPaths->add(path);
}

void ClosureBuilder::addMustBeMissingPath(const char* path)
{
    //fprintf(stderr, "must be missing: %s\n", path);
    if ( _mustBeMissingPaths == nullptr )
        _mustBeMissingPaths = PathPool::allocate();
    _mustBeMissingPaths->add(path);
}

ClosureBuilder::BuilderLoadedImage& ClosureBuilder::findLoadedImage(ImageNum imageNum)
{
    for (BuilderLoadedImage& li : _loadedImages) {
        if ( li.imageNum == imageNum ) {
            return li;
        }
    }
    for (BuilderLoadedImage& li : _loadedImages) {
        if ( li.overrideImageNum == imageNum ) {
            return li;
        }
    }
    assert(0 && "LoadedImage not found");
}

ClosureBuilder::BuilderLoadedImage& ClosureBuilder::findLoadedImage(const MachOAnalyzer* mh)
{
    for (BuilderLoadedImage& li : _loadedImages) {
        if ( li.loadAddress() == mh ) {
             return li;
        }
    }
    assert(0 && "LoadedImage not found");
}

const MachOAnalyzer* ClosureBuilder::machOForImageNum(ImageNum imageNum)
{
    return findLoadedImage(imageNum).loadAddress();
}

const MachOAnalyzer* ClosureBuilder::findDependent(const MachOLoaded* mh, uint32_t depIndex)
{
    for (const BuilderLoadedImage& li : _loadedImages) {
        if ( li.loadAddress() == mh ) {
            if (li.isBadImage) {
                // Bad image duting building group 1 closures, so the dependents array
                // is potentially incomplete.
                return nullptr;
            }
            ImageNum childNum = li.dependents[depIndex].imageNum();
            return machOForImageNum(childNum);
        }
    }
    return nullptr;
}

ImageNum ClosureBuilder::imageNumForMachO(const MachOAnalyzer* mh)
{
    for (const BuilderLoadedImage& li : _loadedImages) {
        if ( li.loadAddress() == mh ) {
             return li.imageNum;
        }
    }
    assert(0 && "unknown mach-o");
    return 0;
}

void ClosureBuilder::recursiveLoadDependents(LoadedImageChain& forImageChain)
{
    // if dependents is set, then we have already loaded this
    if ( forImageChain.image.dependents.begin() != nullptr )
        return;

    uintptr_t startDepIndex = _dependencies.count();
    // add dependents
    __block uint32_t depIndex = 0;
    forImageChain.image.loadAddress()->forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool &stop) {
        Image::LinkKind kind = Image::LinkKind::regular;
        if ( isWeak )
            kind = Image::LinkKind::weak;
        else if ( isReExport )
            kind = Image::LinkKind::reExport;
        else if ( isUpward )
            kind = Image::LinkKind::upward;
        BuilderLoadedImage* foundImage;
        if ( findImage(loadPath, forImageChain, foundImage, true, false) ) {
            // verify this is compatable dylib version
            if ( foundImage->loadAddress()->filetype != MH_DYLIB ) {
                _diag.error("found '%s' which is not a dylib.  Needed by '%s'", foundImage->path(), forImageChain.image.path());
            }
            else {
                const char* installName;
                uint32_t    foundCompatVers;
                uint32_t    foundCurrentVers;
                foundImage->loadAddress()->getDylibInstallName(&installName, &foundCompatVers, &foundCurrentVers);
                if ( (foundCompatVers < compatVersion) && foundImage->loadAddress()->enforceCompatVersion() ) {
                    char foundStr[32];
                    char requiredStr[32];
                    MachOFile::packedVersionToString(foundCompatVers, foundStr);
                    MachOFile::packedVersionToString(compatVersion, requiredStr);
                    _diag.error("found '%s' which has compat version (%s) which is less than required (%s).  Needed by '%s'",
                                foundImage->path(), foundStr, requiredStr, forImageChain.image.path());
                }
            }
            if ( _diag.noError() )
                _dependencies.push_back(Image::LinkedImage(kind, foundImage->imageNum));
        }
        else if ( isWeak ) {
            _dependencies.push_back(Image::LinkedImage(Image::LinkKind::weak, kMissingWeakLinkedImage));
        }
        else {
            BLOCK_ACCCESSIBLE_ARRAY(char, extra, 4096);
            extra[0] = '\0';
            const char* targetLeaf = strrchr(loadPath, '/');
            if ( targetLeaf == nullptr )
                targetLeaf = loadPath;
            if ( _mustBeMissingPaths != nullptr ) {
                strcpy(extra, ", tried: ");
                _mustBeMissingPaths->forEachPath(^(const char* aPath) {
                    const char* aLeaf = strrchr(aPath, '/');
                    if ( aLeaf == nullptr )
                        aLeaf = aPath;
                  if ( strcmp(targetLeaf, aLeaf) == 0 ) {
                        strlcat(extra, "'", 4096);
                        strlcat(extra, aPath, 4096);
                        strlcat(extra, "' ", 4096);
                    }
                });
            }
            if ( _diag.hasError() ) {
        #if BUILDING_CACHE_BUILDER
                std::string errorMessageBuffer = _diag.errorMessage();
                const char* msg = errorMessageBuffer.c_str();
        #else
                const char* msg = _diag.errorMessage();
        #endif
                char msgCopy[strlen(msg)+4];
                strcpy(msgCopy, msg);
                _diag.error("dependent dylib '%s' not found for '%s'. %s", loadPath, forImageChain.image.path(), msgCopy);
            }
            else {
                _diag.error("dependent dylib '%s' not found for '%s'%s", loadPath, forImageChain.image.path(), extra);
            }
            if ( _launchErrorInfo != nullptr ) {
                _launchErrorInfo->kind              = DYLD_EXIT_REASON_DYLIB_MISSING;
                _launchErrorInfo->clientOfDylibPath = forImageChain.image.path();
                _launchErrorInfo->targetDylibPath   = loadPath;
                _launchErrorInfo->symbol            = nullptr;
           }
        }
        ++depIndex;
        if ( _diag.hasError() )
            stop = true;
    });
    if ( _diag.hasError() )
        return;
    forImageChain.image.dependents = _dependencies.subArray(startDepIndex, depIndex);

    // breadth first recurse
    for (Image::LinkedImage dep : forImageChain.image.dependents) {
        // don't recurse upwards
        if ( dep.kind() == Image::LinkKind::upward )
            continue;
        // don't recurse down missing weak links
        if ( (dep.kind() == Image::LinkKind::weak) && (dep.imageNum() == kMissingWeakLinkedImage) )
            continue;
        BuilderLoadedImage& depLoadedImage = findLoadedImage(dep.imageNum());
        LoadedImageChain chain = { &forImageChain, depLoadedImage };
        recursiveLoadDependents(chain);
        if ( _diag.hasError() )
            break;
    }
}

void ClosureBuilder::loadDanglingUpwardLinks()
{
    bool danglingFixed;
    do {
        danglingFixed = false;
        for (BuilderLoadedImage& li : _loadedImages) {
            if ( li.dependents.begin() == nullptr ) {
                // this image has not have dependents set (probably a dangling upward link or referenced by upward link)
                LoadedImageChain chain = { nullptr, li };
                recursiveLoadDependents(chain);
                danglingFixed = true;
                break;
            }
        }
    } while (danglingFixed && _diag.noError());
}

bool ClosureBuilder::overridableDylib(const BuilderLoadedImage& forImage)
{
    // only set on dylibs in the dyld shared cache
    if ( !_makingDyldCacheImages )
        return false;

    // on macOS dylibs always override cache
    if ( _platform == Platform::macOS )
        return true;

    // on embedded platforms with Internal cache, allow overrides
    if ( !_makingCustomerCache )
        return true;

    // embedded platform customer caches, no overrides
    return false;  // FIXME, allow libdispatch.dylib to be overridden
}

void ClosureBuilder::buildImage(ImageWriter& writer, BuilderLoadedImage& forImage)
{
    const MachOAnalyzer* macho = forImage.loadAddress();
	// set ImageNum
    writer.setImageNum(forImage.imageNum);

    // set flags
    writer.setHasWeakDefs(macho->hasWeakDefs());
    writer.setIsBundle(macho->isBundle());
    writer.setIsDylib(macho->isDylib());
    writer.setIs64(macho->is64());
    writer.setIsExecutable(macho->isMainExecutable());
    writer.setUses16KPages(macho->uses16KPages());
    writer.setOverridableDylib(overridableDylib(forImage));
    writer.setInDyldCache(macho->inDyldCache());
    if ( macho->hasObjC() ) {
        writer.setHasObjC(true);
        bool hasPlusLoads = macho->hasPlusLoadMethod(_diag);
        writer.setHasPlusLoads(hasPlusLoads);
        if ( hasPlusLoads )
            forImage.hasInits = true;
    }
    else {
        writer.setHasObjC(false);
        writer.setHasPlusLoads(false);
    }

    if ( forImage.markNeverUnload ) {
        writer.setNeverUnload(true);
    }

#if BUILDING_DYLD || BUILDING_LIBDYLD
    // shared cache not built by dyld or libdyld.dylib, so must be real file
    writer.setFileInfo(forImage.loadedFileInfo.inode, forImage.loadedFileInfo.mtime);
#else
    if ( _platform == Platform::macOS ) {
        if ( macho->inDyldCache() && !_dyldCache->header.dylibsExpectedOnDisk ) {
            // don't add file info for shared cache files mastered out of final file system
        }
        else {
            // file is either not in cache or is in cache but not mastered out
            writer.setFileInfo(forImage.loadedFileInfo.inode, forImage.loadedFileInfo.mtime);
        }
    }
    else {
        // all other platforms, cache is built off-device, so inodes are not known
    }
#endif

    // add info on how to load image
    if ( !macho->inDyldCache() ) {
        writer.setMappingInfo(forImage.loadedFileInfo.sliceOffset, macho->mappedSize());
        // add code signature, if signed
        uint32_t codeSigFileOffset;
        uint32_t codeSigSize;
        if ( macho->hasCodeSignature(codeSigFileOffset, codeSigSize) ) {
            writer.setCodeSignatureLocation(codeSigFileOffset, codeSigSize);
            uint8_t cdHash[20];
            if ( macho->getCDHash(cdHash) )
                writer.setCDHash(cdHash);
        }
        // add FairPlay encryption range if encrypted
        uint32_t fairPlayFileOffset;
        uint32_t fairPlaySize;
        if ( macho->isFairPlayEncrypted(fairPlayFileOffset, fairPlaySize) ) {
            writer.setFairPlayEncryptionRange(fairPlayFileOffset, fairPlaySize);
        }
    }

    // set path
    writer.addPath(forImage.path());
    if ( _aliases != nullptr ) {
        for (const CachedDylibAlias& alias : *_aliases) {
            if ( strcmp(alias.realPath, forImage.path()) == 0 )
                writer.addPath(alias.aliasPath);
        }
    }

    // set uuid, if has one
    uuid_t uuid;
    if ( macho->getUuid(uuid) )
        writer.setUUID(uuid);

    // set dependents
    writer.setDependents(forImage.dependents);

    // set segments
    addSegments(writer, macho);

    // record if this dylib overrides something in the cache
    if ( forImage.overrideImageNum != 0 ) {
        writer.setAsOverrideOf(forImage.overrideImageNum);
        const char* overridePath = _dyldImageArray->imageForNum(forImage.overrideImageNum)->path();
        writer.addPath(overridePath);
        if ( strcmp(overridePath, "/usr/lib/system/libdyld.dylib") == 0 )
            _libDyldImageNum = forImage.imageNum;
        else if ( strcmp(overridePath, "/usr/lib/libSystem.B.dylib") == 0 )
            _libSystemImageNum = forImage.imageNum;
    }


    // do fix up info for non-cached, and cached if building cache
    if ( !macho->inDyldCache() || _makingDyldCacheImages ) {
        if ( macho->hasChainedFixups() ) {
            addChainedFixupInfo(writer, forImage);
        }
        else {
            if ( _handlers != nullptr ) {
                reportRebasesAndBinds(writer, forImage);
            }
            else {
                addRebaseInfo(writer, macho);
                if ( _diag.noError() )
                    addBindInfo(writer, forImage);
            }
        }
    }
    if ( _diag.hasError() ) {
        writer.setInvalid();
        return;
    }

    // add initializers
    bool contentRebased = forImage.contentRebased;
    __block unsigned initCount = 0;
    macho->forEachInitializer(_diag, contentRebased, ^(uint32_t offset) {
        ++initCount;
    }, _dyldCache);
    if ( initCount != 0 ) {
        BLOCK_ACCCESSIBLE_ARRAY(uint32_t, initOffsets, initCount);
        __block unsigned index = 0;
       macho->forEachInitializer(_diag, contentRebased, ^(uint32_t offset) {
            initOffsets[index++] = offset;
        }, _dyldCache);
        writer.setInitOffsets(initOffsets, initCount);
        forImage.hasInits = true;
    }

    // record if image has DOF sections
    STACK_ALLOC_ARRAY(uint32_t, dofSectionOffsets, 256);
    macho->forEachDOFSection(_diag, ^(uint32_t offset) {
        dofSectionOffsets.push_back(offset);
    });
    if ( !dofSectionOffsets.empty() ) {
        writer.setDofOffsets(dofSectionOffsets);
    }

}

void ClosureBuilder::addSegments(ImageWriter& writer, const MachOAnalyzer* mh)
{
    const uint32_t segCount = mh->segmentCount();
    if ( mh->inDyldCache() ) {
        uint64_t cacheUnslideBaseAddress = _dyldCache->unslidLoadAddress();
        BLOCK_ACCCESSIBLE_ARRAY(Image::DyldCacheSegment, segs, segCount);
        mh->forEachSegment(^(const MachOAnalyzer::SegmentInfo& info, bool& stop) {
            segs[info.segIndex] = { (uint32_t)(info.vmAddr-cacheUnslideBaseAddress), (uint32_t)info.vmSize, info.protections };
        });
        writer.setCachedSegments(segs, segCount);
    }
    else {
        const uint32_t   pageSize          = (mh->uses16KPages() ? 0x4000 : 0x1000);
        __block uint32_t diskSegIndex      = 0;
        __block uint32_t totalPageCount    = 0;
        __block uint32_t lastFileOffsetEnd = 0;
        __block uint64_t lastVmAddrEnd     = 0;
        BLOCK_ACCCESSIBLE_ARRAY(Image::DiskSegment, dsegs, segCount*3); // room for padding
        mh->forEachSegment(^(const MachOAnalyzer::SegmentInfo& info, bool& stop) {
            if ( (info.fileOffset != 0) && (info.fileOffset != lastFileOffsetEnd) ) {
                Image::DiskSegment filePadding;
                filePadding.filePageCount   = (info.fileOffset - lastFileOffsetEnd)/pageSize;
                filePadding.vmPageCount     = 0;
                filePadding.permissions     = 0;
                filePadding.paddingNotSeg   = 1;
                dsegs[diskSegIndex++] = filePadding;
            }
            if ( (lastVmAddrEnd != 0) && (info.vmAddr != lastVmAddrEnd) ) {
                Image::DiskSegment vmPadding;
                vmPadding.filePageCount   = 0;
                vmPadding.vmPageCount     = (info.vmAddr - lastVmAddrEnd)/pageSize;
                vmPadding.permissions     = 0;
                vmPadding.paddingNotSeg   = 1;
                dsegs[diskSegIndex++] = vmPadding;
                totalPageCount += vmPadding.vmPageCount;
            }
            {
                Image::DiskSegment segInfo;
                segInfo.filePageCount   = (info.fileSize+pageSize-1)/pageSize;
                segInfo.vmPageCount     = (info.vmSize+pageSize-1)/pageSize;
                segInfo.permissions     = info.protections & 7;
                segInfo.paddingNotSeg   = 0;
                dsegs[diskSegIndex++] = segInfo;
                totalPageCount   += segInfo.vmPageCount;
                if ( info.fileSize != 0 )
                    lastFileOffsetEnd = (uint32_t)(info.fileOffset + info.fileSize);
                if ( info.vmSize != 0 )
                    lastVmAddrEnd     = info.vmAddr + info.vmSize;
            }
        });
        writer.setDiskSegments(dsegs, diskSegIndex);
    }
}

void ClosureBuilder::addInterposingTuples(LaunchClosureWriter& writer, const Image* image, const MachOAnalyzer* mh)
{
    const unsigned pointerSize  = mh->pointerSize();
    mh->forEachInterposingSection(_diag, ^(uint64_t sectVmOffset, uint64_t sectVmSize, bool &stop) {
        const uint32_t entrySize = 2*pointerSize;
        const uint32_t tupleCount = (uint32_t)(sectVmSize/entrySize);
        BLOCK_ACCCESSIBLE_ARRAY(InterposingTuple, resolvedTuples, tupleCount);
        for (uint32_t i=0; i < tupleCount; ++i) {
            resolvedTuples[i].stockImplementation.absolute.kind  = Image::ResolvedSymbolTarget::kindAbsolute;
            resolvedTuples[i].stockImplementation.absolute.value = 0;
            resolvedTuples[i].newImplementation.absolute.kind    = Image::ResolvedSymbolTarget::kindAbsolute;
            resolvedTuples[i].newImplementation.absolute.value   = 0;
        }
        image->forEachFixup(^(uint64_t imageOffsetToRebase, bool &rebaseStop) {
            if ( imageOffsetToRebase < sectVmOffset )
                return;
            if ( imageOffsetToRebase > sectVmOffset+sectVmSize )
                return;
            uint64_t offsetIntoSection = imageOffsetToRebase - sectVmOffset;
            uint64_t rebaseIndex = offsetIntoSection/entrySize;
            if ( rebaseIndex*entrySize != offsetIntoSection )
                return;
            const void* content = (uint8_t*)mh + imageOffsetToRebase;
            uint64_t unslidTargetAddress = mh->is64() ?  *(uint64_t*)content : *(uint32_t*)content;
            resolvedTuples[rebaseIndex].newImplementation.image.kind     = Image::ResolvedSymbolTarget::kindImage;
            resolvedTuples[rebaseIndex].newImplementation.image.imageNum = image->imageNum();
            resolvedTuples[rebaseIndex].newImplementation.image.offset   = unslidTargetAddress - mh->preferredLoadAddress();
        }, ^(uint64_t imageOffsetToBind, Image::ResolvedSymbolTarget bindTarget, bool &bindStop) {
           if ( imageOffsetToBind < sectVmOffset )
                return;
            if ( imageOffsetToBind > sectVmOffset+sectVmSize )
                return;
            uint64_t offsetIntoSection = imageOffsetToBind - sectVmOffset;
            uint64_t bindIndex = offsetIntoSection/entrySize;
            if ( bindIndex*entrySize + pointerSize != offsetIntoSection )
                return;
            resolvedTuples[bindIndex].stockImplementation = bindTarget;
        }, ^(uint64_t imageOffsetStart, const Array<Image::ResolvedSymbolTarget>& targets, bool& chainStop) {
            // walk each fixup in the chain
            image->forEachChainedFixup((void*)mh, imageOffsetStart, ^(uint64_t* fixupLoc, MachOLoaded::ChainedFixupPointerOnDisk fixupInfo, bool& stopChain) {
                uint64_t imageOffsetToFixup = (uint64_t)fixupLoc - (uint64_t)mh;
                if ( fixupInfo.authRebase.auth ) {
#if SUPPORT_ARCH_arm64e
                    if ( fixupInfo.authBind.bind ) {
                        closure::Image::ResolvedSymbolTarget bindTarget = targets[fixupInfo.authBind.ordinal];
                        if ( imageOffsetToFixup < sectVmOffset )
                            return;
                        if ( imageOffsetToFixup > sectVmOffset+sectVmSize )
                            return;
                        uint64_t offsetIntoSection = imageOffsetToFixup - sectVmOffset;
                        uint64_t bindIndex = offsetIntoSection/entrySize;
                        if ( bindIndex*entrySize + pointerSize != offsetIntoSection )
                            return;
                        resolvedTuples[bindIndex].stockImplementation = bindTarget;
                    }
                    else {
                        if ( imageOffsetToFixup < sectVmOffset )
                            return;
                        if ( imageOffsetToFixup > sectVmOffset+sectVmSize )
                            return;
                        uint64_t offsetIntoSection = imageOffsetToFixup - sectVmOffset;
                        uint64_t rebaseIndex = offsetIntoSection/entrySize;
                        if ( rebaseIndex*entrySize != offsetIntoSection )
                            return;
                        uint64_t unslidTargetAddress = (uint64_t)mh->preferredLoadAddress() + fixupInfo.authRebase.target;
                        resolvedTuples[rebaseIndex].newImplementation.image.kind     = Image::ResolvedSymbolTarget::kindImage;
                        resolvedTuples[rebaseIndex].newImplementation.image.imageNum = image->imageNum();
                        resolvedTuples[rebaseIndex].newImplementation.image.offset   = unslidTargetAddress - mh->preferredLoadAddress();
                    }
#else
                    _diag.error("malformed chained pointer");
                    stop = true;
                    stopChain = true;
#endif
                }
                else {
                    if ( fixupInfo.plainRebase.bind ) {
                        closure::Image::ResolvedSymbolTarget bindTarget = targets[fixupInfo.plainBind.ordinal];
                        if ( imageOffsetToFixup < sectVmOffset )
                            return;
                        if ( imageOffsetToFixup > sectVmOffset+sectVmSize )
                            return;
                        uint64_t offsetIntoSection = imageOffsetToFixup - sectVmOffset;
                        uint64_t bindIndex = offsetIntoSection/entrySize;
                        if ( bindIndex*entrySize + pointerSize != offsetIntoSection )
                            return;
                        resolvedTuples[bindIndex].stockImplementation = bindTarget;
                    }
                    else {
                        if ( imageOffsetToFixup < sectVmOffset )
                            return;
                        if ( imageOffsetToFixup > sectVmOffset+sectVmSize )
                            return;
                        uint64_t offsetIntoSection = imageOffsetToFixup - sectVmOffset;
                        uint64_t rebaseIndex = offsetIntoSection/entrySize;
                        if ( rebaseIndex*entrySize != offsetIntoSection )
                            return;
                        uint64_t unslidTargetAddress = fixupInfo.plainRebase.signExtendedTarget();
                        resolvedTuples[rebaseIndex].newImplementation.image.kind     = Image::ResolvedSymbolTarget::kindImage;
                        resolvedTuples[rebaseIndex].newImplementation.image.imageNum = image->imageNum();
                        resolvedTuples[rebaseIndex].newImplementation.image.offset   = unslidTargetAddress - mh->preferredLoadAddress();
                    }
                }
            });
        });

        // remove any tuples in which both sides are not set (or target is weak-import NULL)
        STACK_ALLOC_ARRAY(InterposingTuple, goodTuples, tupleCount);
        for (uint32_t i=0; i < tupleCount; ++i) {
            if ( (resolvedTuples[i].stockImplementation.image.kind != Image::ResolvedSymbolTarget::kindAbsolute)
              && (resolvedTuples[i].newImplementation.image.kind != Image::ResolvedSymbolTarget::kindAbsolute) )
                goodTuples.push_back(resolvedTuples[i]);
        }
        writer.addInterposingTuples(goodTuples);

        // if the target of the interposing is in the dyld shared cache, add a PatchEntry so the cache is fixed up at launch
        STACK_ALLOC_ARRAY(Closure::PatchEntry, patches, goodTuples.count());
        for (const InterposingTuple& aTuple : goodTuples) {
            if ( aTuple.stockImplementation.sharedCache.kind == Image::ResolvedSymbolTarget::kindSharedCache ) {
                uint32_t imageIndex;
                assert(_dyldCache->addressInText((uint32_t)aTuple.stockImplementation.sharedCache.offset, &imageIndex));
                ImageNum imageInCache = imageIndex+1;
                Closure::PatchEntry patch;
                patch.exportCacheOffset      = (uint32_t)aTuple.stockImplementation.sharedCache.offset;
                patch.overriddenDylibInCache = imageInCache;
                patch.replacement            = aTuple.newImplementation;
                patches.push_back(patch);
            }
        }
        writer.addCachePatches(patches);
    });
}

void ClosureBuilder::addRebaseInfo(ImageWriter& writer, const MachOAnalyzer* mh)
{
	const uint64_t ptrSize = mh->pointerSize();
    Image::RebasePattern maxLeapPattern = { 0xFFFFF, 0, 0xF };
    const uint64_t maxLeapCount = maxLeapPattern.repeatCount * maxLeapPattern.skipCount;
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(Image::RebasePattern, rebaseEntries, 1024);
    __block uint64_t lastLocation = -ptrSize;
	mh->forEachRebase(_diag, true, ^(uint64_t runtimeOffset, bool& stop) {
        const uint64_t delta   = runtimeOffset - lastLocation;
        const bool     aligned = ((delta % ptrSize) == 0);
        if ( delta == ptrSize ) {
            // this rebase location is contiguous to previous
            if ( rebaseEntries.back().contigCount < 255 ) {
                // just bump previous's contigCount
                rebaseEntries.back().contigCount++;
            }
            else {
                // previous contiguous run already has max 255, so start a new run
                rebaseEntries.push_back({ 1, 1, 0 });
            }
        }
        else if ( aligned && (delta <= (ptrSize*15)) ) {
            // this rebase is within skip distance of last rebase
            rebaseEntries.back().skipCount = (uint8_t)((delta-ptrSize)/ptrSize);
            int lastIndex = (int)(rebaseEntries.count() - 1);
            if ( lastIndex > 1 ) {
                if ( (rebaseEntries[lastIndex].contigCount == rebaseEntries[lastIndex-1].contigCount)
                  && (rebaseEntries[lastIndex].skipCount   == rebaseEntries[lastIndex-1].skipCount) ) {
                    // this entry as same contig and skip as prev, so remove it and bump repeat count of previous
                    rebaseEntries.pop_back();
                    rebaseEntries.back().repeatCount += 1;
                }
            }
            rebaseEntries.push_back({ 1, 1, 0 });
        }
        else {
            uint64_t advanceCount = (delta-ptrSize);
            if ( (runtimeOffset < lastLocation) && (lastLocation != -ptrSize) ) {
                // out of rebases! handle this be resting rebase offset to zero
                rebaseEntries.push_back({ 0, 0, 0 });
                advanceCount = runtimeOffset;
            }
            // if next rebase is too far to reach with one pattern, use series
            while ( advanceCount > maxLeapCount ) {
                rebaseEntries.push_back(maxLeapPattern);
                advanceCount -= maxLeapCount;
            }
            // if next rebase is not reachable with skipCount==1 or skipCount==15, add intermediate
            while ( advanceCount > maxLeapPattern.repeatCount ) {
                uint64_t count = advanceCount / maxLeapPattern.skipCount;
                rebaseEntries.push_back({ (uint32_t)count, 0, maxLeapPattern.skipCount });
                advanceCount -= (count*maxLeapPattern.skipCount);
            }
            if ( advanceCount != 0 )
                rebaseEntries.push_back({ (uint32_t)advanceCount, 0, 1 });
            rebaseEntries.push_back({ 1, 1, 0 });
        }
        lastLocation = runtimeOffset;
	});
    writer.setRebaseInfo(rebaseEntries);

    // i386 programs also use text relocs to rebase stubs
    if ( mh->cputype == CPU_TYPE_I386 ) {
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(Image::TextFixupPattern, textRebases, 512);
        __block uint64_t lastOffset = -4;
        mh->forEachTextRebase(_diag, ^(uint64_t runtimeOffset, bool& stop) {
            if ( textRebases.freeCount() < 2 ) {
                _diag.error("too many text rebase locations (%ld) in %s", textRebases.maxCount(), writer.currentImage()->path());
                stop = true;
            }
            bool mergedIntoPrevious = false;
            if ( (runtimeOffset > lastOffset) && !textRebases.empty() ) {
                uint32_t skipAmount = (uint32_t)(runtimeOffset - lastOffset);
                if ( (textRebases.back().repeatCount == 1) && (textRebases.back().skipCount == 0) ) {
                    textRebases.back().repeatCount = 2;
                    textRebases.back().skipCount   = skipAmount;
                    mergedIntoPrevious             = true;
                }
                else if ( textRebases.back().skipCount == skipAmount ) {
                    textRebases.back().repeatCount += 1;
                    mergedIntoPrevious = true;
                }
            }
            if ( !mergedIntoPrevious ) {
                Image::TextFixupPattern pattern;
                pattern.target.raw    = 0;
                pattern.startVmOffset = (uint32_t)runtimeOffset;
                pattern.repeatCount   = 1;
                pattern.skipCount     = 0;
                textRebases.push_back(pattern);
            }
            lastOffset = runtimeOffset;
        });
        writer.setTextRebaseInfo(textRebases);
    }
}


void ClosureBuilder::forEachBind(BuilderLoadedImage& forImage, void (^handler)(uint64_t runtimeOffset, Image::ResolvedSymbolTarget target, const ResolvedTargetInfo& targetInfo, bool& stop),
                                                               void (^strongHandler)(const char* strongSymbolName))
{
    __block int                         lastLibOrdinal  = 256;
    __block const char*                 lastSymbolName  = nullptr;
    __block uint64_t                    lastAddend      = 0;
    __block Image::ResolvedSymbolTarget target;
    __block ResolvedTargetInfo          targetInfo;
    forImage.loadAddress()->forEachBind(_diag, ^(uint64_t runtimeOffset, int libOrdinal, const char* symbolName, bool weakImport, uint64_t addend, bool& stop) {
        if ( (symbolName == lastSymbolName) && (libOrdinal == lastLibOrdinal) && (addend == lastAddend) ) {
            // same symbol lookup as last location
            handler(runtimeOffset, target, targetInfo, stop);
        }
        else if ( findSymbol(forImage, libOrdinal, symbolName, weakImport, addend, target, targetInfo) ) {
            handler(runtimeOffset, target, targetInfo, stop);
            lastSymbolName = symbolName;
            lastLibOrdinal = libOrdinal;
            lastAddend     = addend;
        }
        else {
            stop = true;
        }
    }, ^(const char* symbolName) {
        strongHandler(symbolName);
    });
}

void ClosureBuilder::addBindInfo(ImageWriter& writer, BuilderLoadedImage& forImage)
{
    const uint32_t ptrSize = forImage.loadAddress()->pointerSize();
	STACK_ALLOC_OVERFLOW_SAFE_ARRAY(Image::BindPattern, binds, 512);
    __block uint64_t                    lastOffset = -ptrSize;
	__block Image::ResolvedSymbolTarget lastTarget = { {0, 0} };
    forEachBind(forImage, ^(uint64_t runtimeOffset, Image::ResolvedSymbolTarget target, const ResolvedTargetInfo& targetInfo, bool& stop) {
        if ( targetInfo.weakBindCoalese )  {
            // may be previous bind to this location
            // if so, update that rather create new BindPattern
            for (Image::BindPattern& aBind : binds) {
                if ( (aBind.startVmOffset == runtimeOffset) && (aBind.repeatCount == 1)  && (aBind.skipCount == 0) ) {
                    aBind.target = target;
                    return;
                }
            }
        }
        bool mergedIntoPrevious = false;
        if ( !mergedIntoPrevious && (target == lastTarget) && (runtimeOffset > lastOffset) && !binds.empty() ) {
            uint64_t skipAmount = (runtimeOffset - lastOffset - ptrSize)/ptrSize;
            if ( skipAmount*ptrSize != (runtimeOffset - lastOffset - ptrSize) ) {
                // misaligned pointer means we cannot optimize 
            }
            else {
                if ( (binds.back().repeatCount == 1) && (binds.back().skipCount == 0) && (skipAmount <= 255) ) {
                    binds.back().repeatCount = 2;
                    binds.back().skipCount   = skipAmount;
                    assert(binds.back().skipCount == skipAmount); // check overflow
                    mergedIntoPrevious       = true;
                }
                else if ( (binds.back().skipCount == skipAmount) && (binds.back().repeatCount < 0xfff) ) {
                    uint32_t prevRepeatCount = binds.back().repeatCount;
                    binds.back().repeatCount += 1;
                    assert(binds.back().repeatCount > prevRepeatCount); // check overflow
                    mergedIntoPrevious       = true;
                }
            }
        }
        if ( (target == lastTarget) && (runtimeOffset == lastOffset) && !binds.empty() ) {
            // duplicate bind for same location, ignore this one
            mergedIntoPrevious = true;
        }
        if ( !mergedIntoPrevious ) {
            Image::BindPattern pattern;
            pattern.target        = target;
            pattern.startVmOffset = runtimeOffset;
            pattern.repeatCount   = 1;
            pattern.skipCount     = 0;
            assert(pattern.startVmOffset == runtimeOffset);
            binds.push_back(pattern);
        }
        lastTarget = target;
        lastOffset = runtimeOffset;
	}, ^(const char* strongSymbolName) {
        if ( !_makingDyldCacheImages ) {
            // something has a strong symbol definition that may override a weak impl in the dyld cache
            Image::ResolvedSymbolTarget strongOverride;
            ResolvedTargetInfo          strongTargetInfo;
            if ( findSymbolInImage(forImage.loadAddress(), strongSymbolName, 0, false, strongOverride, strongTargetInfo) ) {
                for (const BuilderLoadedImage& li : _loadedImages) {
                    if ( li.loadAddress()->inDyldCache() && li.loadAddress()->hasWeakDefs() ) {
                        Image::ResolvedSymbolTarget implInCache;
                        ResolvedTargetInfo          implInCacheInfo;
                        if ( findSymbolInImage(li.loadAddress(), strongSymbolName, 0, false, implInCache, implInCacheInfo) ) {
                            // found another instance in some dylib in dyld cache, will need to patch it
                            Closure::PatchEntry patch;
                            patch.exportCacheOffset      = (uint32_t)implInCache.sharedCache.offset;
                            patch.overriddenDylibInCache = li.imageNum;
                            patch.replacement            = strongOverride;
                            _weakDefCacheOverrides.push_back(patch);
                        }
                    }
                }
            }
        }
	});
    writer.setBindInfo(binds);
}

void ClosureBuilder::reportRebasesAndBinds(ImageWriter& writer, BuilderLoadedImage& forImage)
{
    // report all rebases
    forImage.loadAddress()->forEachRebase(_diag, true, ^(uint64_t runtimeOffset, bool& stop) {
        _handlers->rebase(forImage.imageNum, forImage.loadAddress(), (uint32_t)runtimeOffset);
    });

    // report all binds
    forEachBind(forImage, ^(uint64_t runtimeOffset, Image::ResolvedSymbolTarget target, const ResolvedTargetInfo& targetInfo, bool& stop) {
        _handlers->bind(forImage.imageNum, forImage.loadAddress(), (uint32_t)runtimeOffset, target, targetInfo);
    },
    ^(const char* strongSymbolName) {});

    // i386 programs also use text relocs to rebase stubs
    if ( forImage.loadAddress()->cputype == CPU_TYPE_I386 ) {
        // FIX ME
    }
}

// These are mangled symbols for all the variants of operator new and delete
// which a main executable can define (non-weak) and override the
// weak-def implementation in the OS.
static const char* sTreatAsWeak[] = {
    "__Znwm", "__ZnwmRKSt9nothrow_t",
    "__Znam", "__ZnamRKSt9nothrow_t",
    "__ZdlPv", "__ZdlPvRKSt9nothrow_t", "__ZdlPvm",
    "__ZdaPv", "__ZdaPvRKSt9nothrow_t", "__ZdaPvm",
    "__ZnwmSt11align_val_t", "__ZnwmSt11align_val_tRKSt9nothrow_t",
    "__ZnamSt11align_val_t", "__ZnamSt11align_val_tRKSt9nothrow_t",
    "__ZdlPvSt11align_val_t", "__ZdlPvSt11align_val_tRKSt9nothrow_t", "__ZdlPvmSt11align_val_t",
    "__ZdaPvSt11align_val_t", "__ZdaPvSt11align_val_tRKSt9nothrow_t", "__ZdaPvmSt11align_val_t"
};


void ClosureBuilder::addChainedFixupInfo(ImageWriter& writer, const BuilderLoadedImage& forImage)
{
    // calculate max page starts
    __block uint32_t dataPageCount = 1;
    forImage.loadAddress()->forEachSegment(^(const dyld3::MachOFile::SegmentInfo& info, bool& stop) {
        if ( info.protections & VM_PROT_WRITE ) {
            dataPageCount += ((info.fileSize+4095) / 4096);
        }
    });

    // build array of starts
    STACK_ALLOC_ARRAY(uint64_t, starts, dataPageCount);
    forImage.loadAddress()->forEachChainedFixupStart(_diag, ^(uint64_t runtimeOffset, bool& stop) {
        starts.push_back(runtimeOffset);
    });

    // build array of targets
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(Image::ResolvedSymbolTarget, targets,     1024);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(ResolvedTargetInfo,          targetInfos, 1024);
    forImage.loadAddress()->forEachChainedFixupTarget(_diag, ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
        Image::ResolvedSymbolTarget target;
        ResolvedTargetInfo          targetInfo;
        if ( !findSymbol(forImage, libOrdinal, symbolName, weakImport, addend, target, targetInfo) ) {
            const char* expectedInPath = forImage.loadAddress()->dependentDylibLoadPath(libOrdinal-1);
            _diag.error("symbol '%s' not found, expected in '%s', needed by '%s'", symbolName, expectedInPath, forImage.path());
            stop = true;
            return;
        }
        if ( libOrdinal == BIND_SPECIAL_DYLIB_WEAK_DEF_COALESCE ) {
            // add if not already in array
            bool alreadyInArray = false;
            for (const char* sym : _weakDefsFromChainedBinds) {
                if ( strcmp(sym, symbolName) == 0 ) {
                    alreadyInArray = true;
                    break;
                }
            }
            if ( !alreadyInArray )
                _weakDefsFromChainedBinds.push_back(symbolName);
        }
        targets.push_back(target);
        targetInfos.push_back(targetInfo);
    });
    if ( _diag.hasError() )
        return;

    if ( _handlers != nullptr )
        _handlers->chainedBind(forImage.imageNum, forImage.loadAddress(), starts, targets, targetInfos);
    else
        writer.setChainedFixups(starts, targets); // store results in Image object

    // with chained fixups, main executable may define symbol that overrides weak-defs but has no fixup
    if ( _isLaunchClosure && forImage.loadAddress()->hasWeakDefs() && forImage.loadAddress()->isMainExecutable() ) {
        for (const char* weakSymbolName : sTreatAsWeak) {
            Diagnostics exportDiag;
            dyld3::MachOAnalyzer::FoundSymbol foundInfo;
            if ( forImage.loadAddress()->findExportedSymbol(exportDiag, weakSymbolName, foundInfo, nullptr) ) {
                _weakDefsFromChainedBinds.push_back(weakSymbolName);
            }
        }
    }
}


bool ClosureBuilder::findSymbolInImage(const MachOAnalyzer* macho, const char* symbolName, uint64_t addend, bool followReExports,
                                       Image::ResolvedSymbolTarget& target, ResolvedTargetInfo& targetInfo)
{
    targetInfo.foundInDylib        = nullptr;
    targetInfo.requestedSymbolName = symbolName;
    targetInfo.addend              = addend;
    targetInfo.isWeakDef           = false;
    MachOLoaded::DependentToMachOLoaded reexportFinder = ^(const MachOLoaded* mh, uint32_t depIndex) {
        return (const MachOLoaded*)findDependent(mh, depIndex);
    };
    MachOAnalyzer::DependentToMachOLoaded finder = nullptr;
    if ( followReExports )
        finder = reexportFinder;

    dyld3::MachOAnalyzer::FoundSymbol foundInfo;
    if ( macho->findExportedSymbol(_diag, symbolName, foundInfo, finder) ) {
        const MachOAnalyzer* impDylib = (const MachOAnalyzer*)foundInfo.foundInDylib;
        targetInfo.foundInDylib    = foundInfo.foundInDylib;
        targetInfo.foundSymbolName = foundInfo.foundSymbolName;
        if ( foundInfo.isWeakDef )
            targetInfo.isWeakDef = true;
        if ( foundInfo.kind == MachOAnalyzer::FoundSymbol::Kind::absolute ) {
            target.absolute.kind   = Image::ResolvedSymbolTarget::kindAbsolute;
            target.absolute.value  = foundInfo.value + addend;
        }
        else if ( impDylib->inDyldCache() ) {
            target.sharedCache.kind   = Image::ResolvedSymbolTarget::kindSharedCache;
            target.sharedCache.offset = (uint8_t*)impDylib - (uint8_t*)_dyldCache + foundInfo.value + addend;
        }
        else {
            target.image.kind     = Image::ResolvedSymbolTarget::kindImage;
            target.image.imageNum = findLoadedImage(impDylib).imageNum;
            target.image.offset   = foundInfo.value + addend;
        }
        return true;
    }
    return false;
}

bool ClosureBuilder::findSymbol(const BuilderLoadedImage& fromImage, int libOrdinal, const char* symbolName, bool weakImport, uint64_t addend,
                                Image::ResolvedSymbolTarget& target, ResolvedTargetInfo& targetInfo)
{
    targetInfo.weakBindCoalese      = false;
    targetInfo.weakBindSameImage    = false;
    targetInfo.requestedSymbolName  = symbolName;
    targetInfo.libOrdinal           = libOrdinal;
    if ( libOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) {
        for (const BuilderLoadedImage& li : _loadedImages) {
            if ( !li.rtldLocal && findSymbolInImage(li.loadAddress(), symbolName, addend, true, target, targetInfo) )
                return true;
        }
        if ( weakImport ) {
            target.absolute.kind  = Image::ResolvedSymbolTarget::kindAbsolute;
            target.absolute.value = 0;
            return true;
        }
        _diag.error("symbol '%s' not found, expected in flat namespace by '%s'", symbolName, fromImage.path());
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_WEAK_DEF_COALESCE ) {
        // to resolve weakDef coalesing, we need to search all images in order and use first definition
        // but, if first found is a weakDef, a later non-weak def overrides that
        bool foundWeakDefImpl   = false;
        bool foundStrongDefImpl = false;
        bool foundImpl          = false;
        Image::ResolvedSymbolTarget  aTarget;
        ResolvedTargetInfo           aTargetInfo;
        STACK_ALLOC_ARRAY(const BuilderLoadedImage*, cachedDylibsUsingSymbol, 1024);
        for (const BuilderLoadedImage& li : _loadedImages) {
            // only search images with weak-defs that were not loaded with RTLD_LOCAL
            if ( li.loadAddress()->hasWeakDefs() && !li.rtldLocal ) {
                if ( findSymbolInImage(li.loadAddress(), symbolName, addend, false, aTarget, aTargetInfo) ) {
                    foundImpl = true;
                    // with non-chained images, weak-defs first have a rebase to their local impl, and a weak-bind which allows earlier impls to override
                    if ( !li.loadAddress()->hasChainedFixups() && (aTargetInfo.foundInDylib == fromImage.loadAddress()) )
                        targetInfo.weakBindSameImage = true;
                    if ( aTargetInfo.isWeakDef ) {
                        // found a weakDef impl, if this is first found, set target to this
                        if ( !foundWeakDefImpl && !foundStrongDefImpl ) {
                            target      = aTarget;
                            targetInfo  = aTargetInfo;
                        }
                        foundWeakDefImpl = true;
                    }
                    else {
                        // found a non-weak impl, use this (unless early strong found)
                        if ( !foundStrongDefImpl ) {
                            target      = aTarget;
                            targetInfo  = aTargetInfo;
                        }
                        foundStrongDefImpl = true;
                    }
                }
                if ( foundImpl && !_makingDyldCacheImages && li.loadAddress()->inDyldCache() )
                    cachedDylibsUsingSymbol.push_back(&li);
            }
        }
        // now that final target found, if any dylib in dyld cache uses that symbol name, redirect it to new target
        if ( !cachedDylibsUsingSymbol.empty() ) {
            for (const BuilderLoadedImage* li : cachedDylibsUsingSymbol) {
                Image::ResolvedSymbolTarget implInCache;
                ResolvedTargetInfo          implInCacheInfo;
                if ( findSymbolInImage(li->loadAddress(), symbolName, addend, false, implInCache, implInCacheInfo) ) {
                    if ( implInCache != target ) {
                        // found another instance in some dylib in dyld cache, will need to patch it
                        Closure::PatchEntry patch;
                        patch.exportCacheOffset      = (uint32_t)implInCache.sharedCache.offset;
                        patch.overriddenDylibInCache = li->imageNum;
                        patch.replacement            = target;
                        _weakDefCacheOverrides.push_back(patch);
                    }
                }
            }
        }
        targetInfo.weakBindCoalese = true;

        if ( foundImpl )
            return true;
        _diag.error("symbol '%s' not found, expected to be weak-def coalesced", symbolName);
    }
    else {
        const BuilderLoadedImage* targetLoadedImage = nullptr;
        if ( (libOrdinal > 0) && (libOrdinal <= (int)fromImage.dependents.count()) ) {
            ImageNum childNum = fromImage.dependents[libOrdinal - 1].imageNum();
            if ( childNum != kMissingWeakLinkedImage ) {
                targetLoadedImage = &findLoadedImage(childNum);
            }
        }
        else if ( libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
            targetLoadedImage = &fromImage;
        }
        else if ( libOrdinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE ) {
            targetLoadedImage = &_loadedImages[_mainProgLoadIndex];
        }
        else {
            _diag.error("unknown special ordinal %d in %s", libOrdinal, fromImage.path());
            return false;
        }

        if ( targetLoadedImage != nullptr ) {
            if ( findSymbolInImage(targetLoadedImage->loadAddress(), symbolName, addend, true, target, targetInfo) )
                return true;
        }

        if ( weakImport ) {
            target.absolute.kind  = Image::ResolvedSymbolTarget::kindAbsolute;
            target.absolute.value = 0;
            return true;
        }
        const char* expectedInPath = targetLoadedImage ? targetLoadedImage->path() : "unknown";
        _diag.error("symbol '%s' not found, expected in '%s', needed by '%s'", symbolName, expectedInPath, fromImage.path());
        if ( _launchErrorInfo != nullptr ) {
            _launchErrorInfo->kind              = DYLD_EXIT_REASON_SYMBOL_MISSING;
            _launchErrorInfo->clientOfDylibPath = fromImage.path();
            _launchErrorInfo->targetDylibPath   = expectedInPath;
            _launchErrorInfo->symbol            = symbolName;
        }
    }
    return false;
}


void ClosureBuilder::depthFirstRecurseSetInitInfo(uint32_t loadIndex, InitInfo initInfos[], uint32_t& initOrder, bool& hasError)
{
    if ( initInfos[loadIndex].visited )
        return;
    initInfos[loadIndex].visited        = true;
    initInfos[loadIndex].danglingUpward = false;

    if (_loadedImages[loadIndex].isBadImage) {
        hasError = true;
        return;
    }

    for (const Image::LinkedImage& dep : _loadedImages[loadIndex].dependents) {
        if ( dep.imageNum() == kMissingWeakLinkedImage )
            continue;
        ClosureBuilder::BuilderLoadedImage& depLi = findLoadedImage(dep.imageNum());
        uint32_t depLoadIndex = (uint32_t)_loadedImages.index(depLi);
        if ( dep.kind() == Image::LinkKind::upward ) {
            if ( !initInfos[depLoadIndex].visited )
                initInfos[depLoadIndex].danglingUpward = true;
        }
        else {
            depthFirstRecurseSetInitInfo(depLoadIndex, initInfos, initOrder, hasError);
            if (hasError)
                return;
        }
    }
    initInfos[loadIndex].initOrder = initOrder++;
}

void ClosureBuilder::computeInitOrder(ImageWriter& imageWriter, uint32_t loadIndex)
{
    // allocate array to track initializers
    InitInfo initInfos[_loadedImages.count()];
    bzero(initInfos, sizeof(initInfos));

    // recurse all images and build initializer list from bottom up
    uint32_t initOrder = 1;
    bool hasMissingDependent = false;
    depthFirstRecurseSetInitInfo(loadIndex, initInfos, initOrder, hasMissingDependent);
    if (hasMissingDependent) {
        imageWriter.setInvalid();
        return;
    }

    // any images not visited yet are are danging, force add them to end of init list
    for (uint32_t i=0; i < (uint32_t)_loadedImages.count(); ++i) {
        if ( !initInfos[i].visited && initInfos[i].danglingUpward ) {
            depthFirstRecurseSetInitInfo(i, initInfos, initOrder, hasMissingDependent);
        }
    }

    if (hasMissingDependent) {
        imageWriter.setInvalid();
        return;
    }
    
    // build array of just images with initializer
    STACK_ALLOC_ARRAY(uint32_t, indexOfImagesWithInits, _loadedImages.count());
    uint32_t index = 0;
    for (const BuilderLoadedImage& li : _loadedImages) {
        if ( initInfos[index].visited && li.hasInits ) {
            indexOfImagesWithInits.push_back(index);
        }
        ++index;
    }

    // bubble sort (FIXME)
    if ( indexOfImagesWithInits.count() > 1 ) {
        for (uint32_t i=0; i < indexOfImagesWithInits.count()-1; ++i) {
            for (uint32_t j=0; j < indexOfImagesWithInits.count()-i-1; ++j) {
                if ( initInfos[indexOfImagesWithInits[j]].initOrder > initInfos[indexOfImagesWithInits[j+1]].initOrder ) {
                    uint32_t temp               = indexOfImagesWithInits[j];
                    indexOfImagesWithInits[j]   = indexOfImagesWithInits[j+1];
                    indexOfImagesWithInits[j+1] = temp;
                }
            }
        }
    }

    // copy ImageNum of each image with initializers into array
    ImageNum initNums[indexOfImagesWithInits.count()];
    for (uint32_t i=0; i < indexOfImagesWithInits.count(); ++i) {
        initNums[i] = _loadedImages[indexOfImagesWithInits[i]].imageNum;
    }

    // add to closure info
    imageWriter.setInitsOrder(initNums, (uint32_t)indexOfImagesWithInits.count());
}

void ClosureBuilder::addCachePatchInfo(ImageWriter& imageWriter, const BuilderLoadedImage& forImage)
{
    assert(_handlers != nullptr);
    _handlers->forEachExportsPatch(forImage.imageNum, ^(const CacheDylibsBindingHandlers::PatchInfo& info) {
        assert(info.usesCount != 0);
        imageWriter.addExportPatchInfo(info.exportCacheOffset, info.exportSymbolName, info.usesCount, info.usesArray);
    });
}

void ClosureBuilder::addClosureInfo(LaunchClosureWriter& closureWriter)
{
    // record which is libSystem
    assert(_libSystemImageNum != 0);
	closureWriter.setLibSystemImageNum(_libSystemImageNum);

    // record which is libdyld
    assert(_libDyldImageNum != 0);
    Image::ResolvedSymbolTarget entryLocation;
    ResolvedTargetInfo          entryInfo;
    if ( findSymbolInImage(findLoadedImage(_libDyldImageNum).loadAddress(), "__ZN5dyld318entryVectorForDyldE", 0, false, entryLocation, entryInfo) ) {
        const dyld3::LibDyldEntryVector* libDyldEntry = nullptr;
        switch ( entryLocation.image.kind ) {
            case Image::ResolvedSymbolTarget::kindSharedCache:
                libDyldEntry = (dyld3::LibDyldEntryVector*)((uint8_t*)_dyldCache + entryLocation.sharedCache.offset);
                break;
            case Image::ResolvedSymbolTarget::kindImage:
                libDyldEntry = (dyld3::LibDyldEntryVector*)((uint8_t*)findLoadedImage(entryLocation.image.imageNum).loadAddress() + entryLocation.image.offset);
                break;
        }
        if ( (libDyldEntry != nullptr) && (libDyldEntry->binaryFormatVersion == dyld3::closure::kFormatVersion) )
            closureWriter.setLibDyldEntry(entryLocation);
        else
            _diag.error("libdyld.dylib entry vector is incompatible");
    }
    else {
        _diag.error("libdyld.dylib is missing entry vector");
    }

    // record which is main executable
    ImageNum mainProgImageNum = _loadedImages[_mainProgLoadIndex].imageNum;
    closureWriter.setTopImageNum(mainProgImageNum);

    // add entry
    uint32_t    entryOffset;
    bool        usesCRT;
    if ( _loadedImages[_mainProgLoadIndex].loadAddress()->getEntry(entryOffset, usesCRT) ) {
        Image::ResolvedSymbolTarget location;
        location.image.kind     = Image::ResolvedSymbolTarget::kindImage;
        location.image.imageNum = mainProgImageNum;
        location.image.offset   = entryOffset;
        if ( usesCRT )
            closureWriter.setStartEntry(location);
        else
            closureWriter.setMainEntry(location);
    }

    // add env vars that must match at launch time
    _pathOverrides.forEachEnvVar(^(const char* envVar) {
        closureWriter.addEnvVar(envVar);
    });

    // add list of files which must be missing
    STACK_ALLOC_ARRAY(const char*, paths, 8192);
    if ( _mustBeMissingPaths != nullptr ) {
        _mustBeMissingPaths->forEachPath(^(const char* aPath) {
            paths.push_back(aPath);
        });
    }
	closureWriter.setMustBeMissingFiles(paths);
}


// used at launch by dyld when kernel has already mapped main executable
const LaunchClosure* ClosureBuilder::makeLaunchClosure(const LoadedFileInfo& fileInfo, bool allowInsertFailures)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_BUILD_CLOSURE, 0, 0, 0);
    const mach_header* mainMH = (const mach_header*)fileInfo.fileContent;
    // set up stack based storage for all arrays
    BuilderLoadedImage  loadImagesStorage[512];
    Image::LinkedImage  dependenciesStorage[512*8];
    InterposingTuple    tuplesStorage[64];
    Closure::PatchEntry cachePatchStorage[64];
    const char*         weakDefNameStorage[64];
    _loadedImages.setInitialStorage(loadImagesStorage, 512);
    _dependencies.setInitialStorage(dependenciesStorage, 512*8);
    _interposingTuples.setInitialStorage(tuplesStorage, 64);
    _weakDefCacheOverrides.setInitialStorage(cachePatchStorage, 64);
    _weakDefsFromChainedBinds.setInitialStorage(weakDefNameStorage, 64);
    ArrayFinalizer<BuilderLoadedImage> scopedCleanup(_loadedImages, ^(BuilderLoadedImage& li) { if (li.unmapWhenDone) {_fileSystem.unloadFile(li.loadedFileInfo); li.unmapWhenDone=false;} });

    const MachOAnalyzer* mainExecutable = MachOAnalyzer::validMainExecutable(_diag, mainMH, fileInfo.path, fileInfo.sliceLen, _archName, _platform);
    if ( mainExecutable == nullptr )
        return nullptr;
    if ( !mainExecutable->isDynamicExecutable() ) {
        _diag.error("not a main executable");
        return nullptr;
    }
    _isLaunchClosure   = true;

    // add any DYLD_INSERT_LIBRARIES
    _nextIndex = 0;
    _pathOverrides.forEachInsertedDylib(^(const char* dylibPath) {
        BuilderLoadedImage insertEntry;
        insertEntry.loadedFileInfo.path = strdup_temp(dylibPath);
        insertEntry.imageNum            = _startImageNum + _nextIndex++;
        insertEntry.unmapWhenDone       = true;
        insertEntry.contentRebased      = false;
        insertEntry.hasInits            = false;
        insertEntry.markNeverUnload     = true;
        insertEntry.rtldLocal           = false;
        insertEntry.isBadImage          = false;
        insertEntry.overrideImageNum    = 0;
        _loadedImages.push_back(insertEntry);
    });
    _mainProgLoadIndex = (uint32_t)_loadedImages.count();

    // add main executable
    BuilderLoadedImage mainEntry;
    mainEntry.loadedFileInfo   = fileInfo;
    mainEntry.imageNum         = _startImageNum + _nextIndex++;
    mainEntry.unmapWhenDone    = false;
    mainEntry.contentRebased   = false;
    mainEntry.hasInits         = false;
    mainEntry.markNeverUnload  = true;
    mainEntry.rtldLocal        = false;
    mainEntry.isBadImage       = false;
    mainEntry.overrideImageNum = 0;
    _loadedImages.push_back(mainEntry);

	// get mach_headers for all images needed to launch this main executable
    LoadedImageChain chainStart = { nullptr, _loadedImages[_mainProgLoadIndex] };
    recursiveLoadDependents(chainStart);
    if ( _diag.hasError() )
        return nullptr;
    for (uint32_t i=0; i < _mainProgLoadIndex; ++i) {
        closure::LoadedFileInfo loadedFileInfo = MachOAnalyzer::load(_diag, _fileSystem, _loadedImages[i].loadedFileInfo.path, _archName, _platform);
        const char* originalLoadPath = _loadedImages[i].loadedFileInfo.path;
        _loadedImages[i].loadedFileInfo = loadedFileInfo;
        if ( _loadedImages[i].loadAddress() != nullptr ) {
            LoadedImageChain insertChainStart = { nullptr, _loadedImages[i] };
            recursiveLoadDependents(insertChainStart);
        }
        if ( _diag.hasError() || (_loadedImages[i].loadAddress() == nullptr) ) {
            if ( !allowInsertFailures ) {
                if ( _diag.noError() )
                    _diag.error("could not load inserted dylib %s", originalLoadPath);
                return nullptr;
            }
            _diag.clearError(); // FIXME add way to plumb back warning
            // remove slot for inserted image that could not loaded
            _loadedImages.remove(i);
            i -= 1;
            _mainProgLoadIndex -= 1;
            _nextIndex -= 1;
            // renumber images in this closure
            for (uint32_t j=i+1; j < _loadedImages.count(); ++j) {
                if ( (_loadedImages[j].imageNum >= _startImageNum) && (_loadedImages[j].imageNum <= _startImageNum+_nextIndex) )
                    _loadedImages[j].imageNum -= 1;
            }
        }
    }
    loadDanglingUpwardLinks();

    // only some images need to go into closure (ones from dyld cache do not)
    STACK_ALLOC_ARRAY(ImageWriter, writers, _loadedImages.count());
    for (BuilderLoadedImage& li : _loadedImages) {
        if ( li.imageNum >= _startImageNum ) {
            writers.push_back(ImageWriter());
            buildImage(writers.back(), li);
            if ( _diag.hasError() )
                return nullptr;
        }
        if ( li.loadAddress()->isDylib() && (strcmp(li.loadAddress()->installName(), "/usr/lib/system/libdyld.dylib") == 0) )
            _libDyldImageNum = li.imageNum;
        else if ( strcmp(li.path(), "/usr/lib/libSystem.B.dylib") == 0 )
            _libSystemImageNum = li.imageNum;
   }

    // add initializer order into top level Images (may be inserted dylibs before main executable)
    for (uint32_t i=0; i <= _mainProgLoadIndex; ++i)
        computeInitOrder(writers[i], i);

    if ( _diag.hasError() )
        return nullptr;

    // combine all Image objects into one ImageArray
    ImageArrayWriter imageArrayWriter(_startImageNum, (uint32_t)writers.count());
    for (ImageWriter& writer : writers) {
        imageArrayWriter.appendImage(writer.finalize());
        writer.deallocate();
    }
    const ImageArray* imageArray = imageArrayWriter.finalize();

    // merge ImageArray object into LaunchClosure object
    __block LaunchClosureWriter closureWriter(imageArray);

    // record shared cache info
    if ( _dyldCache != nullptr ) {
        // record cache UUID
        uuid_t cacheUUID;
        _dyldCache->getUUID(cacheUUID);
        closureWriter.setDyldCacheUUID(cacheUUID);

        // record any cache patching needed because of dylib overriding cache
        for (const BuilderLoadedImage& li : _loadedImages) {
            if ( li.overrideImageNum != 0 ) {
                const Image* cacheImage = _dyldImageArray->imageForNum(li.overrideImageNum);
                STACK_ALLOC_ARRAY(Closure::PatchEntry, patches, cacheImage->patchableExportCount());
                MachOLoaded::DependentToMachOLoaded reexportFinder = ^(const MachOLoaded* mh, uint32_t depIndex) {
                    return (const MachOLoaded*)findDependent(mh, depIndex);
                };
                //fprintf(stderr, "'%s' overrides '%s'\n", li.loadedFileInfo.path, cacheImage->path());
                cacheImage->forEachPatchableExport(^(uint32_t cacheOffsetOfImpl, const char* symbolName) {
                    dyld3::MachOAnalyzer::FoundSymbol foundInfo;
                    Diagnostics                       patchDiag;
                    Closure::PatchEntry               patch;
                    patch.overriddenDylibInCache  = li.overrideImageNum;
                    patch.exportCacheOffset       = cacheOffsetOfImpl;
                    if ( li.loadAddress()->findExportedSymbol(patchDiag, symbolName, foundInfo, reexportFinder) ) {
                        const MachOAnalyzer* impDylib = (const MachOAnalyzer*)foundInfo.foundInDylib;
                        patch.replacement.image.kind     = Image::ResolvedSymbolTarget::kindImage;
                        patch.replacement.image.imageNum = findLoadedImage(impDylib).imageNum;
                        patch.replacement.image.offset   = foundInfo.value;
                    }
                    else {
                        // this means the symbol is missing in the cache override dylib, so set any uses to NULL
                        patch.replacement.absolute.kind    = Image::ResolvedSymbolTarget::kindAbsolute;
                        patch.replacement.absolute.value   = 0;
                    }
                    patches.push_back(patch);
                });
                closureWriter.addCachePatches(patches);
            }
        }

        // handle any extra weak-def coalescing needed by chained fixups
        if ( !_weakDefsFromChainedBinds.empty() ) {
            for (const char* symbolName : _weakDefsFromChainedBinds) {
                Image::ResolvedSymbolTarget cacheOverrideTarget;
                bool haveCacheOverride = false;
                bool foundCachOverrideIsWeakDef = false;
                for (const BuilderLoadedImage& li : _loadedImages) {
                    if ( !li.loadAddress()->hasWeakDefs() )
                        continue;
                    Image::ResolvedSymbolTarget target;
                    ResolvedTargetInfo          targetInfo;
                    if ( findSymbolInImage(li.loadAddress(), symbolName, 0, false, target, targetInfo) ) {
                        if ( li.loadAddress()->inDyldCache() ) {
                            if ( haveCacheOverride ) {
                                Closure::PatchEntry patch;
                                patch.exportCacheOffset      = (uint32_t)target.sharedCache.offset;
                                patch.overriddenDylibInCache = li.imageNum;
                                patch.replacement            = cacheOverrideTarget;
                                _weakDefCacheOverrides.push_back(patch);
                            }
                            else {
                                // found first in cached dylib, so no need to patch cache for this symbol
                                break;
                            }
                        }
                        else {
                            // found image that exports this symbol and is not in cache
                            if ( !haveCacheOverride || (foundCachOverrideIsWeakDef && !targetInfo.isWeakDef) ) {
                                // update cache to use this symbol if it if first found or it is first non-weak found
                                cacheOverrideTarget         = target;
                                foundCachOverrideIsWeakDef  = targetInfo.isWeakDef;
                                haveCacheOverride           = true;
                            }
                        }
                    }
                }
            }
        }

        // record any cache patching needed because weak-def C++ symbols override dyld cache
        if ( !_weakDefCacheOverrides.empty() )
            closureWriter.addCachePatches(_weakDefCacheOverrides);

   }

#if __IPHONE_OS_VERSION_MIN_REQUIRED
    // if closure is built on-device for iOS, then record boot UUID
    char bootSessionUUID[256] = { 0 };
    size_t bootSize = sizeof(bootSessionUUID);
    if ( sysctlbyname("kern.bootsessionuuid", bootSessionUUID, &bootSize, NULL, 0) == 0 )
        closureWriter.setBootUUID(bootSessionUUID);
#endif

     // record any interposing info
    imageArray->forEachImage(^(const Image* image, bool &stop) {
        if ( !image->inDyldCache() )
            addInterposingTuples(closureWriter, image, findLoadedImage(image->imageNum()).loadAddress());
    });

    // modify fixups in contained Images by applying interposing tuples
    closureWriter.applyInterposing();

    // set flags
    closureWriter.setUsedAtPaths(_atPathUsed);
    closureWriter.setUsedFallbackPaths(_fallbackPathUsed);
    closureWriter.setInitImageCount((uint32_t)_loadedImages.count());

    // add other closure attributes
    addClosureInfo(closureWriter);

    // make result
    const LaunchClosure* result = closureWriter.finalize();
    imageArrayWriter.deallocate();

    return result;
}

// used by libdyld for dlopen()
const DlopenClosure* ClosureBuilder::makeDlopenClosure(const char* path, const LaunchClosure* mainClosure, const Array<LoadedImage>& alreadyLoadedList,
                                                       closure::ImageNum callerImageNum, bool noLoad, bool canUseSharedCacheClosure, closure::ImageNum* topImageNum)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_BUILD_CLOSURE, 0, 0, 0);
    // set up stack based storage for all arrays
    BuilderLoadedImage  loadImagesStorage[512];
    Image::LinkedImage  dependenciesStorage[512*8];
    Closure::PatchEntry cachePatchStorage[64];
    _loadedImages.setInitialStorage(loadImagesStorage, 512);
    _dependencies.setInitialStorage(dependenciesStorage, 512*8);
    _weakDefCacheOverrides.setInitialStorage(cachePatchStorage, 64);
    ArrayFinalizer<BuilderLoadedImage> scopedCleanup(_loadedImages, ^(BuilderLoadedImage& li) { if (li.unmapWhenDone) {_fileSystem.unloadFile(li.loadedFileInfo); li.unmapWhenDone=false;} });

    // fill in builder array from already loaded images
    bool cachedDylibsExpectedOnDisk = _dyldCache ? _dyldCache->header.dylibsExpectedOnDisk : true;
    uintptr_t callerImageIndex = UINTPTR_MAX;
    for (const LoadedImage& ali : alreadyLoadedList) {
        const Image*          image       = ali.image();
        const MachOAnalyzer*  ma          = (MachOAnalyzer*)(ali.loadedAddress());
        bool                  inDyldCache = ma->inDyldCache();
        BuilderLoadedImage entry;
        ImageNum overrideImageNum;
        entry.loadedFileInfo.path        = image->path();
        entry.loadedFileInfo.fileContent = ma;
        entry.loadedFileInfo.sliceOffset = 0;
        entry.loadedFileInfo.inode       = 0;
        entry.loadedFileInfo.mtime       = 0;
        entry.imageNum                   = image->imageNum();
        entry.dependents                 = image->dependentsArray();
        entry.unmapWhenDone              = false;
        entry.contentRebased             = inDyldCache;
        entry.hasInits                   = false;
        entry.markNeverUnload            = image->neverUnload();
        entry.rtldLocal                  = ali.hideFromFlatSearch();
        entry.isBadImage                 = false;
        entry.overrideImageNum           = 0;
        if ( !inDyldCache && image->isOverrideOfDyldCacheImage(overrideImageNum) ) {
            entry.overrideImageNum  = overrideImageNum;
            canUseSharedCacheClosure = false;
        }
        if ( !inDyldCache || cachedDylibsExpectedOnDisk )
            image->hasFileModTimeAndInode(entry.loadedFileInfo.inode, entry.loadedFileInfo.mtime);
        if ( entry.imageNum == callerImageNum )
            callerImageIndex = _loadedImages.count();
        _loadedImages.push_back(entry);
   }
    _alreadyInitedIndex = (uint32_t)_loadedImages.count();

    // find main executable (may be needed for @executable_path)
    _isLaunchClosure = false;
    for (uint32_t i=0; i < alreadyLoadedList.count(); ++i) {
        if ( _loadedImages[i].loadAddress()->isMainExecutable() )  {
            _mainProgLoadIndex = i;
            break;
        }
    }

    // add top level dylib being dlopen()ed
    BuilderLoadedImage* foundTopImage;
    _nextIndex = 0;
    // @rpath has caller's LC_PRATH, then main executable's LC_RPATH
    BuilderLoadedImage& callerImage = (callerImageIndex != UINTPTR_MAX) ? _loadedImages[callerImageIndex]  : _loadedImages[_mainProgLoadIndex];
    LoadedImageChain chainCaller = { nullptr, callerImage };
    LoadedImageChain chainMain = { &chainCaller, _loadedImages[_mainProgLoadIndex] };
    if ( !findImage(path, chainMain, foundTopImage, false, canUseSharedCacheClosure) ) {
        // If we didn't find the image, but its a shared cache path, then try again with realpath.
        if ( (strncmp(path, "/usr/lib/", 9) == 0) || (strncmp(path, "/System/Library/", 16) == 0) ) {
            char resolvedPath[PATH_MAX];
            if ( _fileSystem.getRealPath(path, resolvedPath) ) {
                if ( !findImage(resolvedPath, chainMain, foundTopImage, false, canUseSharedCacheClosure) ) {
                    return nullptr;
                }
            } else {
                // We didn't find a new path from realpath
                return nullptr;
            }
        } else {
            // Not in /usr/lib/ or /System/Library/
            return nullptr;
        }
    }

    // exit early in RTLD_NOLOAD mode
    if ( noLoad ) {
        // if no new images added to _loadedImages, then requested path was already loaded
        if ( (uint32_t)_loadedImages.count() == _alreadyInitedIndex )
            *topImageNum = foundTopImage->imageNum;
        else
            *topImageNum = 0;
        return nullptr;
    }

    // fast path if roots are not allowed and target is in dyld cache or is other
    if ( (_dyldCache != nullptr) && (_dyldCache->header.cacheType == kDyldSharedCacheTypeProduction) ) {
        if ( foundTopImage->imageNum < closure::kFirstLaunchClosureImageNum ) {
            *topImageNum = foundTopImage->imageNum;
            return nullptr;
        }
    }

    // recursive load dependents
    // @rpath for stuff top dylib depends on uses LC_RPATH from caller, main exe, and dylib being dlopen()ed
    LoadedImageChain chainTopDylib = { &chainMain, *foundTopImage };
    recursiveLoadDependents(chainTopDylib);
    if ( _diag.hasError() )
        return nullptr;
    loadDanglingUpwardLinks();

    // only some images need to go into closure (ones from dyld cache do not)
    STACK_ALLOC_ARRAY(ImageWriter, writers, _loadedImages.count());
    for (BuilderLoadedImage& li : _loadedImages) {
        if ( li.imageNum >= _startImageNum ) {
            writers.push_back(ImageWriter());
            buildImage(writers.back(), li);
        }
    }

    if ( _diag.hasError() )
        return nullptr;

    // check if top image loaded is in shared cache along with everything it depends on
    *topImageNum = foundTopImage->imageNum;
    if ( writers.count() == 0 ) {
        return nullptr;
    } else if ( canUseSharedCacheClosure && ( foundTopImage->imageNum < closure::kFirstLaunchClosureImageNum ) ) {
        // We used a shared cache built closure, but now discovered roots.  We need to try again
        topImageNum = 0;
        return sRetryDlopenClosure;
    }

    // add initializer order into top level Image
    computeInitOrder(writers[0], (uint32_t)alreadyLoadedList.count());

    if ( _diag.hasError() )
        return nullptr;

    // combine all Image objects into one ImageArray
    ImageArrayWriter imageArrayWriter(_startImageNum, (uint32_t)writers.count());
    for (ImageWriter& writer : writers) {
        imageArrayWriter.appendImage(writer.finalize());
        writer.deallocate();
    }
    const ImageArray* imageArray = imageArrayWriter.finalize();

    // merge ImageArray object into LaunchClosure object
    DlopenClosureWriter closureWriter(imageArray);

    // add other closure attributes
    closureWriter.setTopImageNum(foundTopImage->imageNum);

    // record any cache patching needed because of dylib overriding cache
    if ( _dyldCache != nullptr ) {
        for (const BuilderLoadedImage& li : _loadedImages) {
            if ( (li.overrideImageNum != 0) && (li.imageNum >= _startImageNum) ) {
                const Image* cacheImage = _dyldImageArray->imageForNum(li.overrideImageNum);
                STACK_ALLOC_ARRAY(Closure::PatchEntry, patches, cacheImage->patchableExportCount());
                MachOLoaded::DependentToMachOLoaded reexportFinder = ^(const MachOLoaded* mh, uint32_t depIndex) {
                    return (const MachOLoaded*)findDependent(mh, depIndex);
                };
                //fprintf(stderr, "'%s' overrides '%s'\n", li.loadedFileInfo.path, cacheImage->path());
                cacheImage->forEachPatchableExport(^(uint32_t cacheOffsetOfImpl, const char* symbolName) {
                    dyld3::MachOAnalyzer::FoundSymbol foundInfo;
                    Diagnostics                       patchDiag;
                    Closure::PatchEntry               patch;
                    patch.overriddenDylibInCache  = li.overrideImageNum;
                    patch.exportCacheOffset       = cacheOffsetOfImpl;
                    if ( li.loadAddress()->findExportedSymbol(patchDiag, symbolName, foundInfo, reexportFinder) ) {
                        const MachOAnalyzer* impDylib = (const MachOAnalyzer*)foundInfo.foundInDylib;
                        patch.replacement.image.kind     = Image::ResolvedSymbolTarget::kindImage;
                        patch.replacement.image.imageNum = findLoadedImage(impDylib).imageNum;
                        patch.replacement.image.offset   = foundInfo.value;
                    }
                    else {
                        patch.replacement.absolute.kind    = Image::ResolvedSymbolTarget::kindAbsolute;
                        patch.replacement.absolute.value   = 0;
                    }
                    patches.push_back(patch);
                });
                closureWriter.addCachePatches(patches);
            }
        }
    }

    // Dlopen's should never keep track of missing paths as we don't cache these closures.
    assert(_mustBeMissingPaths == nullptr);

    // make final DlopenClosure object
    const DlopenClosure* result = closureWriter.finalize();
    imageArrayWriter.deallocate();
    return result;
}


// used by dyld_closure_util
const LaunchClosure* ClosureBuilder::makeLaunchClosure(const char* mainPath, bool allowInsertFailures)
{
    closure::LoadedFileInfo loadedFileInfo = MachOAnalyzer::load(_diag, _fileSystem, mainPath, _archName, _platform);
    const MachOAnalyzer* mh = (const MachOAnalyzer*)loadedFileInfo.fileContent;
    loadedFileInfo.path = mainPath;
    if (_diag.hasError())
        return nullptr;
    if (mh == nullptr) {
        _diag.error("could not load file");
        return nullptr;
    }
    if (!mh->isDynamicExecutable()) {
        _diag.error("file is not an executable");
        return nullptr;
    }
    const_cast<PathOverrides*>(&_pathOverrides)->setMainExecutable(mh, mainPath);
    const LaunchClosure* launchClosure = makeLaunchClosure(loadedFileInfo, allowInsertFailures);
    loadedFileInfo.unload(loadedFileInfo);
    return launchClosure;
}


// used by dyld shared cache builder
const ImageArray* ClosureBuilder::makeDyldCacheImageArray(bool customerCache, const Array<CachedDylibInfo>& dylibs, const Array<CachedDylibAlias>& aliases)
{
    // because this is run in cache builder using dispatch_apply() there is minimal stack space
    // so set up storage for all arrays to be vm_allocated
    uintptr_t maxImageCount = dylibs.count() + 16;
    _loadedImages.reserve(maxImageCount);
    _dependencies.reserve(maxImageCount*16);

    _makingDyldCacheImages = true;
    _makingCustomerCache   = customerCache;
    _aliases               = &aliases;

    // build _loadedImages[] with every dylib in cache
    __block ImageNum imageNum = _startImageNum;
    for (const CachedDylibInfo& aDylibInfo : dylibs)  {
        BuilderLoadedImage entry;
        entry.loadedFileInfo                = aDylibInfo.fileInfo;
        entry.imageNum                      = imageNum++;
        entry.unmapWhenDone                 = false;
        entry.contentRebased                = false;
        entry.hasInits                      = false;
        entry.markNeverUnload               = true;
        entry.rtldLocal                     = false;
        entry.isBadImage                    = false;
        entry.overrideImageNum              = 0;
        _loadedImages.push_back(entry);
    }

    // wire up dependencies between cached dylibs
    for (BuilderLoadedImage& li : _loadedImages) {
        LoadedImageChain chainStart = { nullptr, li };
        recursiveLoadDependents(chainStart);
        if ( _diag.hasError() )
            break;
    }
    assert(_loadedImages.count() == dylibs.count());

    // create an ImageWriter for each cached dylib
    STACK_ALLOC_ARRAY(ImageWriter, writers, _loadedImages.count());
    for (BuilderLoadedImage& li : _loadedImages) {
        writers.push_back(ImageWriter());
        buildImage(writers.back(), li);
    }

    // add initializer order into each dylib
    for (const BuilderLoadedImage& li : _loadedImages) {
        uint32_t index = li.imageNum - _startImageNum;
        computeInitOrder(writers[index], index);
    }

    // add exports patch info for each dylib
    for (const BuilderLoadedImage& li : _loadedImages) {
        uint32_t index = li.imageNum - _startImageNum;
        addCachePatchInfo(writers[index], li);
    }

    // combine all Image objects into one ImageArray
    ImageArrayWriter imageArrayWriter(_startImageNum, (uint32_t)writers.count());
    for (ImageWriter& writer : writers) {
        imageArrayWriter.appendImage(writer.finalize());
        writer.deallocate();
    }
    const ImageArray* imageArray = imageArrayWriter.finalize();

    return imageArray;
}


#if BUILDING_CACHE_BUILDER
const ImageArray* ClosureBuilder::makeOtherDylibsImageArray(const Array<LoadedFileInfo>& otherDylibs, uint32_t cachedDylibsCount)
{
    // because this is run in cache builder using dispatch_apply() there is minimal stack space
    // so set up storage for all arrays to be vm_allocated
    uintptr_t maxImageCount = otherDylibs.count() + cachedDylibsCount + 128;
    _loadedImages.reserve(maxImageCount);
    _dependencies.reserve(maxImageCount*16);

    // build _loadedImages[] with every dylib in cache, followed by others
    _nextIndex = 0;
    for (const LoadedFileInfo& aDylibInfo : otherDylibs)  {
        BuilderLoadedImage entry;
        entry.loadedFileInfo                = aDylibInfo;
        entry.imageNum                      = _startImageNum + _nextIndex++;
        entry.unmapWhenDone                 = false;
        entry.contentRebased                = false;
        entry.hasInits                      = false;
        entry.markNeverUnload               = false;
        entry.rtldLocal                     = false;
        entry.isBadImage                    = false;
        entry.overrideImageNum              = 0;
        _loadedImages.push_back(entry);
    }

    // wire up dependencies between cached dylibs
    // Note, _loadedImages can grow when we call recursiveLoadDependents so we need
    // to check the count on each iteration.
    for (uint64_t index = 0; index != _loadedImages.count(); ++index) {
        BuilderLoadedImage& li = _loadedImages[index];
        LoadedImageChain chainStart = { nullptr, li };
        recursiveLoadDependents(chainStart);
        if ( _diag.hasError() ) {
            _diag.warning("while building dlopen closure for %s: %s", li.loadedFileInfo.path, _diag.errorMessage().c_str());
            //fprintf(stderr, "while building dlopen closure for %s: %s\n", li.loadedFileInfo.path, _diag.errorMessage().c_str());
            _diag.clearError();
            li.isBadImage = true;    // mark bad
        }
    }

    auto invalidateBadImages = [&]() {
        // Invalidate images with bad dependencies
        while (true) {
            bool madeChange = false;
            for (BuilderLoadedImage& li : _loadedImages) {
                if (li.isBadImage) {
                    // Already invalidated
                    continue;
                }
                for (Image::LinkedImage depIndex : li.dependents) {
                    if ( depIndex.imageNum() == kMissingWeakLinkedImage )
                        continue;
                    if ( depIndex.imageNum() < dyld3::closure::kLastDyldCacheImageNum )
                        continue;
                    BuilderLoadedImage& depImage = findLoadedImage(depIndex.imageNum());
                    if (depImage.isBadImage) {
                        _diag.warning("while building dlopen closure for %s: dependent dylib had error", li.loadedFileInfo.path);
                        li.isBadImage = true;    // mark bad
                        madeChange = true;
                    }
                }
            }
            if (!madeChange)
                break;
        }
    };

    invalidateBadImages();

    // create an ImageWriter for each cached dylib
    STACK_ALLOC_ARRAY(ImageWriter, writers, _loadedImages.count());
    for (BuilderLoadedImage& li : _loadedImages) {
        if ( li.imageNum == 0 )  {
            writers.push_back(ImageWriter());
            writers.back().setInvalid();
            continue;
        }
        if ( li.imageNum < dyld3::closure::kLastDyldCacheImageNum )
            continue;
        writers.push_back(ImageWriter());
        buildImage(writers.back(), li);
        if ( _diag.hasError() ) {
            _diag.warning("while building dlopen closure for %s: %s", li.loadedFileInfo.path, _diag.errorMessage().c_str());
            //fprintf(stderr, "while building dlopen closure for %s: %s\n", li.loadedFileInfo.path, _diag.errorMessage().c_str());
            _diag.clearError();
            li.isBadImage = true;    // mark bad
            writers.back().setInvalid();
        }
    }

    invalidateBadImages();

    // add initializer order into each dylib
    for (const BuilderLoadedImage& li : _loadedImages) {
        if ( li.imageNum < dyld3::closure::kLastDyldCacheImageNum )
            continue;
        if (li.isBadImage)
            continue;
        uint32_t index = li.imageNum - _startImageNum;
        computeInitOrder(writers[index], index);
    }

    // combine all Image objects into one ImageArray
    ImageArrayWriter imageArrayWriter(_startImageNum, (uint32_t)writers.count());
    for (ImageWriter& writer : writers) {
        imageArrayWriter.appendImage(writer.finalize());
        writer.deallocate();
    }
    const ImageArray* imageArray = imageArrayWriter.finalize();

    return imageArray;
}
#endif


bool ClosureBuilder::inLoadedImageArray(const Array<LoadedImage>& loadedList, ImageNum imageNum)
{
    for (const LoadedImage& ali : loadedList) {
        if ( ali.image()->representsImageNum(imageNum) )
            return true;
    }
    return false;
}

void ClosureBuilder::buildLoadOrderRecurse(Array<LoadedImage>& loadedList, const Array<const ImageArray*>& imagesArrays, const Image* image)
{
    // breadth first load
    STACK_ALLOC_ARRAY(const Image*, needToRecurse, 256);
    image->forEachDependentImage(^(uint32_t dependentIndex, dyld3::closure::Image::LinkKind kind, ImageNum depImageNum, bool &stop) {
        if ( !inLoadedImageArray(loadedList, depImageNum) ) {
            const Image* depImage = ImageArray::findImage(imagesArrays, depImageNum);
            loadedList.push_back(LoadedImage::make(depImage));
            needToRecurse.push_back(depImage);
        }
    });

    // recurse load
    for (const Image* img : needToRecurse) {
        buildLoadOrderRecurse(loadedList, imagesArrays, img);
    }
}

void ClosureBuilder::buildLoadOrder(Array<LoadedImage>& loadedList, const Array<const ImageArray*>& imagesArrays, const Closure* toAdd)
{
    const dyld3::closure::Image* topImage = ImageArray::findImage(imagesArrays, toAdd->topImage());
	loadedList.push_back(LoadedImage::make(topImage));
	buildLoadOrderRecurse(loadedList, imagesArrays, topImage);
}



} // namespace closure
} // namespace dyld3
