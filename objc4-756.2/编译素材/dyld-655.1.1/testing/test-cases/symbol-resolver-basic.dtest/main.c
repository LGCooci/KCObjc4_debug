
// BUILD:  $CC foo.c foo2.c -dynamiclib  -install_name $RUN_DIR/libfoo.dylib -o $BUILD_DIR/libfoo.dylib
// BUILD:  $CC main.c $BUILD_DIR/libfoo.dylib -o $BUILD_DIR/symbol-resolver.exe

// RUN:  ./symbol-resolver.exe
// RUN:  TEN=1 ./symbol-resolver.exe


#include <stdio.h>
#include <stdlib.h>

extern int foo();
extern int fooPlusOne();


int main()
{
	if ( getenv("TEN") != NULL ) {
		if ( foo() != 10 )
			printf("[FAIL] symbol-resolver-basic: foo() != 10\n");
		else if ( fooPlusOne() != 11 )
			printf("[FAIL] symbol-resolver-basic: fooPlusOne() != 11\n");
		else
			printf("[PASS] symbol-resolver-basic\n");
	}
	else {
		if ( foo() != 0 )
			printf("[FAIL] symbol-resolver-basic: foo() != 0\n");
		else if ( fooPlusOne() != 1 )
			printf("[FAIL] symbol-resolver-basic: fooPlusOne() != 1\n");
		else
            printf("[PASS] symbol-resolver-basic\n");
	}
  
	return 0;
}
