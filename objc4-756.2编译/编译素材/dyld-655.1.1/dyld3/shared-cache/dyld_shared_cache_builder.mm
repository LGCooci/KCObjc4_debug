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
#include <sys/resource.h>
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
#include <sys/uio.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <dirent.h>
#include <libgen.h>
#include <pthread.h>
#include <fts.h>

#include <vector>
#include <array>
#include <set>
#include <map>
#include <unordered_set>
#include <algorithm>

#include <spawn.h>

#include <Bom/Bom.h>

#include "Manifest.h"
#include "Diagnostics.h"
#include "DyldSharedCache.h"
#include "BuilderUtils.h"
#include "FileUtils.h"
#include "JSONWriter.h"
#include "StringUtils.h"
#include "mrm_shared_cache_builder.h"

#if !__has_feature(objc_arc)
#error The use of libdispatch in this files requires it to be compiled with ARC in order to avoid leaks
#endif

extern char** environ;

static dispatch_queue_t build_queue;

int runCommandAndWait(Diagnostics& diags, const char* args[])
{
    pid_t pid;
    int   status;
    int   res = posix_spawn(&pid, args[0], nullptr, nullptr, (char**)args, environ);
    if (res != 0)
        diags.error("Failed to spawn %s: %s (%d)", args[0], strerror(res), res);

    do {
        res = waitpid(pid, &status, 0);
    } while (res == -1 && errno == EINTR);
    if (res != -1) {
        if (WIFEXITED(status)) {
            res = WEXITSTATUS(status);
        } else {
            res = -1;
        }
    }

    return res;
}

void processRoots(Diagnostics& diags, std::set<std::string>& roots, const char *tempRootsDir)
{
    std::set<std::string> processedRoots;
    struct stat           sb;
    int                   res = 0;
    const char*           args[8];

    for (const auto& root : roots) {
        res = stat(root.c_str(), &sb);

        if (res == 0 && S_ISDIR(sb.st_mode)) {
            processedRoots.insert(root);
            continue;
        }

        char tempRootDir[MAXPATHLEN];
        strlcpy(tempRootDir, tempRootsDir, MAXPATHLEN);
        strlcat(tempRootDir, "/XXXXXXXX", MAXPATHLEN);
        mkdtemp(tempRootDir);

        if (endsWith(root, ".cpio") || endsWith(root, ".cpio.gz") || endsWith(root, ".cpgz") || endsWith(root, ".cpio.bz2") || endsWith(root, ".cpbz2") || endsWith(root, ".pax") || endsWith(root, ".pax.gz") || endsWith(root, ".pgz") || endsWith(root, ".pax.bz2") || endsWith(root, ".pbz2")) {
            args[0] = (char*)"/usr/bin/ditto";
            args[1] = (char*)"-x";
            args[2] = (char*)root.c_str();
            args[3] = tempRootDir;
            args[4] = nullptr;
        } else if (endsWith(root, ".tar")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".tar.gz") || endsWith(root, ".tgz")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xzf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".tar.bz2")
            || endsWith(root, ".tbz2")
            || endsWith(root, ".tbz")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xjf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".xar")) {
            args[0] = (char*)"/usr/bin/xar";
            args[1] = (char*)"-xf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".zip")) {
            args[0] = (char*)"/usr/bin/ditto";
            args[1] = (char*)"-xk";
            args[2] = (char*)root.c_str();
            args[3] = tempRootDir;
            args[4] = nullptr;
        } else {
            diags.error("unknown archive type: %s", root.c_str());
            continue;
        }

        if (res != runCommandAndWait(diags, args)) {
            fprintf(stderr, "Could not expand archive %s: %s (%d)", root.c_str(), strerror(res), res);
            exit(-1);
        }
        for (auto& existingRoot : processedRoots) {
            if (existingRoot == tempRootDir)
                return;
        }

        processedRoots.insert(tempRootDir);
    }

    roots = processedRoots;
}

bool writeRootList(const std::string& dstRoot, const std::set<std::string>& roots)
{
    mkpath_np(dstRoot.c_str(), 0755);
    if (roots.size() == 0)
        return false;

    std::string rootFile = dstRoot + "/roots.txt";
    FILE*       froots = ::fopen(rootFile.c_str(), "w");
    if (froots == NULL)
        return false;

    for (auto& root : roots) {
        fprintf(froots, "%s\n", root.c_str());
    }

    ::fclose(froots);
    return true;
}

BOMCopierCopyOperation filteredCopyExcludingPaths(BOMCopier copier, const char* path, BOMFSObjType type, off_t size)
{
    std::string absolutePath = &path[1];
    void *userData = BOMCopierUserData(copier);
    std::set<std::string> *cachePaths = (std::set<std::string>*)userData;
    if (cachePaths->count(absolutePath)) {
        return BOMCopierSkipFile;
    }
    return BOMCopierContinue;
}

BOMCopierCopyOperation filteredCopyIncludingPaths(BOMCopier copier, const char* path, BOMFSObjType type, off_t size)
{
    std::string absolutePath = &path[1];
    void *userData = BOMCopierUserData(copier);
    std::set<std::string> *cachePaths = (std::set<std::string>*)userData;
    for (const std::string& cachePath : *cachePaths) {
        if (startsWith(cachePath, absolutePath))
            return BOMCopierContinue;
    }
    if (cachePaths->count(absolutePath)) {
        return BOMCopierContinue;
    }
    return BOMCopierSkipFile;
}

static std::string dispositionToString(Disposition disposition) {
    switch (disposition) {
        case Unknown:
            return "Unknown";
        case InternalDevelopment:
            return "InternalDevelopment";
        case Customer:
            return "Customer";
        case InternalMinDevelopment:
            return "InternalMinDevelopment";
    }
}

static std::string platformToString(Platform platform) {
    switch (platform) {
        case unknown:
            return "unknown";
        case macOS:
            return "macOS";
        case iOS:
            return "iOS";
        case tvOS:
            return "tvOS";
        case watchOS:
            return "watchOS";
        case bridgeOS:
            return "bridgeOS";
        case iOSMac:
            return "iOSMac";
        case iOS_simulator:
            return "iOS_simulator";
        case tvOS_simulator:
            return "tvOS_simulator";
        case watchOS_simulator:
            return "watchOS_simulator";
    }
}

static dyld3::json::Node getBuildOptionsNode(BuildOptions_v1 buildOptions) {
    dyld3::json::Node buildOptionsNode;
    buildOptionsNode.map["version"].value             = dyld3::json::decimal(buildOptions.version);
    buildOptionsNode.map["updateName"].value          = buildOptions.updateName;
    buildOptionsNode.map["deviceName"].value          = buildOptions.deviceName;
    buildOptionsNode.map["disposition"].value         = dispositionToString(buildOptions.disposition);
    buildOptionsNode.map["platform"].value            = platformToString(buildOptions.platform);
    for (unsigned i = 0; i != buildOptions.numArchs; ++i) {
        dyld3::json::Node archNode;
        archNode.value = buildOptions.archs[i];
        buildOptionsNode.map["archs"].array.push_back(archNode);
    }
    buildOptionsNode.map["verboseDiagnostics"].value  = buildOptions.verboseDiagnostics ? "true" : "false";
    return buildOptionsNode;
}

int main(int argc, const char* argv[])
{
    @autoreleasepool {
        __block Diagnostics   diags;
        std::set<std::string> roots;
        std::string           dylibCacheDir;
        std::string           artifactDir;
        std::string           release;
        bool                  emitDevCaches = true;
        bool                  emitElidedDylibs = true;
        bool                  listConfigs = false;
        bool                  copyRoots = false;
        bool                  debug = false;
        bool                  useMRM = false;
        std::string           dstRoot;
        std::string           emitJSONPath;
        std::string           configuration;
        std::string           resultPath;
        std::string           baselineDifferenceResultPath;
        bool                  baselineCopyRoots = false;
        char* tempRootsDir = strdup("/tmp/dyld_shared_cache_builder.XXXXXX");

        mkdtemp(tempRootsDir);

        for (int i = 1; i < argc; ++i) {
            const char* arg = argv[i];
            if (arg[0] == '-') {
                if (strcmp(arg, "-debug") == 0) {
                    diags = Diagnostics(true);
                    debug = true;
                } else if (strcmp(arg, "-list_configs") == 0) {
                    listConfigs = true;
                } else if (strcmp(arg, "-root") == 0) {
                    roots.insert(realPath(argv[++i]));
                } else if (strcmp(arg, "-copy_roots") == 0) {
                    copyRoots = true;
                } else if (strcmp(arg, "-dylib_cache") == 0) {
                    dylibCacheDir = realPath(argv[++i]);
                } else if (strcmp(arg, "-artifact") == 0) {
                    artifactDir = realPath(argv[++i]);
                } else if (strcmp(arg, "-no_development_cache") == 0) {
                    emitDevCaches = false;
                } else if (strcmp(arg, "-no_overflow_dylibs") == 0) {
                    emitElidedDylibs = false;
                } else if (strcmp(arg, "-development_cache") == 0) {
                    emitDevCaches = true;
                } else if (strcmp(arg, "-overflow_dylibs") == 0) {
                    emitElidedDylibs = true;
                } else if (strcmp(arg, "-mrm") == 0) {
                    useMRM = true;
                } else if (strcmp(arg, "-emit_json") == 0) {
                    emitJSONPath = realPath(argv[++i]);
                } else if (strcmp(arg, "-dst_root") == 0) {
                    dstRoot = realPath(argv[++i]);
                } else if (strcmp(arg, "-release") == 0) {
                    release = argv[++i];
                } else if (strcmp(arg, "-results") == 0) {
                    resultPath = realPath(argv[++i]);
                } else if (strcmp(arg, "-baseline_diff_results") == 0) {
                    baselineDifferenceResultPath = realPath(argv[++i]);
                } else if (strcmp(arg, "-baseline_copy_roots") == 0) {
                    baselineCopyRoots = true;
                } else {
                    //usage();
                    fprintf(stderr, "unknown option: %s\n", arg);
                    exit(-1);
                }
            } else {
                if (!configuration.empty()) {
                    fprintf(stderr, "You may only specify one configuration\n");
                    exit(-1);
                }
                configuration = argv[i];
            }
        }

        time_t mytime = time(0);
        fprintf(stderr, "Started: %s", asctime(localtime(&mytime)));
        writeRootList(dstRoot, roots);
        processRoots(diags, roots, tempRootsDir);

        struct rlimit rl = { OPEN_MAX, OPEN_MAX };
        (void)setrlimit(RLIMIT_NOFILE, &rl);

        if (dylibCacheDir.empty() && artifactDir.empty() && release.empty()) {
            fprintf(stderr, "you must specify either -dylib_cache, -artifact or -release\n");
            exit(-1);
        } else if (!dylibCacheDir.empty() && !release.empty()) {
            fprintf(stderr, "you may not use -dylib_cache and -release at the same time\n");
            exit(-1);
        } else if (!dylibCacheDir.empty() && !artifactDir.empty()) {
            fprintf(stderr, "you may not use -dylib_cache and -artifact at the same time\n");
            exit(-1);
        }

        if ((configuration.empty() || dstRoot.empty()) && !listConfigs) {
            fprintf(stderr, "Must specify a configuration and a valid -dst_root OR -list_configs\n");
            exit(-1);
        }

        if (!baselineDifferenceResultPath.empty() && (roots.size() > 1)) {
            fprintf(stderr, "Cannot use -baseline_diff_results with more that one -root\n");
            exit(-1);
        }

        if (!artifactDir.empty()) {
            // Find the dylib cache dir from inside the artifact dir
            struct stat stat_buf;
            if (stat(artifactDir.c_str(), &stat_buf) != 0) {
                fprintf(stderr, "Could not find artifact path '%s'\n", artifactDir.c_str());
                exit(-1);
            }
            std::string dir = artifactDir + "/AppleInternal/Developer/DylibCaches";
            if (stat(dir.c_str(), &stat_buf) != 0) {
                fprintf(stderr, "Could not find artifact path '%s'\n", dir.c_str());
                exit(-1);
            }

            if (!release.empty()) {
                // Use the given release
                dylibCacheDir = dir + "/" + release + ".dlc";
            } else {
                // Find a release directory
                __block std::vector<std::string> subDirectories;
                iterateDirectoryTree("", dir, ^(const std::string& dirPath) {
                    subDirectories.push_back(dirPath);
                    return false;
                }, nullptr, false, false);

                if (subDirectories.empty()) {
                    fprintf(stderr, "Could not find dlc subdirectories inside '%s'\n", dir.c_str());
                    exit(-1);
                }

                if (subDirectories.size() > 1) {
                    fprintf(stderr, "Found too many subdirectories inside artifact path '%s'.  Use -release to select one\n", dir.c_str());
                    exit(-1);
                }

                dylibCacheDir = subDirectories.front();
            }
        }

        if (dylibCacheDir.empty()) {
            dylibCacheDir = std::string("/AppleInternal/Developer/DylibCaches/") + release + ".dlc";
        }

        //Move into the dir so we can use relative path manifests
        chdir(dylibCacheDir.c_str());

        dispatch_async(dispatch_get_main_queue(), ^{
            // If we only want a list of configuations, then tell the manifest to only parse the data and not
            // actually get all the macho's.
            bool onlyParseManifest = listConfigs && configuration.empty();
            auto manifest = dyld3::Manifest(diags, dylibCacheDir + "/Manifest.plist", roots, onlyParseManifest);

            if (manifest.build().empty()) {
                fprintf(stderr, "No manifest found at '%s/Manifest.plist'\n", dylibCacheDir.c_str());
                exit(-1);
            }
            fprintf(stderr, "Building Caches for %s\n", manifest.build().c_str());

            if (listConfigs) {
                manifest.forEachConfiguration([](const std::string& configName) {
                    printf("%s\n", configName.c_str());
                });
                // If we weren't passed a configuration then exit
                if (configuration.empty())
                    exit(0);
            }

            if (!manifest.filterForConfig(configuration)) {
                fprintf(stderr, "No config %s. Please run with -list_configs to see configurations available for this %s.\n",
                    configuration.c_str(), manifest.build().c_str());
                exit(-1);
            }

            (void)mkpath_np((dstRoot + "/System/Library/Caches/com.apple.dyld/").c_str(), 0755);
            bool cacheBuildSuccess = false;
            if (useMRM) {

                FILE* jsonFile = nullptr;
                if (!emitJSONPath.empty()) {
                    jsonFile = fopen(emitJSONPath.c_str(), "w");
                    if (!jsonFile) {
                        diags.verbose("can't open file '%s', errno=%d\n", emitJSONPath.c_str(), errno);
                        return;
                    }
                }
                dyld3::json::Node buildInvocationNode;

                // Find the archs for the configuration we want.
                __block std::set<std::string> validArchs;
                manifest.configuration(configuration).forEachArchitecture(^(const std::string& path) {
                    validArchs.insert(path);
                });

                if (validArchs.size() != 1) {
                    fprintf(stderr, "MRM doesn't support more than one arch per configuration: %s\n",
                            configuration.c_str());
                    exit(-1);
                }

                const char* archs[validArchs.size()];
                uint64_t archIndex = 0;
                for (const std::string& arch : validArchs) {
                    archs[archIndex++] = arch.c_str();
                }

                BuildOptions_v1 buildOptions;
                buildOptions.version                            = 1;
                buildOptions.updateName                         = manifest.build().c_str();
                buildOptions.deviceName                         = configuration.c_str();
                buildOptions.disposition                        = Disposition::Unknown;
                buildOptions.platform                           = (Platform)manifest.platform();
                buildOptions.archs                              = archs;
                buildOptions.numArchs                           = validArchs.size();
                buildOptions.verboseDiagnostics                 = debug;
                buildOptions.isLocallyBuiltCache                = true;

                __block struct SharedCacheBuilder* sharedCacheBuilder = createSharedCacheBuilder(&buildOptions);
                buildInvocationNode.map["build-options"] = getBuildOptionsNode(buildOptions);

                std::set<std::string> requiredBinaries =  {
                    "/usr/lib/libSystem.B.dylib"
                };

                // Get the file data for every MachO in the BOM.
                __block dyld3::json::Node filesNode;
                __block std::vector<std::pair<const void*, size_t>> mappedFiles;
                manifest.forEachMachO(configuration, ^(const std::string &buildPath, const std::string &runtimePath, const std::string &arch, bool shouldBeExcludedIfLeaf) {

                    // Filter based on arch as the Manifest adds the file once for each UUID.
                    if (!validArchs.count(arch))
                        return;

                    struct stat stat_buf;
                    int fd = ::open(buildPath.c_str(), O_RDONLY, 0);
                    if (fd == -1) {
                        diags.verbose("can't open file '%s', errno=%d\n", buildPath.c_str(), errno);
                        return;
                    }

                    if (fstat(fd, &stat_buf) == -1) {
                        diags.verbose("can't stat open file '%s', errno=%d\n", buildPath.c_str(), errno);
                        ::close(fd);
                        return;
                    }

                    const void* buffer = mmap(NULL, (size_t)stat_buf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (buffer == MAP_FAILED) {
                        diags.verbose("mmap() for file at %s failed, errno=%d\n", buildPath.c_str(), errno);
                        ::close(fd);
                    }
                    ::close(fd);

                    mappedFiles.emplace_back(buffer, (size_t)stat_buf.st_size);
                    FileFlags fileFlags = FileFlags::NoFlags;
                    if (requiredBinaries.count(runtimePath))
                        fileFlags = FileFlags::RequiredClosure;
                    addFile(sharedCacheBuilder, runtimePath.c_str(), (uint8_t*)buffer, (size_t)stat_buf.st_size, fileFlags);

                    dyld3::json::Node fileNode;
                    fileNode.map["path"].value  = runtimePath;
                    fileNode.map["flags"].value = "NoFlags";
                    filesNode.array.push_back(fileNode);
                });

                __block dyld3::json::Node symlinksNode;
                manifest.forEachSymlink(configuration, ^(const std::string &fromPath, const std::string &toPath) {
                    addSymlink(sharedCacheBuilder, fromPath.c_str(), toPath.c_str());

                    dyld3::json::Node symlinkNode;
                    symlinkNode.map["from-path"].value  = fromPath;
                    symlinkNode.map["to-path"].value    = toPath;
                    symlinksNode.array.push_back(symlinkNode);
                });

                buildInvocationNode.map["symlinks"] = symlinksNode;

                std::string orderFileData;
                if (!manifest.dylibOrderFile().empty()) {
                    orderFileData = loadOrderFile(manifest.dylibOrderFile());
                    if (!orderFileData.empty()) {
                        addFile(sharedCacheBuilder, "*order file data*", (uint8_t*)orderFileData.data(), orderFileData.size(), FileFlags::DylibOrderFile);
                        dyld3::json::Node fileNode;
                        fileNode.map["path"].value  = manifest.dylibOrderFile();
                        fileNode.map["flags"].value = "DylibOrderFile";
                        filesNode.array.push_back(fileNode);
                    }
                }

                std::string dirtyDataOrderFileData;
                if (!manifest.dirtyDataOrderFile().empty()) {
                    dirtyDataOrderFileData = loadOrderFile(manifest.dirtyDataOrderFile());
                    if (!dirtyDataOrderFileData.empty()) {
                        addFile(sharedCacheBuilder, "*dirty data order file data*", (uint8_t*)dirtyDataOrderFileData.data(), dirtyDataOrderFileData.size(), FileFlags::DirtyDataOrderFile);
                        dyld3::json::Node fileNode;
                        fileNode.map["path"].value  = manifest.dirtyDataOrderFile();
                        fileNode.map["flags"].value = "DirtyDataOrderFile";
                        filesNode.array.push_back(fileNode);
                    }
                }

                buildInvocationNode.map["files"] = filesNode;

                if (jsonFile) {
                    dyld3::json::printJSON(buildInvocationNode, 0, jsonFile);
                    fclose(jsonFile);
                    jsonFile = nullptr;
                }

                cacheBuildSuccess = runSharedCacheBuilder(sharedCacheBuilder);

                if (!cacheBuildSuccess) {
                    for (uint64 i = 0, e = getErrorCount(sharedCacheBuilder); i != e; ++i) {
                        const char* errorMessage = getError(sharedCacheBuilder, i);
                        fprintf(stderr, "ERROR: %s\n", errorMessage);
                    }
                }

                // Now emit each cache we generated, or the errors for them.
                for (uint64 i = 0, e = getCacheResultCount(sharedCacheBuilder); i != e; ++i) {
                    BuildResult result;
                    getCacheResult(sharedCacheBuilder, i, &result);
                    if (result.numErrors) {
                        for (uint64_t errorIndex = 0; errorIndex != result.numErrors; ++errorIndex) {
                            fprintf(stderr, "[%s] ERROR: %s\n", result.loggingPrefix, result.errors[errorIndex]);
                        }
                        cacheBuildSuccess = false;
                        continue;
                    }
                    if (result.numWarnings) {
                        for (uint64_t warningIndex = 0; warningIndex != result.numWarnings; ++warningIndex) {
                            fprintf(stderr, "[%s] WARNING: %s\n", result.loggingPrefix, result.warnings[warningIndex]);
                        }
                    }
                }

                // If we built caches, then write everything out.
                // TODO: Decide if we should we write any good caches anyway?
                if (cacheBuildSuccess) {
                    for (uint64 i = 0, e = getFileResultCount(sharedCacheBuilder); i != e; ++i) {
                        FileResult result;
                        getFileResult(sharedCacheBuilder, i, &result);

                        if (!result.data)
                            continue;

                        const std::string path = dstRoot + result.path;
                        std::string pathTemplate = path + "-XXXXXX";
                        size_t templateLen = strlen(pathTemplate.c_str())+2;
                        char pathTemplateSpace[templateLen];
                        strlcpy(pathTemplateSpace, pathTemplate.c_str(), templateLen);
                        int fd = mkstemp(pathTemplateSpace);
                        if ( fd != -1 ) {
                            ::ftruncate(fd, result.size);
                            uint64_t writtenSize = pwrite(fd, result.data, result.size, 0);
                            if ( writtenSize == result.size ) {
                                ::fchmod(fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH); // mkstemp() makes file "rw-------", switch it to "rw-r--r--"
                                if ( ::rename(pathTemplateSpace, path.c_str()) == 0) {
                                    ::close(fd);
                                    continue; // success
                                }
                            }
                            else {
                                fprintf(stderr, "ERROR: could not write file %s\n", pathTemplateSpace);
                                cacheBuildSuccess = false;
                            }
                            ::close(fd);
                            ::unlink(pathTemplateSpace);
                        }
                        else {
                            fprintf(stderr, "ERROR: could not open file %s\n", pathTemplateSpace);
                            cacheBuildSuccess = false;
                        }
                    }
                }

                destroySharedCacheBuilder(sharedCacheBuilder);

                for (auto mappedFile : mappedFiles)
                    ::munmap((void*)mappedFile.first, mappedFile.second);
            } else {
                manifest.calculateClosure();

                cacheBuildSuccess = build(diags, manifest, dstRoot, false, debug, false, false, emitDevCaches, true);
            }

            if (!cacheBuildSuccess) {
                exit(-1);
            }

            // Compare this cache to the baseline cache and see if we have any roots to copy over
            if (!baselineDifferenceResultPath.empty() || baselineCopyRoots) {
                std::set<std::string> baselineDylibs = manifest.resultsForConfiguration(configuration);

                std::set<std::string> newDylibs;
                manifest.forEachConfiguration([&manifest, &newDylibs](const std::string& configName) {
                    for (auto& arch : manifest.configuration(configName).architectures) {
                        for (auto& dylib : arch.second.results.dylibs) {
                            if (dylib.second.included) {
                                newDylibs.insert(manifest.installNameForUUID(dylib.first));
                            }
                        }
                    }
                });

                if (baselineCopyRoots) {
                    // Work out the set of dylibs in the old cache but not the new one
                    std::set<std::string> dylibsMissingFromNewCache;
                    for (const std::string& baselineDylib : baselineDylibs) {
                        if (!newDylibs.count(baselineDylib))
                            dylibsMissingFromNewCache.insert(baselineDylib);
                    }

                    if (!dylibsMissingFromNewCache.empty()) {
                        BOMCopier copier = BOMCopierNewWithSys(BomSys_default());
                        BOMCopierSetUserData(copier, (void*)&dylibsMissingFromNewCache);
                        BOMCopierSetCopyFileStartedHandler(copier, filteredCopyIncludingPaths);
                        std::string dylibCacheRootDir = realFilePath(dylibCacheDir + "/Root");
                        if (dylibCacheRootDir == "") {
                            fprintf(stderr, "Could not find dylib Root directory to copy baseline roots from\n");
                            exit(1);
                        }
                        BOMCopierCopy(copier, dylibCacheRootDir.c_str(), dstRoot.c_str());
                        BOMCopierFree(copier);

                        for (const std::string& dylibMissingFromNewCache : dylibsMissingFromNewCache) {
                            diags.verbose("Dylib missing from new cache: '%s'\n", dylibMissingFromNewCache.c_str());
                        }
                    }
                }

                if (!baselineDifferenceResultPath.empty()) {
                    auto cppToObjStr = [](const std::string& str) {
                        return [NSString stringWithUTF8String:str.c_str()];
                    };

                    // Work out the set of dylibs in the cache and taken from the -root
                    NSMutableArray<NSString*>* dylibsFromRoots = [NSMutableArray array];
                    for (auto& root : roots) {
                        for (const std::string& dylibInstallName : newDylibs) {
                            struct stat sb;
                            std::string filePath = root + "/" + dylibInstallName;
                            if (!stat(filePath.c_str(), &sb)) {
                                [dylibsFromRoots addObject:cppToObjStr(dylibInstallName)];
                            }
                        }
                    }

                    // Work out the set of dylibs in the new cache but not in the baseline cache.
                    NSMutableArray<NSString*>* dylibsMissingFromBaselineCache = [NSMutableArray array];
                    for (const std::string& newDylib : newDylibs) {
                        if (!baselineDylibs.count(newDylib))
                            [dylibsMissingFromBaselineCache addObject:cppToObjStr(newDylib)];
                    }

                    NSMutableDictionary* cacheDict = [[NSMutableDictionary alloc] init];
                    cacheDict[@"root-paths-in-cache"] = dylibsFromRoots;
                    cacheDict[@"device-paths-to-delete"] = dylibsMissingFromBaselineCache;

                    NSError* error = nil;
                    NSData*  outData = [NSPropertyListSerialization dataWithPropertyList:cacheDict
                                                                                  format:NSPropertyListBinaryFormat_v1_0
                                                                                 options:0
                                                                                   error:&error];
                    (void)[outData writeToFile:cppToObjStr(baselineDifferenceResultPath) atomically:YES];
                }
            }

            if (copyRoots) {
                std::set<std::string> cachePaths;
                manifest.forEachConfiguration([&manifest, &cachePaths](const std::string& configName) {
                    for (auto& arch : manifest.configuration(configName).architectures) {
                        for (auto& dylib : arch.second.results.dylibs) {
                            if (dylib.second.included) {
                                cachePaths.insert(manifest.installNameForUUID(dylib.first));
                            }
                        }
                    }
                });

                BOMCopier copier = BOMCopierNewWithSys(BomSys_default());
                BOMCopierSetUserData(copier, (void*)&cachePaths);
                BOMCopierSetCopyFileStartedHandler(copier, filteredCopyExcludingPaths);
                for (auto& root : roots) {
                    BOMCopierCopy(copier, root.c_str(), dstRoot.c_str());
                }
                BOMCopierFree(copier);
            }

            int err = sync_volume_np(dstRoot.c_str(), SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_WAIT);
            if (err) {
                fprintf(stderr, "Volume sync failed errnor=%d (%s)\n", err, strerror(err));
            }

            // Now that all the build commands have been issued lets put a barrier in after then which can tear down the app after
            // everything is written.

            if (!resultPath.empty()) {
                manifest.write(resultPath);
            }

            const char* args[8];
            args[0] = (char*)"/bin/rm";
            args[1] = (char*)"-rf";
            args[2] = (char*)tempRootsDir;
            args[3] = nullptr;
            (void)runCommandAndWait(diags, args);

            for (const std::string& warn : diags.warnings()) {
                fprintf(stderr, "dyld_shared_cache_builder: warning: %s\n", warn.c_str());
            }
            exit(0);
        });
    }

    dispatch_main();

    return 0;
}
