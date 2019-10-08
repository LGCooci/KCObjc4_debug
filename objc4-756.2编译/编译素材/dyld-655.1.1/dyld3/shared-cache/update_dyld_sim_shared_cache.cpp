/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2016 Apple Inc. All rights reserved.
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
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <dirent.h>
#include <rootless.h>
#include <dscsym.h>
#include <dispatch/dispatch.h>
#include <pthread/pthread.h>

#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_set>
#include <iostream>
#include <fstream>

#include "MachOFile.h"
#include "FileUtils.h"
#include "StringUtils.h"
#include "DyldSharedCache.h"
#include "MachOAnalyzer.h"
#include "ClosureFileSystemPhysical.h"



struct MappedMachOsByCategory
{
    std::string                                 archName;
    std::vector<DyldSharedCache::MappedMachO>   dylibsForCache;
    std::vector<DyldSharedCache::MappedMachO>   otherDylibsAndBundles;
    std::vector<DyldSharedCache::MappedMachO>   mainExecutables;
};

static const char* sSearchDirs[] = {
    "/bin",
    "/sbin",
    "/usr",
    "/System",
};

static const char* sSkipDirs[] = {
    "/usr/share",
    "/usr/local/include",
};


static const char* sMacOsAdditions[] = {
    "/usr/lib/system/libsystem_kernel.dylib",
    "/usr/lib/system/libsystem_platform.dylib",
    "/usr/lib/system/libsystem_pthread.dylib",
};


static bool verbose = false;

static bool addIfMachO(const dyld3::closure::FileSystem& fileSystem, const std::string& runtimePath, const struct stat& statBuf, dyld3::Platform platform, std::vector<MappedMachOsByCategory>& files)
{
    bool result = false;
    for (MappedMachOsByCategory& file : files) {
        Diagnostics diag;
        dyld3::closure::LoadedFileInfo loadedFileInfo = dyld3::MachOAnalyzer::load(diag, fileSystem, runtimePath.c_str(), file.archName.c_str(), dyld3::Platform::macOS);
        const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)loadedFileInfo.fileContent;
        if ( ma != nullptr ) {
            bool sipProtected = false; // isProtectedBySIP(fd);
            bool issetuid     = false;
            if ( ma->isDynamicExecutable() ) {
                //fprintf(stderr, "requireSIP=%d, sipProtected=%d, path=%s\n", requireSIP, sipProtected, fullPath.c_str());
                issetuid = (statBuf.st_mode & (S_ISUID|S_ISGID));
                file.mainExecutables.emplace_back(runtimePath, ma, loadedFileInfo.sliceLen, issetuid, sipProtected, loadedFileInfo.sliceOffset, statBuf.st_mtime, statBuf.st_ino);
                result = true;
            }
            else if ( ma->canBePlacedInDyldCache(runtimePath.c_str(), ^(const char* msg) {}) ) {
                file.dylibsForCache.emplace_back(runtimePath, ma, loadedFileInfo.sliceLen, issetuid, sipProtected, loadedFileInfo.sliceOffset, statBuf.st_mtime, statBuf.st_ino);
                result = true;
           }
        }
	}
    return result;
}

static void findAllFiles(const std::string& simRuntimeRootPath, dyld3::Platform platform, std::vector<MappedMachOsByCategory>& files)
{
    std::unordered_set<std::string> skipDirs;
    for (const char* s : sSkipDirs)
        skipDirs.insert(s);

    for (const char* searchDir : sSearchDirs ) {
        dyld3::closure::FileSystemPhysical fileSystem(simRuntimeRootPath.c_str());
        iterateDirectoryTree(simRuntimeRootPath, searchDir, ^(const std::string& dirPath) { return (skipDirs.count(dirPath) != 0); }, ^(const std::string& path, const struct stat& statBuf) {
            // ignore files that don't have 'x' bit set (all runnable mach-o files do)
            const bool hasXBit = ((statBuf.st_mode & S_IXOTH) == S_IXOTH);
            if ( !hasXBit && !endsWith(path, ".dylib") )
                return;

            // ignore files too small
            if ( statBuf.st_size < 0x3000 )
                return;

            // if the file is mach-o add to list
            addIfMachO(fileSystem, path, statBuf, platform, files);
         });
    }
}

static void addMacOSAdditions(std::vector<MappedMachOsByCategory>& allFileSets)
{
    dyld3::closure::FileSystemPhysical fileSystem;
    for (const char* addPath : sMacOsAdditions) {
        struct stat statBuf;
        if ( stat(addPath, &statBuf) == 0 ) {
            addIfMachO(fileSystem, addPath, statBuf, dyld3::Platform::macOS, allFileSets);
        }
    }
}


static bool dontCache(const std::string& simRuntimeRootPath, const std::string& archName,
                      const std::unordered_set<std::string>& pathsWithDuplicateInstallName,
                      const DyldSharedCache::MappedMachO& aFile, bool warn)
{
    if ( startsWith(aFile.runtimePath, "/usr/lib/system/introspection/") )
        return true;
    if ( startsWith(aFile.runtimePath, "/usr/local/") )
        return true;

    // anything inside a .app bundle is specific to app, so should be in shared cache
    if ( aFile.runtimePath.find(".app/") != std::string::npos )
        return true;

    if ( aFile.runtimePath.find("//") != std::string::npos ) {
        if (warn) fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s double-slash in install name %s\n", archName.c_str(), aFile.runtimePath.c_str());
    }

    const char* installName = aFile.mh->installName();
    if ( (pathsWithDuplicateInstallName.count(aFile.runtimePath) != 0) && (aFile.runtimePath != installName) ) {
        if (warn) fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s skipping because of duplicate install name %s\n", archName.c_str(), aFile.runtimePath.c_str());
        return true;
    }

    if ( aFile.runtimePath != installName ) {
        // see if install name is a symlink to actual path
        std::string fullInstall = simRuntimeRootPath + installName;
        char resolvedPath[PATH_MAX];
        if ( realpath(fullInstall.c_str(), resolvedPath) != NULL ) {
            std::string resolvedSymlink = resolvedPath;
            if ( !simRuntimeRootPath.empty() ) {
                resolvedSymlink = resolvedSymlink.substr(simRuntimeRootPath.size());
            }
            if ( aFile.runtimePath == resolvedSymlink ) {
                return false;
            }
        }
        if (warn) fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s skipping because of bad install name %s\n", archName.c_str(), aFile.runtimePath.c_str());
        return true;
    }

    return false;
}

static void pruneCachedDylibs(const std::string& simRuntimeRootPath, MappedMachOsByCategory& fileSet)
{
    std::unordered_set<std::string> pathsWithDuplicateInstallName;

    std::unordered_map<std::string, std::string> installNameToFirstPath;
    for (DyldSharedCache::MappedMachO& aFile : fileSet.dylibsForCache) {
        //fprintf(stderr, "dylib: %s\n", aFile.runtimePath.c_str());
        const char* installName = aFile.mh->installName();
        auto pos = installNameToFirstPath.find(installName);
        if ( pos == installNameToFirstPath.end() ) {
            installNameToFirstPath[installName] = aFile.runtimePath;
        }
        else {
            pathsWithDuplicateInstallName.insert(aFile.runtimePath);
            pathsWithDuplicateInstallName.insert(installNameToFirstPath[installName]);
        }
    }

    for (DyldSharedCache::MappedMachO& aFile : fileSet.dylibsForCache) {
        if ( dontCache(simRuntimeRootPath, fileSet.archName, pathsWithDuplicateInstallName, aFile, true) )
            fileSet.otherDylibsAndBundles.push_back(aFile);
     }
    fileSet.dylibsForCache.erase(std::remove_if(fileSet.dylibsForCache.begin(), fileSet.dylibsForCache.end(),
        [&](const DyldSharedCache::MappedMachO& aFile) { return dontCache(simRuntimeRootPath, fileSet.archName, pathsWithDuplicateInstallName, aFile, false); }),
        fileSet.dylibsForCache.end());
}

static bool existingCacheUpToDate(const std::string& existingCache, const std::vector<DyldSharedCache::MappedMachO>& currentDylibs)
{
    // if no existing cache, it is not up-to-date
    int fd = ::open(existingCache.c_str(), O_RDONLY);
    if ( fd < 0 )
        return false;

    // build map of found dylibs
    std::unordered_map<std::string, const DyldSharedCache::MappedMachO*> currentDylibMap;
    for (const DyldSharedCache::MappedMachO& aFile : currentDylibs) {
        //fprintf(stderr, "0x%0llX 0x%0llX  %s\n", aFile.inode, aFile.modTime, aFile.runtimePath.c_str());
        currentDylibMap[aFile.runtimePath] = &aFile;
    }

    // make sure all dylibs in existing cache have same mtime and inode as found dylib
    __block bool foundMismatch = false;
    const uint64_t cacheMapLen = 0x40000000;
    void *p = ::mmap(NULL, cacheMapLen, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( p != MAP_FAILED ) {
        const DyldSharedCache* cache = (DyldSharedCache*)p;
        cache->forEachImageEntry(^(const char* installName, uint64_t mTime, uint64_t inode) {
            bool foundMatch = false;
            auto pos = currentDylibMap.find(installName);
            if ( pos != currentDylibMap.end() ) {
                const DyldSharedCache::MappedMachO* foundDylib = pos->second;
                if ( (foundDylib->inode == inode) && (foundDylib->modTime == mTime) ) {
                    foundMatch = true;
                }
            }
            if ( !foundMatch ) {
                // use slow path and look for any dylib with a matching inode and mtime
                bool foundSlow = false;
                for (const DyldSharedCache::MappedMachO& aFile : currentDylibs) {
                    if ( (aFile.inode == inode) && (aFile.modTime == mTime) ) {
                        foundSlow = true;
                        break;
                    }
                }
                if ( !foundSlow ) {
                    foundMismatch = true;
                    if ( verbose )
                        fprintf(stderr, "rebuilding dyld cache because dylib changed: %s\n", installName);
                }
            }
         });
        ::munmap(p, cacheMapLen);
    }

    ::close(fd);

    return !foundMismatch;
}


inline uint32_t absolutetime_to_milliseconds(uint64_t abstime)
{
    return (uint32_t)(abstime/1000/1000);
}


#define TERMINATE_IF_LAST_ARG( s )      \
    do {                                \
        if ( i == argc - 1 ) {          \
            fprintf(stderr, s );        \
            return 1;                   \
        }                               \
    } while ( 0 )

int main(int argc, const char* argv[])
{
    std::string                     rootPath;
    std::string                     dylibListFile;
    bool                            force = false;
    std::string                     cacheDir;
    std::unordered_set<std::string> archStrs;

    dyld3::Platform platform = dyld3::Platform::iOS;

    // parse command line options
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "-debug") == 0) {
            verbose = true;
        }
        else if (strcmp(arg, "-verbose") == 0) {
            verbose = true;
        }
        else if (strcmp(arg, "-tvOS") == 0) {
            platform = dyld3::Platform::tvOS_simulator;
        }
        else if (strcmp(arg, "-iOS") == 0) {
            platform = dyld3::Platform::iOS_simulator;
        }
        else if (strcmp(arg, "-watchOS") == 0) {
            platform = dyld3::Platform::watchOS_simulator;
        }
        else if ( strcmp(arg, "-runtime_dir") == 0 ) {
            TERMINATE_IF_LAST_ARG("-runtime_dir missing path argument\n");
            rootPath = argv[++i];
        }
        else if (strcmp(arg, "-cache_dir") == 0) {
            TERMINATE_IF_LAST_ARG("-cache_dir missing path argument\n");
            cacheDir = argv[++i];
        }
        else if (strcmp(arg, "-arch") == 0) {
            TERMINATE_IF_LAST_ARG("-arch missing argument\n");
            archStrs.insert(argv[++i]);
        }
        else if (strcmp(arg, "-force") == 0) {
            force = true;
        }
        else {
            //usage();
            fprintf(stderr, "update_dyld_sim_shared_cache: unknown option: %s\n", arg);
            return 1;
        }
    }

    if ( cacheDir.empty() ) {
        fprintf(stderr, "missing -cache_dir <path> option to specify directory in which to write cache file(s)\n");
        return 1;
    }

    if ( rootPath.empty() ) {
        fprintf(stderr, "missing -runtime_dir <path> option to specify directory which is root of simulator runtime)\n");
        return 1;
    }
    else {
        // canonicalize rootPath
        char resolvedPath[PATH_MAX];
        if ( realpath(rootPath.c_str(), resolvedPath) != NULL ) {
            rootPath = resolvedPath;
        }
    }

    int err = mkpath_np(cacheDir.c_str(), S_IRWXU | S_IRGRP|S_IXGRP | S_IROTH|S_IXOTH);
    if ( (err != 0) && (err != EEXIST) ) {
        fprintf(stderr, "mkpath_np fail: %d", err);
        return 1;
    }

    if ( archStrs.empty() ) {
        switch ( platform ) {
            case dyld3::Platform::iOS_simulator:
                archStrs.insert("x86_64");
                break;
            case dyld3::Platform::tvOS_simulator:
                archStrs.insert("x86_64");
                break;
            case dyld3::Platform::watchOS_simulator:
                archStrs.insert("i386");
                break;
             case dyld3::Platform::macOS:
                assert(0 && "macOS does not have a simulator");
                break;
             case dyld3::Platform::bridgeOS:
                assert(0 && "bridgeOS does not have a simulator");
                break;
             case dyld3::Platform::iOS:
             case dyld3::Platform::tvOS:
             case dyld3::Platform::watchOS:
             case dyld3::Platform::iOSMac:
             case dyld3::Platform::unknown:
                assert(0 && "invalid platform");
                break;
       }
    }

    uint64_t t1 = mach_absolute_time();

    // find all mach-o files for requested architectures
    __block std::vector<MappedMachOsByCategory> allFileSets;
    if ( archStrs.count("x86_64") )
        allFileSets.push_back({"x86_64"});
    if ( archStrs.count("i386") )
        allFileSets.push_back({"i386"});
    findAllFiles(rootPath, platform, allFileSets);
    addMacOSAdditions(allFileSets);
    for (MappedMachOsByCategory& fileSet : allFileSets) {
        pruneCachedDylibs(rootPath, fileSet);
    }

    uint64_t t2 = mach_absolute_time();

    fprintf(stderr, "time to scan file system and construct lists of mach-o files: %ums\n", absolutetime_to_milliseconds(t2-t1));

    // build all caches in parallel
    __block bool cacheBuildFailure = false;
    dispatch_apply(allFileSets.size(), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^(size_t index) {
        MappedMachOsByCategory& fileSet = allFileSets[index];
        const std::string outFile = cacheDir + "/dyld_shared_cache_" + fileSet.archName;
        __block std::unordered_set<std::string> knownMissingDylib;

        DyldSharedCache::MappedMachO (^loader)(const std::string&) = ^DyldSharedCache::MappedMachO(const std::string& runtimePath) {
            std::string fullPath = rootPath + runtimePath;
            struct stat statBuf;
            if ( stat(fullPath.c_str(), &statBuf) == 0 ) {
                std::vector<MappedMachOsByCategory> mappedFiles;
                mappedFiles.push_back({fileSet.archName});
                dyld3::closure::FileSystemPhysical fileSystem(rootPath.c_str());
                if ( addIfMachO(fileSystem, runtimePath, statBuf, platform, mappedFiles) ) {
                    if ( !mappedFiles.back().dylibsForCache.empty() )
                        return mappedFiles.back().dylibsForCache.back();
                }
            }
            if ( knownMissingDylib.count(runtimePath) == 0 ) {
                fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s could not use in dylid cache: %s\n", fileSet.archName.c_str(), runtimePath.c_str());
                knownMissingDylib.insert(runtimePath);
            }
            return DyldSharedCache::MappedMachO();
        };
        size_t startCount = fileSet.dylibsForCache.size();
        std::vector<std::pair<DyldSharedCache::MappedMachO, std::set<std::string>>> excludes;
        DyldSharedCache::verifySelfContained(fileSet.dylibsForCache, loader, excludes);
        for (size_t i=startCount; i < fileSet.dylibsForCache.size(); ++i) {
            fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s not found in initial scan, but adding required dylib %s\n", fileSet.archName.c_str(), fileSet.dylibsForCache[i].runtimePath.c_str());
        }
        for (auto& exclude : excludes) {
            std::string reasons = "(\"";
            for (auto i = exclude.second.begin(); i != exclude.second.end(); ++i) {
                reasons += *i;
                if (i != --exclude.second.end()) {
                    reasons += "\", \"";
                }
            }
            reasons += "\")";
            fprintf(stderr, "update_dyld_shared_cache: warning: %s rejected from cached dylibs: %s (%s)\n", fileSet.archName.c_str(), exclude.first.runtimePath.c_str(), reasons.c_str());
            fileSet.otherDylibsAndBundles.push_back(exclude.first);
        }

        // check if cache is already up to date
        if ( !force ) {
            if ( existingCacheUpToDate(outFile, fileSet.dylibsForCache) )
                return;
        }
        fprintf(stderr, "make %s cache with %lu dylibs, %lu other dylibs, %lu programs\n", fileSet.archName.c_str(), fileSet.dylibsForCache.size(), fileSet.otherDylibsAndBundles.size(), fileSet.mainExecutables.size());

        // build cache new cache file
        DyldSharedCache::CreateOptions options;
        options.outputFilePath               = outFile;
        options.outputMapFilePath            = cacheDir + "/dyld_shared_cache_" + fileSet.archName + ".map";
        options.archName                     = fileSet.archName;
        options.platform                     = platform;
        options.excludeLocalSymbols          = false;
        options.optimizeStubs                = false;
        options.optimizeObjC                 = true;
        options.codeSigningDigestMode        = DyldSharedCache::SHA256only;
        options.dylibsRemovedDuringMastering = false;
        options.inodesAreSameAsRuntime       = true;
        options.cacheSupportsASLR            = false;
        options.forSimulator                 = true;
        options.isLocallyBuiltCache          = true;
        options.verbose                      = verbose;
        options.evictLeafDylibsOnOverflow    = true;
        options.pathPrefixes                 = { rootPath };
        DyldSharedCache::CreateResults results = DyldSharedCache::create(options, fileSet.dylibsForCache, fileSet.otherDylibsAndBundles, fileSet.mainExecutables);

        // print any warnings
        for (const std::string& warn : results.warnings) {
            fprintf(stderr, "update_dyld_shared_cache: warning: %s %s\n", fileSet.archName.c_str(), warn.c_str());
        }
        if ( !results.errorMessage.empty() ) {
            fprintf(stderr, "update_dyld_shared_cache: %s\n", results.errorMessage.c_str());
            cacheBuildFailure = true;
        }
    });

    // we could unmap all input files, but tool is about to quit

    return (cacheBuildFailure ? 1 : 0);
}

