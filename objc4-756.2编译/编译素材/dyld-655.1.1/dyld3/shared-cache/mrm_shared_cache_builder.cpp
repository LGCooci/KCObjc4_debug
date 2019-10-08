/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
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

#include "mrm_shared_cache_builder.h"
#include "CacheBuilder.h"
#include "ClosureFileSystem.h"
#include "FileUtils.h"
#include <pthread.h>
#include <memory>
#include <vector>
#include <map>

static const uint64_t kMinBuildVersion = 1; //The minimum version BuildOptions struct we can support
static const uint64_t kMaxBuildVersion = 1; //The maximum version BuildOptions struct we can support

namespace dyld3 {
namespace closure {

struct FileInfo {
    const char*     path;
    const uint8_t*  data;
    const uint64_t  length;
    FileFlags       flags;
    uint64_t        mtime;
    uint64_t        inode;
};

class FileSystemMRM : public FileSystem {
public:
    FileSystemMRM() : FileSystem() { }

    bool getRealPath(const char possiblePath[MAXPATHLEN], char realPath[MAXPATHLEN]) const override {
        Diagnostics diag;
        std::string resolvedPath = symlinkResolver.realPath(diag, possiblePath);
        if (diag.hasError()) {
            diag.verbose("MRM error: %s\n", diag.errorMessage().c_str());
            diag.clearError();
            return false;
        }

        // FIXME: Should we only return real paths of files which point to macho's?  For now that is what we are doing
        auto it = fileMap.find(resolvedPath);
        if (it == fileMap.end())
            return false;

        memcpy(realPath, resolvedPath.c_str(), std::min((size_t)MAXPATHLEN, resolvedPath.size() + 1));
        return true;
    }

    bool loadFile(const char* path, LoadedFileInfo& info, char realerPath[MAXPATHLEN], void (^error)(const char* format, ...)) const override {
        Diagnostics diag;
        std::string resolvedPath = symlinkResolver.realPath(diag, path);
        if (diag.hasError()) {
            diag.verbose("MRM error: %s\n", diag.errorMessage().c_str());
            diag.clearError();
            return false;
        }

        auto it = fileMap.find(resolvedPath);
        if (it == fileMap.end())
            return false;

        if (resolvedPath == path)
            realerPath[0] = '\0';
        else
            memcpy(realerPath, resolvedPath.c_str(), std::min((size_t)MAXPATHLEN, resolvedPath.size() + 1));

        // The file exists at this exact path.  Lets use it!
        const FileInfo& fileInfo = files[it->second];

        info.fileContent                = fileInfo.data;
        info.fileContentLen             = fileInfo.length;
        info.sliceOffset                = 0;
        info.sliceLen                   = fileInfo.length;
        info.inode                      = fileInfo.inode;
        info.mtime                      = fileInfo.mtime;
        info.unload                     = nullptr;
        info.path                       = path;
        return true;
    }

    void unloadFile(const LoadedFileInfo& info) const override {
        if (info.unload)
            info.unload(info);
    }

    void unloadPartialFile(LoadedFileInfo& info, uint64_t keepStartOffset, uint64_t keepLength) const override {
        // Note we don't actually unload the data here, but we do want to update the offsets for other data structures to track where we are
        info.fileContent = (const void*)((char*)info.fileContent + keepStartOffset);
        info.fileContentLen = keepLength;
    }

    bool fileExists(const char* path, uint64_t* inode=nullptr, uint64_t* mtime=nullptr, bool* issetuid=nullptr) const override {
        Diagnostics diag;
        std::string resolvedPath = symlinkResolver.realPath(diag, path);
        if (diag.hasError()) {
            diag.verbose("MRM error: %s\n", diag.errorMessage().c_str());
            diag.clearError();
            return false;
        }

        auto it = fileMap.find(resolvedPath);
        if (it == fileMap.end())
            return false;

        // The file exists at this exact path.  Lets use it!
        const FileInfo& fileInfo = files[it->second];
        if (inode)
            *inode = fileInfo.inode;
        if (mtime)
            *mtime = fileInfo.mtime;
        if (issetuid)
            *issetuid = false;
        return true;
    }

    // MRM file APIs
    bool addFile(const char* path, uint8_t* data, uint64_t size, Diagnostics& diag, FileFlags fileFlags) {
        auto iteratorAndInserted = fileMap.insert(std::make_pair(path, files.size()));
        if (!iteratorAndInserted.second) {
            diag.error("Already have content for path: '%s'", path);
            return false;
        }

        symlinkResolver.addFile(diag, path);
        if (diag.hasError())
            return false;

        // on iOS, inode is used to hold hash of path
        uint64_t hash = 0;
        for (const char* s = path; *s != '\0'; ++s)
            hash += hash*4 + *s;
        uint64_t inode = hash;
        uint64_t mtime = 0;

        files.push_back((FileInfo){ path, data, size, fileFlags, mtime, inode });
        return true;
    }

    bool addSymlink(const char* fromPath, const char* toPath, Diagnostics& diag) {
        symlinkResolver.addSymlink(diag, fromPath, toPath);
        return !diag.hasError();
    }

    void forEachFileInfo(std::function<void(const char* path, FileFlags fileFlags)> lambda) {
        for (const FileInfo& fileInfo : files)
            lambda(fileInfo.path, fileInfo.flags);
    }

    size_t fileCount() const {
        return files.size();
    }

    std::vector<DyldSharedCache::FileAlias> getResolvedSymlinks(Diagnostics& diag) {
        return symlinkResolver.getResolvedSymlinks(diag);
    }

private:
    std::vector<FileInfo> files;
    std::map<std::string, uint64_t> fileMap;
    SymlinkResolver symlinkResolver;
};

} // namespace closure
} // namespace dyld3

struct BuildInstance {
    std::unique_ptr<DyldSharedCache::CreateOptions> options;
    std::unique_ptr<CacheBuilder>                   builder;
    std::vector<CacheBuilder::InputFile>            inputFiles;
    std::vector<const char*>                        errors;
    std::vector<const char*>                        warnings;
    std::vector<std::string>                        errorStrings;   // Owns the data for the errors
    std::vector<std::string>                        warningStrings; // Owns the data for the warnings
    uint8_t*                                        cacheData       = nullptr;
    uint64_t                                        cacheSize       = 0;
    uint8_t*                                        cacheMapData    = nullptr;
    uint64_t                                        cacheMapSize    = 0;
    std::string                                     cdHash;         // Owns the data for the cdHash
};

struct BuildFileResult {
    std::string                                 path;
    const uint8_t*                              data;
    uint64_t                                    size;
};

struct SharedCacheBuilder {
    SharedCacheBuilder(const BuildOptions_v1* options);
    const BuildOptions_v1*          options;
    dyld3::closure::FileSystemMRM   fileSystem;

    std::string dylibOrderFileData;
    std::string dirtyDataOrderFileData;

    // An array of builders and their options as we may have more than one builder for a given device variant.
    std::vector<BuildInstance> builders;

    // The results from all of the builders
    // We keep this in a vector to own the data.
    std::vector<BuildFileResult> fileResults;

    std::vector<std::string> errors;
    pthread_mutex_t lock;

    enum State {
        AcceptingFiles,
        Building,
        FinishedBuilding
    };

    State state = AcceptingFiles;

    void runSync(void (^block)()) {
        pthread_mutex_lock(&lock);
        block();
        pthread_mutex_unlock(&lock);
    }

    __attribute__((format(printf, 2, 3)))
    void error(const char* format, ...) {
        va_list list;
        va_start(list, format);
        Diagnostics diag;
        diag.error(format, list);
        va_end(list);

        errors.push_back(diag.errorMessage());
    }
};

SharedCacheBuilder::SharedCacheBuilder(const BuildOptions_v1* options) : options(options), lock(PTHREAD_MUTEX_INITIALIZER) {

}

void validiateBuildOptions(const BuildOptions_v1* options, SharedCacheBuilder& builder) {
    if (options->version < kMinBuildVersion) {
        builder.error("Builder version %llu is less than minimum supported version of %llu", options->version, kMinBuildVersion);
    }
    if (options->version > kMaxBuildVersion) {
        builder.error("Builder version %llu is greater than maximum supported version of %llu", options->version, kMaxBuildVersion);
    }
    if (!options->updateName) {
        builder.error("updateName must not be null");
    }
    if (!options->deviceName) {
        builder.error("deviceName must not be null");
    }
    switch (options->disposition) {
        case Disposition::Unknown:
        case Disposition::InternalDevelopment:
        case Disposition::Customer:
            break;
        default:
            builder.error("unknown disposition value");
            break;
    }
    switch (options->platform) {
        case Platform::unknown:
            builder.error("platform must not be unknown");
            break;
        case Platform::macOS:
        case Platform::iOS:
        case Platform::tvOS:
        case Platform::watchOS:
        case Platform::bridgeOS:
        case Platform::iOSMac:
        case Platform::iOS_simulator:
        case Platform::tvOS_simulator:
        case Platform::watchOS_simulator:
            break;
        default:
            builder.error("unknown platform value");
            break;
    }
    if (!options->archs) {
        builder.error("archs must not be null");
    }
    if (!options->numArchs) {
        builder.error("numArchs must not be 0");
    }
}

struct SharedCacheBuilder* createSharedCacheBuilder(const BuildOptions_v1* options) {
    SharedCacheBuilder* builder = new SharedCacheBuilder(options);

    // Check the option struct values are valid
    validiateBuildOptions(options, *builder);

    return builder;
}

bool addFile(struct SharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, FileFlags fileFlags) {
    __block bool success = false;
    builder->runSync(^() {
        if (builder->state != SharedCacheBuilder::AcceptingFiles) {
            builder->error("Cannot add file: '%s' as we have already started building", path);
            return;
        }
        size_t pathLength = strlen(path);
        if (pathLength == 0) {
            builder->error("Empty path");
            return;
        }
        if (pathLength >= MAXPATHLEN) {
            builder->error("Path is too long: '%s'", path);
            return;
        }
        if (data == nullptr) {
            builder->error("Data cannot be null for file: '%s'", path);
            return;
        }
        switch (fileFlags) {
            case NoFlags:
            case MustBeInCache:
            case ShouldBeExcludedFromCacheIfUnusedLeaf:
            case RequiredClosure:
                break;
            case DylibOrderFile:
                builder->dylibOrderFileData = std::string((char*)data, size);
                success = true;
                return;
            case DirtyDataOrderFile:
                builder->dirtyDataOrderFileData = std::string((char*)data, size);
                success = true;
                return;
            default:
                builder->error("unknown file flags value");
                break;
        }
        Diagnostics diag;
        if (!builder->fileSystem.addFile(path, data, size, diag, fileFlags)) {
            builder->errors.push_back(diag.errorMessage());
            return;
        }
        success = true;
    });
    return success;
}

bool addSymlink(struct SharedCacheBuilder* builder, const char* fromPath, const char* toPath) {
    __block bool success = false;
    builder->runSync(^() {
        if (builder->state != SharedCacheBuilder::AcceptingFiles) {
            builder->error("Cannot add file: '%s' as we have already started building", fromPath);
            return;
        }
        size_t pathLength = strlen(fromPath);
        if (pathLength == 0) {
            builder->error("Empty path");
            return;
        }
        if (pathLength >= MAXPATHLEN) {
            builder->error("Path is too long: '%s'", fromPath);
            return;
        }
        Diagnostics diag;
        if (!builder->fileSystem.addSymlink(fromPath, toPath, diag)) {
            builder->errors.push_back(diag.errorMessage());
            return;
        }
        success = true;
    });
    return success;
}

static bool platformExcludeLocalSymbols(Platform platform) {
    switch (platform) {
        case Platform::unknown:
        case Platform::macOS:
            return false;
        case Platform::iOS:
        case Platform::tvOS:
        case Platform::watchOS:
        case Platform::bridgeOS:
            return true;
        case Platform::iOSMac:
        case Platform::iOS_simulator:
        case Platform::tvOS_simulator:
        case Platform::watchOS_simulator:
            return false;
    }
}

static DyldSharedCache::CodeSigningDigestMode platformCodeSigningDigestMode(Platform platform) {
    switch (platform) {
        case Platform::unknown:
        case Platform::macOS:
        case Platform::iOS:
        case Platform::tvOS:
            return DyldSharedCache::SHA256only;
        case Platform::watchOS:
            return DyldSharedCache::Agile;
        case Platform::bridgeOS:
        case Platform::iOSMac:
        case Platform::iOS_simulator:
        case Platform::tvOS_simulator:
        case Platform::watchOS_simulator:
            return DyldSharedCache::SHA256only;
    }
}

static bool platformIsForSimulator(Platform platform) {
    switch (platform) {
        case Platform::unknown:
        case Platform::macOS:
        case Platform::iOS:
        case Platform::tvOS:
        case Platform::watchOS:
        case Platform::bridgeOS:
        case Platform::iOSMac:
            return false;
        case Platform::iOS_simulator:
        case Platform::tvOS_simulator:
        case Platform::watchOS_simulator:
            return true;
    }
}

static const char* dispositionName(Disposition disposition) {
    switch (disposition) {
        case Disposition::Unknown:
            return "";
        case Disposition::InternalDevelopment:
            return "Internal";
        case Disposition::Customer:
            return "Customer";
        case Disposition::InternalMinDevelopment:
            return "InternalMinDevelopment";
    }
}

bool runSharedCacheBuilder(struct SharedCacheBuilder* builder) {
    __block bool success = false;
    builder->runSync(^() {
        if (builder->state != SharedCacheBuilder::AcceptingFiles) {
            builder->error("Builder has already been run");
            return;
        }
        builder->state = SharedCacheBuilder::Building;
        if (builder->fileSystem.fileCount() == 0) {
            builder->error("Cannot run builder with no files");
        }

        Diagnostics diag;
        std::vector<DyldSharedCache::FileAlias> aliases = builder->fileSystem.getResolvedSymlinks(diag);
        if (diag.hasError()) {
            diag.verbose("Symlink resolver error: %s\n", diag.errorMessage().c_str());
        }

        if (!builder->errors.empty()) {
            builder->error("Skipping running shared cache builder due to previous errors");
            return;
        }

        __block std::vector<CacheBuilder::InputFile> inputFiles;
        builder->fileSystem.forEachFileInfo(^(const char* path, FileFlags fileFlags) {
            CacheBuilder::InputFile::State state = CacheBuilder::InputFile::Unset;
            switch (fileFlags) {
                case FileFlags::NoFlags:
                    state = CacheBuilder::InputFile::Unset;
                    break;
                case FileFlags::MustBeInCache:
                    state = CacheBuilder::InputFile::MustBeIncluded;
                    break;
                case FileFlags::ShouldBeExcludedFromCacheIfUnusedLeaf:
                    state = CacheBuilder::InputFile::MustBeExcludedIfUnused;
                    break;
                case FileFlags::RequiredClosure:
                    state = CacheBuilder::InputFile::MustBeIncluded;
                    break;
                case FileFlags::DylibOrderFile:
                case FileFlags::DirtyDataOrderFile:
                    builder->error("Order files should not be in the file system");
                    return;
            }
            inputFiles.emplace_back((CacheBuilder::InputFile){ path, state });
        });

        auto addCacheConfiguration = ^(bool isOptimized) {
            for (uint64_t i = 0; i != builder->options->numArchs; ++i) {
                auto options = std::make_unique<DyldSharedCache::CreateOptions>((DyldSharedCache::CreateOptions){});
                const char *cacheSuffix = (isOptimized ? "" : ".development");
                std::string runtimePath = (builder->options->platform == Platform::macOS) ? "/private/var/db/dyld/" : "/System/Library/Caches/com.apple.dyld/";
                options->outputFilePath = runtimePath + "dyld_shared_cache_" + builder->options->archs[i] + cacheSuffix;
                options->outputMapFilePath = options->outputFilePath + ".map";
                options->archName = builder->options->archs[i];
                options->platform = (dyld3::Platform)builder->options->platform;
                options->excludeLocalSymbols = platformExcludeLocalSymbols(builder->options->platform);
                options->optimizeStubs = isOptimized;
                options->optimizeObjC = true;
                options->codeSigningDigestMode = platformCodeSigningDigestMode(builder->options->platform);
                options->dylibsRemovedDuringMastering = true;
                options->inodesAreSameAsRuntime = false;
                options->cacheSupportsASLR = true;
                options->forSimulator = platformIsForSimulator(builder->options->platform);
                options->isLocallyBuiltCache = builder->options->isLocallyBuiltCache;
                options->verbose = builder->options->verboseDiagnostics;
                options->evictLeafDylibsOnOverflow = true;
                options->loggingPrefix = std::string(builder->options->deviceName) + dispositionName(builder->options->disposition) + "." + builder->options->archs[i] + cacheSuffix;
                options->pathPrefixes = { "" };
                options->dylibOrdering = parseOrderFile(builder->dylibOrderFileData);
                options->dirtyDataSegmentOrdering = parseOrderFile(builder->dirtyDataOrderFileData);

                auto cacheBuilder = std::make_unique<CacheBuilder>(*options.get(), builder->fileSystem);
                builder->builders.emplace_back((BuildInstance) { std::move(options), std::move(cacheBuilder), inputFiles });
            }
        };

        // Enqueue a cache for each configuration
        switch (builder->options->disposition) {
            case Disposition::Unknown:
            case Disposition::InternalDevelopment:
                addCacheConfiguration(false);
                addCacheConfiguration(true);
                break;
            case Disposition::Customer:
                addCacheConfiguration(true);
            case Disposition::InternalMinDevelopment:
                addCacheConfiguration(false);
        }

        // FIXME: This step can run in parallel.
        for (auto& buildInstance : builder->builders) {
            CacheBuilder* builder = buildInstance.builder.get();
            builder->build(buildInstance.inputFiles, aliases);

            // First put the warnings in to a vector to own them.
            buildInstance.warningStrings.reserve(builder->warnings().size());
            for (const std::string& warning : builder->warnings())
                buildInstance.warningStrings.push_back(warning);

            // Then copy to a vector to reference the owner
            buildInstance.warnings.reserve(buildInstance.warningStrings.size());
            for (const std::string& warning : buildInstance.warningStrings)
                buildInstance.warnings.push_back(warning.c_str());

            if (!builder->errorMessage().empty()) {
                // First put the errors in to a vector to own them.
                buildInstance.errorStrings.push_back(builder->errorMessage());

                // Then copy to a vector to reference the owner
                buildInstance.errors.reserve(buildInstance.errorStrings.size());
                for (const std::string& error : buildInstance.errorStrings)
                    buildInstance.errors.push_back(error.c_str());
            }

            if (builder->errorMessage().empty()) {
                builder->writeBuffer(buildInstance.cacheData, buildInstance.cacheSize);
                builder->writeMapFileBuffer(buildInstance.cacheMapData, buildInstance.cacheMapSize);
                buildInstance.cdHash = builder->cdHashFirst();
            }
        }

        // Now that we have run all of the builds, collect the results
        // First push file results for each of the shared caches we built
        __block std::map<std::string, uint32_t> dylibsInCaches;
        for (auto& buildInstance : builder->builders) {
            CacheBuilder* cacheBuilder = buildInstance.builder.get();
            if (!cacheBuilder->errorMessage().empty())
                continue;
            builder->fileResults.push_back((BuildFileResult) { buildInstance.options->outputFilePath, buildInstance.cacheData, buildInstance.cacheSize });
            builder->fileResults.push_back((BuildFileResult) { buildInstance.options->outputMapFilePath, buildInstance.cacheMapData, buildInstance.cacheMapSize });

            cacheBuilder->forEachCacheDylib(^(const std::string &path) {
                ++dylibsInCaches[path];
            });
        }

        // Add entries to tell us to remove all of the dylibs from disk which are in every cache.
        const size_t numCaches = builder->builders.size();
        for (const auto& dylibAndCount : dylibsInCaches) {
            if (dylibAndCount.second == numCaches)
                builder->fileResults.push_back((BuildFileResult) { dylibAndCount.first, nullptr, 0 });
        }

        builder->state = SharedCacheBuilder::FinishedBuilding;
        success = true;
    });
    return success;
}

uint64_t getErrorCount(const struct SharedCacheBuilder* builder) {
    return builder->errors.size();
}

const char* getError(const struct SharedCacheBuilder* builder, uint64_t errorIndex) {
    if (errorIndex >= builder->errors.size())
        return nullptr;
    return builder->errors[errorIndex].c_str();
}

uint64_t getCacheResultCount(const struct SharedCacheBuilder* builder) {
    return builder->builders.size();
}

void getCacheResult(struct SharedCacheBuilder* builder, uint64_t cacheIndex, BuildResult* result) {
    if (cacheIndex >= builder->builders.size())
        return;

    BuildInstance& buildInstance = builder->builders[cacheIndex];

    result->version         = 1;
    result->loggingPrefix   = buildInstance.options->loggingPrefix.c_str();
    result->warnings        = buildInstance.warnings.empty() ? nullptr : buildInstance.warnings.data();
    result->numWarnings     = buildInstance.warnings.size();
    result->errors          = buildInstance.errors.empty() ? nullptr : buildInstance.errors.data();
    result->numErrors       = buildInstance.errors.size();
    result->sharedCachePath = buildInstance.options->outputFilePath.c_str();
    result->cdHash          = buildInstance.cdHash.c_str();
}

uint64_t getFileResultCount(const struct SharedCacheBuilder* builder) {
    return builder->fileResults.size();
}

void getFileResult(struct SharedCacheBuilder* builder, uint64_t fileIndex, FileResult* result) {
    if (fileIndex >= builder->fileResults.size())
        return;
    const BuildFileResult& buildFileResult = builder->fileResults[fileIndex];
    *result = (FileResult) { buildFileResult.path.c_str(), buildFileResult.data, buildFileResult.size };
}

void destroySharedCacheBuilder(struct SharedCacheBuilder* builder) {
    delete builder;
}
