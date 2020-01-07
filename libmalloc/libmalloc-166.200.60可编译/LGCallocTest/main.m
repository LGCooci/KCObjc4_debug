//
//  main.m
//  LGCallocTest
//
//  Created by cooci on 2019/2/7.
//

#import <Foundation/Foundation.h>
#import <malloc/malloc.h>
int main(int argc, const char * argv[]) {
    @autoreleasepool {
		
		void *p = calloc(1, 40);
		NSLog(@"%lu",malloc_size(p));
    }
    return 0;
}
