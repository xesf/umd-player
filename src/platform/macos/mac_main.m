/* macOS: window, display, input, and the main loop. The window is 960x544, like the Vita screen. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "../platform.h"
#include "../../core/deint.h"
#include "mac_frame.h"

#define WINDOW_W 960
#define WINDOW_H 544

static uint32_t held_buttons;
static CALayer *video_layer, *overlay_layer;
static uint32_t overlay_id;
static NSView *content_view;

static uint32_t button_for_key(unsigned short code)
{
    switch (code) {
    case 126: return PLAT_BTN_UP;
    case 125: return PLAT_BTN_DOWN;
    case 123: return PLAT_BTN_LEFT;
    case 124: return PLAT_BTN_RIGHT;
    case 49:  return PLAT_BTN_CROSS;     /* space */
    case 51:  return PLAT_BTN_CIRCLE;    /* backspace */
    case 0:   return PLAT_BTN_SQUARE;    /* A */
    case 13:  return PLAT_BTN_TRIANGLE;  /* W */
    case 12:  return PLAT_BTN_L;         /* Q */
    case 14:  return PLAT_BTN_R;         /* E */
    case 36:  return PLAT_BTN_START;     /* return */
    case 48:  return PLAT_BTN_SELECT;    /* tab */
    default:  return 0;
    }
}

@interface UMDView : NSView
@end

@implementation UMDView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e
{
    if (e.keyCode == 53) {   /* escape */
        printf("quit: escape key\n");
        [NSApp terminate:nil];
        return;
    }
    held_buttons |= button_for_key(e.keyCode);
}
- (void)keyUp:(NSEvent *)e { held_buttons &= ~button_for_key(e.keyCode); }
@end

@interface UMDAppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *window;
@property (strong) NSTimer *timer;
@end

@implementation UMDAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSRect r = NSMakeRect(0, 0, WINDOW_W, WINDOW_H);
    self.window = [[NSWindow alloc] initWithContentRect:r
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"UMD Player";
    content_view = [[UMDView alloc] initWithFrame:r];
    content_view.wantsLayer = YES;
    content_view.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    video_layer = [CALayer layer];
    video_layer.contentsGravity = kCAGravityResize;
    video_layer.magnificationFilter = kCAFilterLinear;
    [content_view.layer addSublayer:video_layer];
    overlay_layer = [CALayer layer];
    overlay_layer.contentsGravity = kCAGravityResize;
    [content_view.layer addSublayer:overlay_layer];
    self.window.contentView = content_view;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:content_view];
    [NSApp activateIgnoringOtherApps:YES];

    self.timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *t) {
        (void)t;
        if (!app_frame())
            [NSApp terminate:nil];
    }];
    [[NSRunLoop mainRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    printf("quit: window closed\n");
    return YES;
}
- (void)applicationWillTerminate:(NSNotification *)n { (void)n; [self.timer invalidate]; app_shutdown(); }
@end

uint32_t plat_buttons(void)
{
    return held_buttons;
}

void plat_present(plat_frame *frame, const plat_present_info *info)
{
    CGRect bounds = content_view.bounds;
    double aspect, w, h;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (!frame) {
        video_layer.contents = nil;
    } else {
        if (info->deinterlace && !frame->deinterlaced) {
            CVPixelBufferLockBaseAddress(frame->image, 0);
            deint_blend32(CVPixelBufferGetBaseAddress(frame->image), (int)CVPixelBufferGetWidth(frame->image),
                          (int)CVPixelBufferGetHeight(frame->image), (int)CVPixelBufferGetBytesPerRow(frame->image));
            CVPixelBufferUnlockBaseAddress(frame->image, 0);
            frame->deinterlaced = 1;
        }
        video_layer.contents = (__bridge id)CVPixelBufferGetIOSurface(frame->image);
        aspect = (double)info->width / info->height;
        if (info->sar_w > 0 && info->sar_h > 0)
            aspect *= (double)info->sar_w / info->sar_h;
        w = bounds.size.width;
        h = w / aspect;
        if (h > bounds.size.height) {
            h = bounds.size.height;
            w = h * aspect;
        }
        video_layer.frame = CGRectMake((bounds.size.width - w) / 2, (bounds.size.height - h) / 2, w, h);
    }
    if (!frame || !info->overlay_rgba) {
        overlay_layer.contents = nil;
        overlay_id = 0;
    } else {
        CGRect v = video_layer.frame;
        double sx = v.size.width / info->width, sy = v.size.height / info->height;
        if (info->overlay_id != overlay_id) {
            /* Copy the pixels: the app reuses its buffer for the next subtitle. */
            CFDataRef data = CFDataCreate(NULL, info->overlay_rgba, (CFIndex)info->overlay_stride * info->overlay_h);
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
            CGImageRef img = CGImageCreate((size_t)info->overlay_w, (size_t)info->overlay_h, 8, 32,
                                           (size_t)info->overlay_stride, cs, kCGImageAlphaLast | kCGBitmapByteOrderDefault,
                                           dp, NULL, false, kCGRenderingIntentDefault);
            overlay_layer.contents = (__bridge id)img;
            CGImageRelease(img);
            CFRelease(data);
            CGDataProviderRelease(dp);
            CGColorSpaceRelease(cs);
            overlay_id = info->overlay_id;
        }
        /* Layer coordinates start at the bottom left; overlay positions at the top left. */
        overlay_layer.frame = CGRectMake(v.origin.x + info->overlay_x * sx,
                                         v.origin.y + v.size.height - (info->overlay_y + info->overlay_h) * sy,
                                         info->overlay_w * sx, info->overlay_h * sy);
    }
    [CATransaction commit];
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        NSApplication *app;
        UMDAppDelegate *delegate;

        if (app_init(argc, argv) != 0)
            return 1;
        app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        delegate = [[UMDAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
