#import <Cocoa/Cocoa.h>

@interface AppDelegate : NSObject <NSApplicationDelegate>

@property (nonatomic, strong) NSDisplayLink1* timer;
@property (nonatomic, assign) NSTimeInterval lastTimestamp;


@end

