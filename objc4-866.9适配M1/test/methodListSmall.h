#include "test.h"

struct ObjCClass {
    struct ObjCClass * __ptrauth_objc_isa_pointer isa;
    struct ObjCClass * __ptrauth_objc_super_pointer superclass;
    void *cachePtr;
    uintptr_t zero;
    struct ObjCClass_ro * __ptrauth_objc_class_ro data;
};

struct ObjCClass_ro {
    uint32_t flags;
    uint32_t instanceStart;
    uint32_t instanceSize;
#ifdef __LP64__
    uint32_t reserved;
#endif

    const uint8_t * ivarLayout;
    
    const char * name;
    struct ObjCMethodList * __ptrauth_objc_method_list_pointer baseMethodList;
    struct protocol_list_t * baseProtocols;
    const struct ivar_list_t * ivars;

    const uint8_t * weakIvarLayout;
    struct property_list_t *baseProperties;
};

struct ObjCMethod {
    char *name;
    char *type;
    IMP imp;
};

struct ObjCMethodList {
    uint32_t sizeAndFlags;
    uint32_t count;
    struct ObjCMethod methods[];
};

struct ObjCMethodSmall {
    int32_t nameOffset;
    int32_t typeOffset;
    int32_t impOffset;
};

struct ObjCMethodListSmall {
    uint32_t sizeAndFlags;
    uint32_t count;
    struct ObjCMethodSmall methods[];
};


extern struct ObjCClass OBJC_METACLASS_$_NSObject;
extern struct ObjCClass OBJC_CLASS_$_NSObject;


struct ObjCClass_ro FooMetaclass_ro = {
    .flags = 1,
    .instanceStart = 40,
    .instanceSize = 40,
    .name = "Foo",
};

struct ObjCClass FooMetaclass = {
    .isa = &OBJC_METACLASS_$_NSObject,
    .superclass = &OBJC_METACLASS_$_NSObject,
    .cachePtr = &_objc_empty_cache,
    .data = &FooMetaclass_ro,
};


int ranMyMethod1;
extern "C" void myMethod1(id self __unused, SEL _cmd) {
    testprintf("myMethod1\n");
    testassert(_cmd == @selector(myMethod1));
    ranMyMethod1 = 1;
}

int ranMyMethod2;
extern "C" void myMethod2(id self __unused, SEL _cmd) {
    testprintf("myMethod2\n");
    testassert(_cmd == @selector(myMethod2));
    ranMyMethod2 = 1;
}

int ranMyMethod3;
extern "C" void myMethod3(id self __unused, SEL _cmd) {
    testprintf("myMethod3\n");
    testassert(_cmd == @selector(myMethod3));
    ranMyMethod3 = 1;
}

int ranMyReplacedMethod1;
extern "C" void myReplacedMethod1(id self __unused, SEL _cmd) {
    testprintf("myReplacedMethod1\n");
    testassert(_cmd == @selector(myMethod1));
    ranMyReplacedMethod1 = 1;
}

int ranMyReplacedMethod2;
extern "C" void myReplacedMethod2(id self __unused, SEL _cmd) {
    testprintf("myReplacedMethod2\n");
    testassert(_cmd == @selector(myMethod2));
    ranMyReplacedMethod2 = 1;
}

struct BigStruct {
  uintptr_t a, b, c, d, e, f, g;
};

int ranMyMethodStret;
extern "C" BigStruct myMethodStret(id self __unused, SEL _cmd) {
    testprintf("myMethodStret\n");
    testassert(_cmd == @selector(myMethodStret));
    ranMyMethodStret = 1;
    BigStruct ret = {};
    return ret;
}

int ranMyReplacedMethodStret;
extern "C" BigStruct myReplacedMethodStret(id self __unused, SEL _cmd) {
    testprintf("myReplacedMethodStret\n");
    testassert(_cmd == @selector(myMethodStret));
    ranMyReplacedMethodStret = 1;
    BigStruct ret = {};
    return ret;
}

extern struct ObjCMethodList Foo_methodlistSmall;

asm("\
.section __TEXT,__cstring\n\
_MyMethod1Name:\n\
    .asciz \"myMethod1\"\n\
_MyMethod2Name:\n\
    .asciz \"myMethod2\"\n\
_MyMethod3Name:\n\
    .asciz \"myMethod3\"\n\
_BoringMethodType:\n\
    .asciz \"v16@0:8\"\n\
_MyMethodStretName:\n\
    .asciz \"myMethodStret\"\n\
_MyMethodNullTypesName:\n\
    .asciz \"myMethodNullTypes\"\n\
_StretType:\n\
    .asciz \"{BigStruct=QQQQQQQ}16@0:8\"\n\
");

#if __LP64__
asm("\
.section __DATA,__objc_selrefs,literal_pointers,no_dead_strip\n\
_MyMethod1NameRef:\n\
    .quad _MyMethod1Name\n\
_MyMethod2NameRef:\n\
    .quad _MyMethod2Name\n\
_MyMethod3NameRef:\n\
    .quad _MyMethod3Name\n\
_MyMethodStretNameRef:\n\
    .quad _MyMethodStretName\n\
_MyMethodNullTypesNameRef:\n\
    .quad _MyMethodNullTypesName\n\
");
#else
asm("\
.section __DATA,__objc_selrefs,literal_pointers,no_dead_strip\n\
_MyMethod1NameRef:\n\
    .long _MyMethod1Name\n\
_MyMethod2NameRef:\n\
    .long _MyMethod2Name\n\
_MyMethod3NameRef:\n\
    .long _MyMethod3Name\n\
_MyMethodStretNameRef:\n\
    .long _MyMethodStretName\n\
_MyMethodNullTypesNameRef:\n\
    .long _MyMethodNullTypesName\n\
");
#endif

#if MUTABLE_METHOD_LIST
asm(".section __DATA,__objc_methlist\n");
#else
asm(".section __TEXT,__objc_methlist\n");
#endif

asm("\
    .p2align 2\n\
_Foo_methodlistSmall:\n\
    .long 12 | 0x80000000\n\
    .long 5\n\
    \n\
    .long _MyMethod1NameRef - .\n\
    .long _BoringMethodType - .\n\
    .long _myMethod1 - .\n\
    \n\
    .long _MyMethod2NameRef - .\n\
    .long _BoringMethodType - .\n\
    .long _myMethod2 - .\n\
    \n\
    .long _MyMethod3NameRef - .\n\
    .long _BoringMethodType - .\n\
    .long _myMethod3 - .\n\
    \n\
    .long _MyMethodStretNameRef - .\n\
    .long _StretType - .\n\
    .long _myMethodStret - .\n\
\n\
    .long _MyMethodNullTypesNameRef - .\n\
    .long 0\n\
    .long _myMethod1 - .\n\
");

struct ObjCClass_ro Foo_ro = {
    .instanceStart = 8,
    .instanceSize = 8,
    .name = "Foo",
    .baseMethodList = &Foo_methodlistSmall,
};

struct ObjCClass FooClass = {
    .isa = &FooMetaclass,
    .superclass = &OBJC_CLASS_$_NSObject,
    .cachePtr = &_objc_empty_cache,
    .data = &Foo_ro,
};


@interface Foo: NSObject

- (void)myMethod1;
- (void)myMethod2;
- (void)myMethod3;
- (BigStruct)myMethodStret;

@end
