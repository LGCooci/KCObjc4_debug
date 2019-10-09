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


all-check: all $(CHECK)

check: $(CHECK)

check-ios:
	./main 10

check-macosx:
	./main 10
	export DYLD_VERSIONED_FRAMEWORK_PATH="${PWD}/alt11" && ./main 11 "alt11/Foo.framework/Versions/A/Foo"
	export DYLD_VERSIONED_FRAMEWORK_PATH="${PWD}/alt9" && ./main 10
	export DYLD_VERSIONED_FRAMEWORK_PATH="${PWD}/alt9:${PWD}/alt11" && ./main 11
	export DYLD_VERSIONED_FRAMEWORK_PATH="${PWD}/alt11:${PWD}/alt12" && ./main 12

all: 
	mkdir -p Foo.framework alt11/Foo.framework/Versions/A alt9/Foo.framework alt12/Foo.framework Bar.framework alt11/Bar.framework/Versions/A/
	${CC} ${CCFLAGS} -dynamiclib bar.c -DRESULT=10 -current_version 10 -install_name "${PWD}/Bar.framework/Bar" -o Bar.framework/Bar
	${CC} ${CCFLAGS} -dynamiclib foo.c -DRESULT=10 -current_version 10 -install_name "${PWD}/Foo.framework/Foo" -o Foo.framework/Foo
	${CC} ${CCFLAGS} -I${TESTROOT}/include -o main main.c Bar.framework/Bar Foo.framework/Foo
	${CC} ${CCFLAGS} -dynamiclib foo.c -DRESULT=11 -current_version 11 -install_name "${PWD}/Foo.framework/Foo" -o alt11/Foo.framework/Versions/A/Foo
	${CC} ${CCFLAGS} -dynamiclib bar.c -DRESULT=11 -current_version 11 -install_name "${PWD}/Bar.framework/Foo" -o alt11/Bar.framework/Versions/A/Bar
	cd alt11/Foo.framework && ln -sf Versions/A/Foo
	cd alt11/Bar.framework && ln -sf Versions/A/Bar
	${CC} ${CCFLAGS} -dynamiclib foo.c -DRESULT=9  -current_version 9  -install_name "${PWD}/Foo.framework/Foo" -o alt9/Foo.framework/Foo
	${CC} ${CCFLAGS} -dynamiclib foo.c -DRESULT=12 -current_version 12 -install_name "${PWD}/Foo.framework/Foo" -o alt12/Foo.framework/Foo  


clean:
	${RM} -rf main Foo.framework Bar.framework alt9 alt11 alt12

