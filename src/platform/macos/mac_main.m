/* macOS: window, display, input, and the main loop. The window is 960x544, like the Vita screen. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "../platform.h"
#include "../../core/deint.h"
#include "mac_frame.h"
#include "../../app/library.h"

#include <mach-o/dyld.h>

#include <sys/stat.h>

#define WINDOW_W 960
#define WINDOW_H 544

static uint32_t held_buttons;
static int touch_down, touch_x, touch_y;   /* the mouse stands in for the touch screen */
static double scroll_acc;                  /* wheel distance not yet given out as steps */
static CALayer *video_layer, *overlay_layer, *ui_layer;
static uint32_t overlay_id, ui_id;
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
    case 53:  return PLAT_BTN_CIRCLE;    /* escape */
    case 0:   return PLAT_BTN_SQUARE;    /* A */
    case 13:  return PLAT_BTN_TRIANGLE;  /* W */
    case 12:  return PLAT_BTN_L;         /* Q */
    case 14:  return PLAT_BTN_R;         /* E */
    case 36:  return PLAT_BTN_CROSS;     /* return, as space */
    case 76:  return PLAT_BTN_CROSS;     /* keypad enter */
    case 122: return PLAT_BTN_HELP;      /* F1 */
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
    /* Control-F11: full screen (macOS keeps F11 alone for Show Desktop). */
    if (e.keyCode == 103 && (e.modifierFlags & NSEventModifierFlagControl)) {
        if (!e.isARepeat)
            [self.window toggleFullScreen:nil];
        return;
    }
    held_buttons |= button_for_key(e.keyCode);
}
- (void)keyUp:(NSEvent *)e { held_buttons &= ~button_for_key(e.keyCode); }
- (void)track:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSSize s = self.bounds.size;
    touch_x = s.width > 0 ? (int)(p.x * 960 / s.width) : 0;
    touch_y = s.height > 0 ? (int)((s.height - p.y) * 544 / s.height) : 0;   /* view y goes up */
}
- (void)mouseDown:(NSEvent *)e { [self track:e]; touch_down = 1; }
- (void)mouseDragged:(NSEvent *)e { [self track:e]; }
- (void)mouseUp:(NSEvent *)e { (void)e; touch_down = 0; }
/* A wheel notch is about one line; a trackpad reports points, so a step takes a swipe of 40. */
- (void)scrollWheel:(NSEvent *)e { scroll_acc -= e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 40 : e.scrollingDeltaY; }
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
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
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
    ui_layer = [CALayer layer];
    ui_layer.contentsGravity = kCAGravityResize;
    ui_layer.magnificationFilter = kCAFilterNearest;
    [content_view.layer addSublayer:ui_layer];
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

int plat_keyboard_ui(void)
{
    return 1;                 /* no gamepads on macOS yet */
}

int plat_stick_y(void)
{
    return 0;
}

int plat_touch(int *x, int *y)
{
    if (!touch_down)
        return 0;
    *x = touch_x;
    *y = touch_y;
    return 1;
}

int plat_scroll(void)
{
    int steps = (int)scroll_acc;     /* toward zero: the rest waits for more scrolling */
    scroll_acc -= steps;
    return steps;
}

static CGRect screen_rect(plat_rect r)
{
    CGRect b = content_view.bounds;
    /* Layer coordinates start at the bottom left; plat_rect at the top left. */
    return CGRectMake(r.x * b.size.width, (1 - r.y - r.h) * b.size.height, r.w * b.size.width, r.h * b.size.height);
}

static CGImageRef image_from_rgba(const uint8_t *rgba, int w, int h, int stride)
{
    /* Copy the pixels: the app reuses its buffers. */
    CFDataRef data = CFDataCreate(NULL, rgba, (CFIndex)stride * h);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)stride, cs,
                                   kCGImageAlphaLast | kCGBitmapByteOrderDefault, dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    CFRelease(data);
    return img;
}

void plat_present(plat_frame *frame, const plat_present_info *info)
{
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
        /* contentsRect is in unit coordinates of the image, origin at the bottom left. */
        video_layer.contentsRect = CGRectMake((CGFloat)info->src_x / info->video_w,
                                              1 - (CGFloat)(info->src_y + info->src_h) / info->video_h,
                                              (CGFloat)info->src_w / info->video_w, (CGFloat)info->src_h / info->video_h);
        video_layer.frame = screen_rect(info->dst);
    }
    if (!frame || !info->overlay_rgba) {
        overlay_layer.contents = nil;
        overlay_id = 0;
    } else {
        if (info->overlay_id != overlay_id) {
            CGImageRef img = image_from_rgba(info->overlay_rgba, info->overlay_w, info->overlay_h, info->overlay_stride);
            overlay_layer.contents = (__bridge id)img;
            CGImageRelease(img);
            overlay_id = info->overlay_id;
        }
        overlay_layer.frame = screen_rect(info->overlay_dst);
    }
    if (!info->ui_rgba) {
        ui_layer.contents = nil;
        ui_id = 0;
    } else {
        if (info->ui_id != ui_id) {
            CGImageRef img = image_from_rgba(info->ui_rgba, info->ui_w, info->ui_h, info->ui_stride);
            ui_layer.contents = (__bridge id)img;
            CGImageRelease(img);
            ui_id = info->ui_id;
        }
        ui_layer.frame = content_view.bounds;
    }
    [CATransaction commit];
}

int plat_draws_sprites(void) { return 0; }

void plat_screen_size(int *w, int *h)
{
    *w = (int)content_view.bounds.size.width;
    *h = (int)content_view.bounds.size.height;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        NSApplication *app;
        UMDAppDelegate *delegate;

        /* without a folder, the library is ~/UMD */
        char lib[1024], *args[64];
        int n = 0, have_path = 0;
        for (int i = 0; i < argc && n < 62; i++) {
            /* -t and -a take a value; anything else not starting with '-' is the folder or disc */
            if (i > 0 && argv[i][0] != '-' && strcmp(argv[i - 1], "-t") != 0 && strcmp(argv[i - 1], "-a") != 0)
                have_path = 1;
            args[n++] = argv[i];
        }
        if (!have_path) {
            /* the app's folder if it holds disc images, else a UMD folder next to it, else ~/UMD */
            char exe[1024], dir[1024] = "", *slash;
            uint32_t size = sizeof(exe);
            if (_NSGetExecutablePath(exe, &size) == 0 && realpath(exe, dir) && (slash = strrchr(dir, '/')))
                *slash = 0;
            /* in UMDPlayer.app/Contents/MacOS: the folder that holds the .app */
            if ((slash = strstr(dir, ".app/Contents/MacOS")) && slash[19] == 0) {
                while (slash > dir && *slash != '/')
                    slash--;
                *slash = 0;
            }
            snprintf(lib, sizeof(lib), "%s/UMD", dir);
            if (dir[0] && library_has_discs(dir)) {
                snprintf(lib, sizeof(lib), "%s", dir);
            } else if (!dir[0] || !library_has_discs(lib)) {
                snprintf(lib, sizeof(lib), "%s/UMD", getenv("HOME") ? getenv("HOME") : ".");
                mkdir(lib, 0755);
            }
            memmove(&args[2], &args[1], (size_t)(n - 1) * sizeof(args[0]));
            args[1] = lib;
            n++;
        }
        args[n] = NULL;
        if (app_init(n, args) != 0)
            return 1;
        app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        delegate = [[UMDAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
