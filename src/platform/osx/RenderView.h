#pragma once

#if __has_feature(modules)
@import MetalKit;
#else
#import <MetalKit/MetalKit.h>
#endif

@interface RenderView : MTKView

@end
