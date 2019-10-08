##
# Copyright (c) 2010 Apple Inc. All rights reserved.
#
# @APPLE_LICENSE_HEADER_START@
# 
# This file contains Original Code and/or Modifications of Original Code
# as defined in and that are subject to the Apple Public Source License
# Version 2.0 (the 'License'). You may not use this file except in
# compliance with the License. Please obtain a copy of the License at
# http://www.opensource.apple.com/apsl/ and read it before using this
# file.
# 
# The Original Code and all software distributed under the License are
# distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
# EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
# INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
# Please see the License for the specific language governing rights and
# limitations under the License.
# 
# @APPLE_LICENSE_HEADER_END@
##
TESTROOT = ../..
include ${TESTROOT}/include/common.makefile

PWD = $(shell pwd)

ifeq "$(OS_NAME)" "iPhoneOS"
	CHECK = check-ios
else
	CHECK = check-macosx
endif

all-check: all check

check: $(CHECK)

check-ios:
	./main 10

check-macosx:
	./main 10
	export DYLD_VERSIONED_LIBRARY_PATH="${PWD}/alt11" && ./main 11
	export DYLD_VERSIONED_LIBRARY_PATH="${PWD}/alt9" && ./main 10
	export DYLD_VERSIONED_LIBRARY_PATH="${PWD}/alt9:${PWD}/alt11" && ./main 11
	export DYLD_VERSIONED_LIBRARY_PATH="${PWD}/alt11:${PWD}/alt12" && ./main 12

all: 
	mkdir -p alt11 alt9 alt12
	${CC} ${CCFLAGS} -dynamiclib foo.c -DRESULT=10 -current_version 10 -o "${PWD}/libfoo.dylib"
	${CC} ${CCFLAGS} -dynamiclib bar.c -DRESULT=10 -current_version 10 -o "${PWD}/libbar.dylib"
	${CC} ${CCFLAGS} -I${TESTROOT}/include -o main main.c libbar.dylib libfoo.dylib
	${CC} ${CCFLAGS} -dynamiclib foo.c -DRESULT=11 -current_version 11 -install_name "${PWD}/libfoo.dylib" -o alt11/libfoo.dylib  
	${CC} ${CCFLAGS} -dynamiclib bar.c -DRESULT=11 -current_version 11 -install_name "${PWD}/libbar.dylib" -o alt11/libbar.dylib
	${CC} ${CCFLAGS} -dynamiclib foo.c -DRESULT=9  -current_version 9  -install_name "${PWD}/libfoo.dylib" -o alt9/libfoo.dylib
	${CC} ${CCFLAGS} -dynamiclib foo.c -DRESULT=12 -current_version 12 -install_name "${PWD}/libfoo.dylib" -o alt12/libfoo.dylib  


clean:
	${RM} -rf main libfoo.dylib libbar.dylib alt9 alt11 alt12

