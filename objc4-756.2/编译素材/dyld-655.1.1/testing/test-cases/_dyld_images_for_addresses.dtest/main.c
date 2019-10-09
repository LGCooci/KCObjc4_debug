
// BUILD:  $CC foo.c -dynamiclib  -install_name $RUN_DIR/libfoo.dylib -o $BUILD_DIR/libfoo.dylib
// BUILD:  $CC main.c $BUILD_DIR/libfoo.dylib -o $BUILD_DIR/dyld_images_for_addresses.exe

// RUN:  ./dyld_images_for_addresses.exe

#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <uuid/uuid.h>
#include <mach-o/dyld_priv.h>

extern void* __dso_handle;

extern int foo1();
extern int foo2();
extern int foo3();


int myfunc()
{
    return 3;
}

int myfunc2()
{
    return 3;
}

static int mystaticfoo()
{
    return 3;
}

int mydata = 5;
int myarray[10];


int main()
{
    printf("[BEGIN] _dyld_images_for_addresses\n");
    int mylocal;

    const void* addresses[10];
    addresses[0] = &myfunc;
    addresses[1] = &myfunc2;
    addresses[2] = &mystaticfoo;
    addresses[3] = &__dso_handle;
    addresses[4] = &mydata;
    addresses[5] = &myarray;
    addresses[6] = &mylocal;    // not owned by dyld, so coresponding dyld_image_uuid_offset should be all zeros
    addresses[7] = &foo1;
    addresses[8] = &foo2;
    addresses[9] = &foo3;

    struct dyld_image_uuid_offset infos[10];
    _dyld_images_for_addresses(10, addresses, infos);

    for (int i=0; i < 10; ++i) {
        uuid_string_t str;
        uuid_unparse_upper(infos[i].uuid, str);
        printf("0x%09llX 0x%08llX %s\n", (long long)infos[i].image, infos[i].offsetInImage, str);
    }

    if ( infos[0].image != infos[1].image )
        printf("[FAIL] _dyld_images_for_addresses 1\n");
    else if ( infos[0].image != infos[2].image )
        printf("[FAIL] _dyld_images_for_addresses 2\n");
    else if ( infos[0].image != infos[3].image )
        printf("[FAIL] _dyld_images_for_addresses 3\n");
    else if ( infos[0].image != infos[4].image )
        printf("[FAIL] _dyld_images_for_addresses 4\n");
    else if ( infos[0].image != infos[5].image )
        printf("[FAIL] _dyld_images_for_addresses 5\n");
    else if ( infos[6].image != NULL )
        printf("[FAIL] _dyld_images_for_addresses 6\n");
    else if ( infos[7].image != infos[8].image )
        printf("[FAIL] _dyld_images_for_addresses 7\n");
    else if ( infos[7].image != infos[9].image )
        printf("[FAIL] _dyld_images_for_addresses 8\n");
    else if ( infos[0].image == infos[7].image )
        printf("[FAIL] _dyld_images_for_addresses 9\n");
    else if ( uuid_compare(infos[0].uuid, infos[1].uuid) != 0  )
        printf("[FAIL] _dyld_images_for_addresses 10\n");
    else if ( uuid_compare(infos[0].uuid, infos[2].uuid) != 0 )
        printf("[FAIL] _dyld_images_for_addresses 11\n");
    else if ( uuid_compare(infos[0].uuid, infos[3].uuid) != 0 )
        printf("[FAIL] _dyld_images_for_addresses 12\n");
    else if ( uuid_compare(infos[0].uuid, infos[4].uuid) != 0 )
        printf("[FAIL] _dyld_images_for_addresses 13\n");
    else if ( uuid_compare(infos[0].uuid, infos[5].uuid) != 0 )
        printf("[FAIL] _dyld_images_for_addresses 14\n");
    else if ( uuid_is_null(infos[6].uuid) == 0 )
        printf("[FAIL] _dyld_images_for_addresses 15\n");
    else if ( uuid_compare(infos[7].uuid, infos[8].uuid) != 0 )
        printf("[FAIL] _dyld_images_for_addresses 16\n");
    else if ( uuid_compare(infos[7].uuid, infos[9].uuid) != 0 )
        printf("[FAIL] _dyld_images_for_addresses 17\n");
    else if ( uuid_compare(infos[0].uuid, infos[7].uuid) == 0 )
        printf("[FAIL] _dyld_images_for_addresses 18\n");
    else
        printf("[PASS] _dyld_images_for_addresses\n");
    return 0;
}

