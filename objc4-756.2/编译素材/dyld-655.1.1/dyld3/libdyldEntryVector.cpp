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

#include <stdarg.h>

#include "dyld_priv.h"
#include "libdyldEntryVector.h"
#include "AllImages.h"
#include "Array.h"
#include "Loading.h"
#include "Logging.h"
#include "PathOverrides.h"
#include "StartGlue.h"
#include "dyld_process_info_internal.h"

extern "C" char start;

VIS_HIDDEN const char** appleParams;

extern bool gUseDyld3;

namespace dyld3 {


AllImages::ProgramVars sVars;
static void (*sChildForkFunction)();

static const char* leafName(const char* argv0)
{
    if ( argv0 == nullptr )
       return "";

    if ( const char* lastSlash = strrchr(argv0, '/') )
        return lastSlash+1;
    else
        return argv0;
}

static void entry_setVars(const mach_header* mainMH, int argc, const char* argv[], const char* envp[], const char* apple[])
{
    NXArgc       = argc;
    NXArgv       = argv;
    environ      = (char**)envp;
    appleParams  = apple;
    __progname   = leafName(argv[0]);

    sVars.mh            = mainMH;
    sVars.NXArgcPtr     = &NXArgc;
    sVars.NXArgvPtr     = &NXArgv;
    sVars.environPtr    = (const char***)&environ;
    sVars.__prognamePtr = &__progname;
    gAllImages.setProgramVars(&sVars);

    gUseDyld3 = true;

    setLoggingFromEnvs(envp);
}

static void entry_setHaltFunction(void (*func)(const char* message) __attribute__((noreturn)) )
{
    setHaltFunction(func);
}

static void entry_setLogFunction(void (*logFunction)(const char* format, va_list list))
{
    setLoggingFunction(logFunction);
}

static void entry_setOldAllImageInfo(dyld_all_image_infos* old)
{
    gAllImages.setOldAllImageInfo(old);
}

static void entry_setNotifyMonitoringDyldMain(void (*notifyMonitoringDyldMain)()) {
    setNotifyMonitoringDyldMain(notifyMonitoringDyldMain);
}

static void entry_setNotifyMonitoringDyld(void (*notifyMonitoringDyld)(bool unloading,unsigned imageCount,
                                                                               const struct mach_header* loadAddresses[],
                                                                               const char* imagePaths[])) {
    setNotifyMonitoringDyld(notifyMonitoringDyld);
}

static void entry_setInitialImageList(const closure::LaunchClosure* closure,
                                const DyldSharedCache* dyldCacheLoadAddress, const char* dyldCachePath,
                                const Array<LoadedImage>& initialImages, const LoadedImage& libSystem)
{
    gAllImages.init(closure, dyldCacheLoadAddress, dyldCachePath, initialImages);
    gAllImages.applyInterposingToDyldCache(closure);

    const char* mainPath = _simple_getenv(appleParams, "executable_path");
    if ( (mainPath != nullptr) && (mainPath[0] == '/') )
        gAllImages.setMainPath(mainPath);

    // run initializer for libSytem.B.dylib
    // this calls back into _dyld_initializer which calls gAllIimages.addImages()
    gAllImages.runLibSystemInitializer(libSystem);

    // now that malloc is available, parse DYLD_ env vars
    closure::gPathOverrides.setEnvVars((const char**)environ, gAllImages.mainExecutable(), gAllImages.mainExecutableImage()->path());
}

static void entry_runInitialzersBottomUp(const mach_header* mainExecutableImageLoadAddress)
{
    gAllImages.runStartupInitialzers();
    gAllImages.notifyMonitorMain();
}

static void entry_setChildForkFunction(void (*func)() )
{
    sChildForkFunction = func;
}

static void entry_setRestrictions(bool allowAtPaths, bool allowEnvPaths)
{
    gAllImages.setRestrictions(allowAtPaths, allowEnvPaths);
}

const LibDyldEntryVector entryVectorForDyld = {
    LibDyldEntryVector::kCurrentVectorVersion,
    closure::kFormatVersion,
    &entry_setVars,
    &entry_setHaltFunction,
    &entry_setOldAllImageInfo,
    &entry_setInitialImageList,
    &entry_runInitialzersBottomUp,
    (__typeof(LibDyldEntryVector::startFunc))address_of_start,
    &entry_setChildForkFunction,
    &entry_setLogFunction,
    &entry_setRestrictions,
    &entry_setNotifyMonitoringDyldMain,
    &entry_setNotifyMonitoringDyld
};

VIS_HIDDEN void _dyld_fork_child()
{
    (*sChildForkFunction)();
}


} // namespace dyld3

