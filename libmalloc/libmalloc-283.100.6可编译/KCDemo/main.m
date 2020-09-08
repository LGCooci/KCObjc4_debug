//
//  main.m
//  KCDemo
//
//  Created by cooci on 2020/9/5.
//

#import <Foundation/Foundation.h>
#import <malloc/malloc.h>

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        // insert code here...
		
		void *p = calloc(1, 40);
        NSLog(@"Hello, World!");
    }
    return 0;
}
