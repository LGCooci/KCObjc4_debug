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

#include "ClosureFileSystemPhysical.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sandbox.h>
#include <sandbox/private.h>
#include <unistd.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

using dyld3::closure::FileSystemPhysical;

bool FileSystemPhysical::getRealPath(const char possiblePath[MAXPATHLEN], char realPath[MAXPATHLEN]) const {
    bool success = false;
    int fd = ::open(possiblePath, O_RDONLY);
    if ( fd != -1 ) {
        success = fcntl(fd, F_GETPATH, realPath) == 0;
        ::close(fd);
    }
    if (success)
        return success;
    realpath(possiblePath, realPath);
    int realpathErrno = errno;
    // If realpath() resolves to a path which does not exist on disk, errno is set to ENOENT
    return (realpathErrno == ENOENT) || (realpathErrno == 0);
}

static bool sandboxBlocked(const char* path, const char* kind)
{
#if TARGET_IPHONE_SIMULATOR
    // sandbox calls not yet supported in dyld_sim
    return false;
#else
    sandbox_filter_type filter = (sandbox_filter_type)(SANDBOX_FILTER_PATH | SANDBOX_CHECK_NO_REPORT);
    return ( sandbox_check(getpid(), kind, filter, path) > 0 );
#endif
}

static bool sandboxBlockedMmap(const char* path)
{
    return sandboxBlocked(path, "file-map-executable");
}

static bool sandboxBlockedOpen(const char* path)
{
    return sandboxBlocked(path, "file-read-data");
}

static bool sandboxBlockedStat(const char* path)
{
    return sandboxBlocked(path, "file-read-metadata");
}

// Returns true on success.  If an error occurs the given callback will be called with the reason.
// On success, info is filled with info about the loaded file.  If the path supplied includes a symlink,
// the supplier realerPath is filled in with the real path of the file, otherwise it is set to the empty string.
bool FileSystemPhysical::loadFile(const char* path, LoadedFileInfo& info, char realerPath[MAXPATHLEN], void (^error)(const char* format, ...)) const {
    // open file
    const char* originalPath    = path;
    char altPath[PATH_MAX];
    int fd = -1;
    if  ( _fileSystemPrefix != nullptr ) {
        strlcpy(altPath, _fileSystemPrefix, PATH_MAX);
        strlcat(altPath, path, PATH_MAX);
        fd = ::open(altPath, O_RDONLY, 0);
        if ( fd != -1 )
            path = altPath;
    }
    if ( fd == -1 ) {
        fd = ::open(path, O_RDONLY, 0);
        if ( fd == -1 ) {
            int openErrno = errno;
            if ( (openErrno == EPERM) && sandboxBlockedOpen(path) )
                error("file system sandbox blocked open(\"%s\", O_RDONLY)", path);
            else if ( (openErrno != ENOENT) && (openErrno != ENOTDIR) )
                error("open(\"%s\", O_RDONLY) failed with errno=%d", path, openErrno);
            return false;
        }
    }

    // Get the realpath of the file if it is a symlink
    if ( fcntl(fd, F_GETPATH, realerPath) == 0 ) {
        // Don't set the realpath if it is just the same as the regular path
        if ( strcmp(originalPath, realerPath) == 0 )
            realerPath[0] = '\0';
    } else {
        error("Could not get real path for \"%s\"\n", path);
        ::close(fd);
        return false;
    }

    // get file info
    struct stat statBuf;
#if TARGET_IPHONE_SIMULATOR
    if ( ::stat(path, &statBuf) != 0 ) {
#else
    if ( ::fstat(fd, &statBuf) != 0 ) {
#endif
        int statErr = errno;
        if ( (statErr == EPERM) && sandboxBlockedStat(path) )
            error("file system sandbox blocked stat(\"%s\")", path);
        else
            error("stat(\"%s\") failed with errno=%d", path, errno);
        ::close(fd);
        return false;
    }

    // only regular files can be loaded
    if ( !S_ISREG(statBuf.st_mode) ) {
        error("not a file for %s", path);
        ::close(fd);
        return false;
    }

    // mach-o files must be at list one page in size
    if ( statBuf.st_size < 4096  ) {
        error("file too short %s", path);
        ::close(fd);
        return false;
    }

    info.fileContent = nullptr;
    info.fileContentLen = statBuf.st_size;
    info.sliceOffset = 0;
    info.sliceLen = statBuf.st_size;
    info.inode = statBuf.st_ino;
    info.mtime = statBuf.st_mtime;
    info.path  = originalPath;

    // mmap() whole file
    void* wholeFile = ::mmap(nullptr, (size_t)statBuf.st_size, PROT_READ, MAP_PRIVATE|MAP_RESILIENT_CODESIGN, fd, 0);
    if ( wholeFile == MAP_FAILED ) {
        int mmapErr = errno;
        if ( mmapErr == EPERM ) {
            if ( sandboxBlockedMmap(path) )
                error("file system sandbox blocked mmap() of '%s'", path);
            else
                error("code signing blocked mmap() of '%s'", path);
        }
        else {
            error("mmap() failed with errno=%d for %s", errno, path);
        }
        ::close(fd);
        return false;
    }
    info.fileContent = wholeFile;

    // Set unmap as the unload method.
    info.unload = [](const LoadedFileInfo& info) {
        ::munmap((void*)info.fileContent, (size_t)info.fileContentLen);
    };

    ::close(fd);
    return true;
}

void FileSystemPhysical::unloadFile(const LoadedFileInfo& info) const {
    if (info.unload)
        info.unload(info);
}

void FileSystemPhysical::unloadPartialFile(LoadedFileInfo& info, uint64_t keepStartOffset, uint64_t keepLength) const {
    // Unmap from 0..keepStartOffset and (keepStartOffset+keepLength)..info.fileContentLen
    if (keepStartOffset)
        ::munmap((void*)info.fileContent, (size_t)keepStartOffset);
    if ((keepStartOffset + keepLength) != info.fileContentLen) {
        // Round up to page alignment
        keepLength = (keepLength + PAGE_SIZE - 1) & (-PAGE_SIZE);
        ::munmap((void*)((char*)info.fileContent + keepStartOffset + keepLength), (size_t)(info.fileContentLen - (keepStartOffset + keepLength)));
    }
    info.fileContent = (const void*)((char*)info.fileContent + keepStartOffset);
    info.fileContentLen = keepLength;
}

bool FileSystemPhysical::fileExists(const char* path, uint64_t* inode, uint64_t* mtime, bool* issetuid) const {
    struct stat statBuf;
    if ( _fileSystemPrefix != nullptr ) {
        char altPath[PATH_MAX];
        strlcpy(altPath, _fileSystemPrefix, PATH_MAX);
        strlcat(altPath, path, PATH_MAX);
        if ( ::stat(altPath, &statBuf) == 0 ) {
            if (inode)
                *inode = statBuf.st_ino;
            if (mtime)
                *mtime = statBuf.st_mtime;
            if (issetuid)
                *issetuid = (statBuf.st_mode & (S_ISUID|S_ISGID));
            return true;
        }
    }
    if ( ::stat(path, &statBuf) != 0 )
        return false;
    if (inode)
        *inode = statBuf.st_ino;
    if (mtime)
        *mtime = statBuf.st_mtime;
    if (issetuid)
        *issetuid = (statBuf.st_mode & (S_ISUID|S_ISGID));
    return true;
}
