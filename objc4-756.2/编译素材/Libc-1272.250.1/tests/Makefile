PROJECT := Libc
TEST_DIR := tests/

ifeq ($(DEVELOPER_DIR),)
    DEVELOPER_DIR := $(shell xcode-select -p)
endif

include $(DEVELOPER_DIR)/AppleInternal/Makefiles/darwintest/Makefile.common

ifeq ($(PLATFORM),BridgeOS)
EXCLUDED_SOURCES += locale.c
endif

WARNING_CFLAGS := -Weverything \
	-Wno-vla -Wno-missing-field-initializers -Wno-padded \
	-Wno-gnu-flexible-array-initializer -Wno-gnu-empty-initializer \
	-Wno-partial-availability -Wno-used-but-marked-unused \
	-Wno-reserved-id-macro -fmacro-backtrace-limit=0 \
	-Wno-c++98-compat
OTHER_CFLAGS := -DDARWINTEST --std=gnu11 $(WARNING_CFLAGS)
OTHER_LDFLAGS := -ldarwintest_utils
ASAN_DYLIB_PATH := /usr/local/lib/sanitizers/

nxheap: OTHER_CFLAGS += -Wno-cast-align
strlcat: OTHER_CFLAGS += -Wno-pointer-arith
psort: OTHER_CFLAGS += -Wno-cast-qual -Wno-sign-conversion
net: OTHER_CFLAGS += -Wno-sign-conversion -Wno-cast-align -Wno-incompatible-pointer-types-discards-qualifiers -Wno-sign-compare
printf: OTHER_CFLAGS += -Wno-format-nonliteral
strlcpy: OTHER_CFLAGS += -D_FORTIFY_SOURCE=0
realpath_edge: OTHER_CFLAGS += -fsanitize=address -I../fbsdcompat
realpath_edge: OTHER_LDFLAGS += -Wl,-rpath -Wl,$(ASAN_DYLIB_PATH)
qsort freebsd_qsort: OTHER_CFLAGS += -Wno-unused-function
ifeq ($(PLATFORM),MacOSX)
qsort_perf: OTHER_CFLAGS += -Wno-sign-compare -Wno-sign-conversion -Wno-cast-align -Wno-shorten-64-to-32
else
EXCLUDED_SOURCES += qsort_perf.c
endif
os_variant: OTHER_CFLAGS += -DOS_CRASH_ENABLE_EXPERIMENTAL_LIBTRACE

include $(DEVELOPER_DIR)/AppleInternal/Makefiles/darwintest/Makefile.targets

