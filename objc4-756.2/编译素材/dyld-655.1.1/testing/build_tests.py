#!/usr/bin/python2.7

import plistlib
import string
import argparse
import sys
import os
import tempfile
import shutil
import subprocess


#
# Scan files in .dtest directory looking for BUILD: or RUN: directives.
# Return a dictionary of all directives.
#
def parseDirectives(testCaseSourceDir):
    onlyLines = []
    buildLines = []
    runLines = []
    minOS = ""
    timeout = ""
    noCrashLogs = []
    for file in os.listdir(testCaseSourceDir):
        if file.endswith((".c", ".cpp", ".cxx")):
            with open(testCaseSourceDir + "/" + file) as f:
                for line in f.read().splitlines():
                    buildIndex = string.find(line, "BUILD:")
                    if buildIndex != -1:
                        buildLines.append(line[buildIndex + 6:].lstrip())
                    runIndex = string.find(line, "RUN:")
                    if runIndex != -1:
                        runLines.append(line[runIndex+4:].lstrip())
                    onlyIndex = string.find(line, "BUILD_ONLY:")
                    if onlyIndex != -1:
                        onlyLines.append(line[onlyIndex+11:].lstrip())
                    minOsIndex = string.find(line, "BUILD_MIN_OS:")
                    if minOsIndex != -1:
                        minOS = line[minOsIndex+13:].lstrip()
                    timeoutIndex = string.find(line, "RUN_TIMEOUT:")
                    if timeoutIndex != -1:
                        timeout = line[timeoutIndex+12:].lstrip()
                    noCrashLogsIndex = string.find(line, "NO_CRASH_LOG:")
                    if noCrashLogsIndex != -1:
                        noCrashLogs.append(line[noCrashLogsIndex+13:].lstrip())
    return {
        "BUILD":        buildLines,
        "BUILD_ONLY":   onlyLines,
        "BUILD_MIN_OS": minOS,
        "RUN":          runLines,
        "RUN_TIMEOUT":  timeout,
        "NO_CRASH_LOG": noCrashLogs
   }


#
# Look at directives dictionary to see if this test should be skipped for this platform
#
def useTestCase(testCaseDirectives, platformName):
    onlyLines = testCaseDirectives["BUILD_ONLY"]
    for only in onlyLines:
        if only == "MacOSX" and platformName != "macosx":
            return False
        if only == "iOS" and platformName != "iphoneos":
            return False
    return True


#
# Use BUILD directives to construct the test case
# Use RUN directives to generate a shell script to run test(s)
#
def buildTestCase(testCaseDirectives, testCaseSourceDir, toolsDir, sdkDir, dyldIncludesDir, minOsOptionsName, defaultMinOS, archOptions, testCaseDestDirBuild, testCaseDestDirRun):
    scratchDir = tempfile.mkdtemp()
    if testCaseDirectives["BUILD_MIN_OS"]:
        minOS = testCaseDirectives["BUILD_MIN_OS"]
    else:
        minOS = defaultMinOS
    compilerSearchOptions = " -isysroot " + sdkDir + " -I" + sdkDir + "/System/Library/Frameworks/System.framework/PrivateHeaders" + " -I" + dyldIncludesDir
    if minOsOptionsName == "mmacosx-version-min":
        taskForPidCommand = "touch "
        envEnableCommand  = "touch "
    else:
        taskForPidCommand = "codesign --force --sign - --entitlements " + testCaseSourceDir + "/../../task_for_pid_entitlement.plist "
        envEnableCommand  = "codesign --force --sign - --entitlements " + testCaseSourceDir + "/../../get_task_allow_entitlement.plist "
    buildSubs = {
        "CC":                   toolsDir + "/usr/bin/clang "   + archOptions + " -" + minOsOptionsName + "=" + str(minOS) + compilerSearchOptions,
        "CXX":                  toolsDir + "/usr/bin/clang++ " + archOptions + " -" + minOsOptionsName + "=" + str(minOS) + compilerSearchOptions,
        "BUILD_DIR":            testCaseDestDirBuild,
        "RUN_DIR":              testCaseDestDirRun,
        "TEMP_DIR":             scratchDir,
        "TASK_FOR_PID_ENABLE":  taskForPidCommand,
        "DYLD_ENV_VARS_ENABLE": envEnableCommand
    }
    os.makedirs(testCaseDestDirBuild)
    os.chdir(testCaseSourceDir)
    print >> sys.stderr, "cd " + testCaseSourceDir
    for line in testCaseDirectives["BUILD"]:
        cmd = string.Template(line).safe_substitute(buildSubs)
        print >> sys.stderr, cmd
        if "&&" in cmd:
            result = subprocess.call(cmd, shell=True)
        else:
            cmdList = []
            cmdList = string.split(cmd)
            result = subprocess.call(cmdList)
        if result:
            return result
    shutil.rmtree(scratchDir, ignore_errors=True)
    sudoSub = ""
    if minOsOptionsName == "mmacosx-version-min":
        sudoSub = "sudo"
    runSubs = {
        "RUN_DIR":        testCaseDestDirRun,
        "REQUIRE_CRASH":  "nocr -require_crash",
        "SUDO":           sudoSub,
    }
    runFilePath = testCaseDestDirBuild + "/run.sh"
    with open(runFilePath, "a") as runFile:
        runFile.write("#!/bin/sh\n")
        runFile.write("cd " + testCaseDestDirRun + "\n")
        os.chmod(runFilePath, 0755)
        runFile.write("echo \"run in dyld2 mode\" \n");
        for runline in testCaseDirectives["RUN"]:
           runFile.write(string.Template(runline).safe_substitute(runSubs) + "\n")
        runFile.write("echo \"run in dyld3 mode\" \n");
        for runline in testCaseDirectives["RUN"]:
            subLine = string.Template(runline).safe_substitute(runSubs)
            if subLine.startswith("sudo "):
                subLine = "sudo DYLD_USE_CLOSURES=1 " + subLine[5:]
            else:
                subLine = "DYLD_USE_CLOSURES=1 " + subLine
            runFile.write(subLine + "\n")
        runFile.write("\n")
        runFile.close()
    return 0



#
# Use XCode build settings to build all unit tests for specified platform
# Generate a .plist for BATS to use to run all tests
#
if __name__ == "__main__":
    dstDir = os.getenv("DSTROOT", "/tmp/dyld_tests/")
    testsRunDstTopDir = "/AppleInternal/CoreOS/tests/dyld/"
    testsBuildDstTopDir = dstDir + testsRunDstTopDir
    shutil.rmtree(testsBuildDstTopDir, ignore_errors=True)
    dyldSrcDir = os.getenv("SRCROOT", "")
    if not dyldSrcDir:
        dyldSrcDir = os.getcwd()
    testsSrcTopDir = dyldSrcDir + "/testing/test-cases/"
    dyldIncludesDir = dyldSrcDir + "/include/"
    sdkDir = os.getenv("SDKROOT", "")
    if not sdkDir:
        sdkDir = subprocess.check_output(["xcrun", "-sdk", "macosx.internal", "--show-sdk-path"]).rstrip()
    toolsDir = os.getenv("TOOLCHAIN_DIR", "/")
    defaultMinOS = ""
    minVersNum = "10.14"
    minOSOption = os.getenv("DEPLOYMENT_TARGET_CLANG_FLAG_NAME", "")
    if minOSOption:
        minOSVersName = os.getenv("DEPLOYMENT_TARGET_CLANG_ENV_NAME", "")
        if minOSVersName:
            minVersNum = os.getenv(minOSVersName, "")
    else:
        minOSOption = "mmacosx-version-min"
    platformName = os.getenv("PLATFORM_NAME", "osx")
    archOptions = ""
    archList = os.getenv("RC_ARCHS", "")
    if archList:
        for arch in string.split(archList, " "):
            archOptions = archOptions + " -arch " + arch
    else:
        archList = os.getenv("ARCHS_STANDARD_32_64_BIT", "")
        if platformName == "watchos":
            archOptions = "-arch armv7k"
        elif platformName == "appletvos":
            archOptions = "-arch arm64"
        else:
            if archList:
                for arch in string.split(archList, " "):
                    archOptions = archOptions + " -arch " + arch
            else:
                archOptions = "-arch x86_64"
    allTests = []
    suppressCrashLogs = []
    for f in sorted(os.listdir(testsSrcTopDir)):
        if f.endswith(".dtest"):
            testName = f[0:-6]
            outDirBuild = testsBuildDstTopDir + testName
            outDirRun = testsRunDstTopDir + testName
            testCaseDir = testsSrcTopDir + f
            testCaseDirectives = parseDirectives(testCaseDir)
            if useTestCase(testCaseDirectives, platformName):
                result = buildTestCase(testCaseDirectives, testCaseDir, toolsDir, sdkDir, dyldIncludesDir, minOSOption, minVersNum, archOptions, outDirBuild, outDirRun)
                if result:
                    sys.exit(result)
                mytest = {}
                mytest["TestName"] = testName
                mytest["Arch"] = "platform-native"
                mytest["WorkingDirectory"] = testsRunDstTopDir + testName
                mytest["Command"] = []
                mytest["Command"].append("./run.sh")
                usesDtrace = False
                for runline in testCaseDirectives["RUN"]:
                    if "$SUDO" in runline:
                        mytest["AsRoot"] = True
                    if "dtrace" in runline:
                        usesDtrace = True
                if testCaseDirectives["RUN_TIMEOUT"]:
                    mytest["Timeout"] = testCaseDirectives["RUN_TIMEOUT"]
                if usesDtrace and minOSOption != "mmacosx-version-min":
                    mytest["BootArgsSet"] = "dtrace_dof_mode=1"
                allTests.append(mytest)
            if testCaseDirectives["NO_CRASH_LOG"]:
                for skipCrash in testCaseDirectives["NO_CRASH_LOG"]:
                    suppressCrashLogs.append(skipCrash)
    batsInfo = { "BATSConfigVersion": "0.1.0",
                 "Project":           "dyld_tests",
                 "Tests":             allTests }
    if suppressCrashLogs:
        batsInfo["IgnoreCrashes"] = []
        for skipCrash in suppressCrashLogs:
            batsInfo["IgnoreCrashes"].append(skipCrash)
    batsFileDir = dstDir + "/AppleInternal/CoreOS/BATS/unit_tests/"
    shutil.rmtree(batsFileDir, ignore_errors=True)
    os.makedirs(batsFileDir)
    batsFilePath = batsFileDir + "dyld.plist"
    with open(batsFilePath, "w") as batsFile:
        batsFile.write(plistlib.writePlistToString(batsInfo))
        batsFile.close()
    os.system('plutil -convert binary1 ' + batsFilePath) # convert the plist in place to binary
    runHelper = dstDir + "/AppleInternal/CoreOS/tests/dyld/run_all_dyld_tests.sh"
    print runHelper
    with open(runHelper, "w") as shFile:
        shFile.write("#!/bin/sh\n")
        for test in allTests:
            shFile.write(test["WorkingDirectory"] + "/run.sh\n")
        shFile.close()
    os.chmod(runHelper, 0755)

