//
//  main.m
//  KCMalloc
//
//  Created by cooci on 2022/11/19.
//

#import <Foundation/Foundation.h>
#import <malloc/malloc.h>

/**
 Cooci - 和谐学习 不急不躁
 需求: 针对malloc源码修改
 目标: 编译调试 objc 下层流程: calloc
 缺陷:
	- 对zone的优化处理进行隔断
	- nano_v2的处理
	- 还有platform的环境变量简化
 */
int main(int argc, const char * argv[]) {
    @autoreleasepool {
		void *p = calloc(1, 24);
		NSLog(@"%lu",malloc_size(p));
		NSLog(@"Hello, KCMalloc");
    }
    return 0;
}
