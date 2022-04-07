//
//  LGPerson.h
//  KCObjcBuild
//
//  Created by cooci on 2022/3/7.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface LGPerson : NSObject

+ (id)person;

+ (id)allocString;
+ (id)initString;
+ (id)copyString;

+ (id)otherString;

- (void)childThreadDemo;
@end

NS_ASSUME_NONNULL_END
