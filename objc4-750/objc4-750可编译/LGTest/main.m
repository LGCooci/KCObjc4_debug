//
//  main.m
//  LGTest
//
//  Created by cooci on 2019/2/7.
//

#import <Foundation/Foundation.h>
#import "LGPerson.h"

int main(int argc, const char * argv[]) {
    @autoreleasepool {

        NSObject *objc1 = [NSObject alloc];
        id __weak objc2 = objc1;
        id __weak objc3 = objc2;
        
        // 对象相等 = hash 

     }
    return 0;
}
