#import "RenderView.h"

#if __has_feature(modules)
@import QuartzCore.CAMetalLayer;
@import CoreVideo.CVDisplayLink;
@import AppKit.NSView;
#else
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CVDisplayLink.h>
#import <AppKit/NSView.h>
#endif

#include <CoreFoundation/CFBundle.h>

#include <imgui.h>
#include <examples/imgui_impl_osx.h>

#include "volcano.h"

@interface RenderView()
{
    CVDisplayLinkRef myDisplayLink;
}
- (CVReturn)getFrameForTime:(const CVTimeStamp*)outputTime;
@end

static CVReturn displayLinkCallback(CVDisplayLinkRef displayLink, const CVTimeStamp* now, const CVTimeStamp* outputTime, CVOptionFlags flagsIn, CVOptionFlags* flagsOut, void* displayLinkContext)
{
    return [(__bridge RenderView*)displayLinkContext getFrameForTime:outputTime];
}

@implementation RenderView

- (id)initWithFrame:(NSRect)frameRect
{
    self = [super initWithFrame:frameRect];
    //self.bounds = frameRect;
    
    self.device = MTLCreateSystemDefaultDevice();
    self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    self.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    self.sampleCount = 1;//self.renderer.multisamples;
    self.delegate = (id <MTKViewDelegate>)self;
    
    // Set paused and only trigger redraw when needs display is set.
    self.paused = YES;
    self.enableSetNeedsDisplay = YES;
    
    self.wantsLayer = YES;
    
    const float backingScaleFactor = [NSScreen mainScreen].backingScaleFactor;
    CGSize size = CGSizeMake(self.bounds.size.width * backingScaleFactor, self.bounds.size.height * backingScaleFactor);
    
    if (![self.layer isKindOfClass:[CAMetalLayer class]])
        [self setLayer:[CAMetalLayer layer]];
    
    CAMetalLayer* renderLayer = [CAMetalLayer layer];
    renderLayer.device = self.device;
    renderLayer.pixelFormat = self.colorPixelFormat;
    renderLayer.framebufferOnly = YES;
    renderLayer.frame = self.bounds;
    renderLayer.drawableSize = size;
        
    CFURLRef resourceURL = CFURLCopyAbsoluteURL(CFBundleCopyResourcesDirectoryURL(CFBundleGetMainBundle()));
    char resourcePath[1024];
    CFStringGetFileSystemRepresentation(CFURLCopyFileSystemPath(resourceURL, kCFURLPOSIXPathStyle), resourcePath, sizeof(resourcePath) - 1);
    strcat(resourcePath, "/resources/");
    
    vkapp_create((__bridge void*)(self), (int)self.bounds.size.width, (int)self.bounds.size.width, (int)size.width, (int)size.height, resourcePath, false);
    
    // Add a tracking area in order to receive mouse events whenever the mouse is within the bounds of our view
    NSTrackingArea* trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseMoved | NSTrackingInVisibleRect | NSTrackingActiveAlways owner:self userInfo:nil];
    [self addTrackingArea:trackingArea];
    
    // If we want to receive key events, we either need to be in the responder chain of the key view,
    // or else we can install a local monitor. The consequence of this heavy-handed approach is that
    // we receive events for all controls, not just ImGui widgets. If we had native controls in our
    // window, we'd want to be much more careful than just ingesting the complete event stream, though
    // we do make an effort to be good citizens by passing along events when ImGui doesn't want to capture.
    NSEventMask eventMask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged | NSEventTypeScrollWheel;
    [NSEvent addLocalMonitorForEventsMatchingMask:eventMask handler:^NSEvent * _Nullable(NSEvent *event) {
        BOOL wantsCapture = ImGui_ImplOSX_HandleEvent(event, self);
        if (event.type == NSEventTypeKeyDown && wantsCapture) {
            return nil;
        } else {
            return event;
        }
        
    }];
    
    ImGui_ImplOSX_Init();
    
    // Setup display link.
    CVDisplayLinkCreateWithActiveCGDisplays(&myDisplayLink);
    CVReturn error = CVDisplayLinkSetOutputCallback(myDisplayLink, &displayLinkCallback, (__bridge void*)self);
    NSAssert((kCVReturnSuccess == error), @"Setting Display Link callback error %d", error);
    error = CVDisplayLinkStart(myDisplayLink);
    NSAssert((kCVReturnSuccess == error), @"Creating Display Link error %d", error);
    
    return self;
}

- (void)mouseMoved:(NSEvent *)event {
    ImGui_ImplOSX_HandleEvent(event, self);
}

- (void)mouseDown:(NSEvent *)event {
    ImGui_ImplOSX_HandleEvent(event, self);
}

- (void)mouseUp:(NSEvent *)event {
    ImGui_ImplOSX_HandleEvent(event, self);
}

- (void)mouseDragged:(NSEvent *)event {
    ImGui_ImplOSX_HandleEvent(event, self);
}

- (void)scrollWheel:(NSEvent *)event {
    ImGui_ImplOSX_HandleEvent(event, self);
}

- (void)dealloc
{
    CVDisplayLinkStop(myDisplayLink);
    
    ImGui_ImplOSX_Shutdown();
    vkapp_destroy();
}

- (CVReturn)getFrameForTime:(const CVTimeStamp*)outputTime
{
    if (true) //if view needs redraw
    {
        // Need to dispatch to main thread as CVDisplayLink uses it's own thread.
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setNeedsDisplay:YES];
        });
    }
    return kCVReturnSuccess;
}

- (void)drawInMTKView:(nonnull MTKView*)view
{
    ImGui_ImplOSX_NewFrame(view);
    vkapp_draw();
}

@end
