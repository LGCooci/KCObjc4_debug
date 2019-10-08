//
//  LGPerson.h
//  01-Runtime 初探
//
//  Created by cooci on 2018/11/27.
//  Copyright © 2018 cooci. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "LGStudent.h"

NS_ASSUME_NONNULL_BEGIN

@interface LGPerson : NSObject
@property (nonatomic, strong) LGStudent *student;
@property (nonatomic, copy) NSString *nameStr;

- (void)run;
+ (void)walk;

- (void)readBook;

@end

NS_ASSUME_NONNULL_END
