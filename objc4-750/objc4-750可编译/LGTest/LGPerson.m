//
//  LGPerson.m
//  01-Runtime 初探
//
//  Created by cooci on 2018/11/27.
//  Copyright © 2018 cooci. All rights reserved.
//

#import "LGPerson.h"
#include <objc/runtime.h>

@implementation LGPerson

- (void)readBook{
    NSLog(@"%s",__func__);
}
+ (void)helloWord{
    NSLog(@"%s",__func__);
}

#pragma mark - 动态方法解析

+ (BOOL)resolveInstanceMethod:(SEL)sel{
 
    return [super resolveInstanceMethod:sel];
}





@end
