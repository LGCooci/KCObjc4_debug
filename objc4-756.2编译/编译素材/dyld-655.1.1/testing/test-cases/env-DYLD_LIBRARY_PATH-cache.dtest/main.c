
// BUILD:  mkdir -p $BUILD_DIR/override
// BUILD:  mkdir -p $BUILD_DIR/re-export-override
// BUILD:  $CC myzlib.c -dynamiclib -o $BUILD_DIR/override/libz.1.dylib -install_name /usr/lib/libz.1.dylib -compatibility_version 1.0 -framework CoreFoundation
// BUILD:  $CC reexported-myzlib.c -dynamiclib -o $BUILD_DIR/re-export-override/reexported.dylib -compatibility_version 1.0 -framework CoreFoundation
// BUILD:  $CC reexporter.c -dynamiclib -o $BUILD_DIR/re-export-override/libz.1.dylib -install_name /usr/lib/libz.1.dylib -compatibility_version 1.0 -Wl,-reexport_library,$BUILD_DIR/re-export-override/reexported.dylib
// BUILD:  $CC main.c  -o $BUILD_DIR/main.exe -lz
// BUILD:  $DYLD_ENV_VARS_ENABLE $BUILD_DIR/main.exe

// RUN:  ./main.exe
// RUN:  DYLD_LIBRARY_PATH=$RUN_DIR/override/ ./main.exe
// RUN:  DYLD_LIBRARY_PATH=$RUN_DIR/re-export-override/ ./main.exe

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <stdbool.h>

// The test here is to override libz.1.dylib which is in the dyld cache with our own implementation.

int main()
{
    bool expectMyDylib = (getenv("DYLD_LIBRARY_PATH") != NULL);

    printf("[BEGIN] env-DYLD_LIBRARY_PATH-cache, %s\n", expectMyDylib ? "my" : "os");

    bool usingMyDylib = (strcmp(zlibVersion(), "my") == 0);

	if ( usingMyDylib == expectMyDylib )
        printf("[PASS] env-DYLD_LIBRARY_PATH-cache, %s\n", expectMyDylib ? "my" : "os");
    else
        printf("[FAIL] env-DYLD_LIBRARY_PATH-cache, %s\n", expectMyDylib ? "my" : "os");

	return 0;
}

