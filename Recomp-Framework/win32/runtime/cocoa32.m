// The macOS host for win32 guests: AppKit owns the main thread, the guest's WinMain runs on a game thread.
// Windows the guest creates become NSWindows whose content view is backed by a CAMetalLayer (presented by the
// D3D9 layer). Keyboard and mouse events become WM_* messages and the key state GetAsyncKeyState reads.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include "rt32.h"

void rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint8_t rt_keys[256];                 // VK -> down
int rt_mouse_x, rt_mouse_y;
static id<MTLDevice> device;

id<MTLDevice> rt_metal_device(void) {
    if (!device) device = MTLCreateSystemDefaultDevice();
    return device;
}

// macOS virtual key code -> Windows VK
static uint8_t vk_of(unsigned short k) {
    static const uint8_t map[128] = {
        [0x00] = 'A', [0x0B] = 'B', [0x08] = 'C', [0x02] = 'D', [0x0E] = 'E', [0x03] = 'F', [0x05] = 'G', [0x04] = 'H',
        [0x22] = 'I', [0x26] = 'J', [0x28] = 'K', [0x25] = 'L', [0x2E] = 'M', [0x2D] = 'N', [0x1F] = 'O', [0x23] = 'P',
        [0x0C] = 'Q', [0x0F] = 'R', [0x01] = 'S', [0x11] = 'T', [0x20] = 'U', [0x09] = 'V', [0x0D] = 'W', [0x07] = 'X',
        [0x10] = 'Y', [0x06] = 'Z',
        [0x1D] = '0', [0x12] = '1', [0x13] = '2', [0x14] = '3', [0x15] = '4', [0x17] = '5', [0x16] = '6', [0x1A] = '7',
        [0x1C] = '8', [0x19] = '9',
        [0x24] = 0x0D, [0x35] = 0x1B, [0x31] = 0x20, [0x30] = 0x09, [0x33] = 0x08, [0x75] = 0x2E,
        [0x7B] = 0x25, [0x7E] = 0x26, [0x7C] = 0x27, [0x7D] = 0x28,
        [0x38] = 0x10, [0x3C] = 0x10, [0x3B] = 0x11, [0x3E] = 0x11, [0x3A] = 0x12, [0x3D] = 0x12,
        [0x7A] = 0x70, [0x78] = 0x71, [0x63] = 0x72, [0x76] = 0x73, [0x60] = 0x74, [0x61] = 0x75, [0x62] = 0x76,
        [0x64] = 0x77, [0x65] = 0x78, [0x6D] = 0x79, [0x67] = 0x7A, [0x6F] = 0x7B,
        [0x74] = 0x21, [0x79] = 0x22, [0x73] = 0x24, [0x77] = 0x23,
    };
    return k < 128 ? map[k] : 0;
}

// macOS virtual key code -> DirectInput scan code (DIK_*)
static uint8_t dik_of(unsigned short k) {
    static const uint8_t map[128] = {
        [0x35] = 0x01, [0x12] = 0x02, [0x13] = 0x03, [0x14] = 0x04, [0x15] = 0x05, [0x17] = 0x06, [0x16] = 0x07, [0x1A] = 0x08,
        [0x1C] = 0x09, [0x19] = 0x0A, [0x1D] = 0x0B, [0x1B] = 0x0C, [0x18] = 0x0D, [0x33] = 0x0E, [0x30] = 0x0F,
        [0x0C] = 0x10, [0x0D] = 0x11, [0x0E] = 0x12, [0x0F] = 0x13, [0x11] = 0x14, [0x10] = 0x15, [0x20] = 0x16, [0x22] = 0x17,
        [0x1F] = 0x18, [0x23] = 0x19, [0x21] = 0x1A, [0x1E] = 0x1B, [0x24] = 0x1C, [0x3B] = 0x1D,
        [0x00] = 0x1E, [0x01] = 0x1F, [0x02] = 0x20, [0x03] = 0x21, [0x05] = 0x22, [0x04] = 0x23, [0x26] = 0x24, [0x28] = 0x25,
        [0x25] = 0x26, [0x29] = 0x27, [0x27] = 0x28, [0x32] = 0x29, [0x38] = 0x2A, [0x2A] = 0x2B,
        [0x06] = 0x2C, [0x07] = 0x2D, [0x08] = 0x2E, [0x09] = 0x2F, [0x0B] = 0x30, [0x2D] = 0x31, [0x2E] = 0x32,
        [0x2B] = 0x33, [0x2F] = 0x34, [0x2C] = 0x35, [0x3C] = 0x36, [0x43] = 0x37, [0x3A] = 0x38, [0x31] = 0x39, [0x39] = 0x3A,
        [0x7A] = 0x3B, [0x78] = 0x3C, [0x63] = 0x3D, [0x76] = 0x3E, [0x60] = 0x3F, [0x61] = 0x40, [0x62] = 0x41, [0x64] = 0x42,
        [0x65] = 0x43, [0x6D] = 0x44, [0x67] = 0x57, [0x6F] = 0x58,
        [0x3E] = 0x9D, [0x3D] = 0xB8, [0x73] = 0xC7, [0x7E] = 0xC8, [0x74] = 0xC9, [0x7B] = 0xCB, [0x7C] = 0xCD, [0x77] = 0xCF,
        [0x7D] = 0xD0, [0x79] = 0xD1, [0x72] = 0xD2, [0x75] = 0xD3,
        [0x52] = 0x52, [0x53] = 0x4F, [0x54] = 0x50, [0x55] = 0x51, [0x56] = 0x4B, [0x57] = 0x4C, [0x58] = 0x4D, [0x59] = 0x47,
        [0x5B] = 0x48, [0x5C] = 0x49, [0x4C] = 0x9C, [0x45] = 0x4E, [0x4E] = 0x4A,
    };
    return k < 128 ? map[k] : 0;
}
void rt_dinput_key(uint8_t dik, int down);   // dinput8.c
int rt_mouse_dx, rt_mouse_dy, rt_mouse_dz;

// a key change (macOS virtual key code) -> DirectInput state, GetAsyncKeyState state and WM_KEYDOWN / WM_KEYUP
static void key_event(uint32_t hwnd, unsigned short code, int down) {
    uint8_t dik = dik_of(code);
    if (dik) rt_dinput_key(dik, down);
    uint8_t vk = vk_of(code);
    if (!vk) return;
    int repeat = down && rt_keys[vk];
    rt_keys[vk] = (uint8_t)down;
    uint32_t lp = 1u | ((uint32_t)code << 16) | (repeat ? 1u << 30 : 0) | (down ? 0 : 3u << 30);
    rt_post_message(hwnd, down ? 0x0100 : 0x0101, vk, lp);
}

// RT_INPUT="SECONDS:KEY,SECONDS:KEY,...": key presses (down, up 100 ms later) at the given times after the first
// window is created, for runs without a user (RT_HEADLESS). KEY: RETURN, ESCAPE, SPACE, TAB, UP, DOWN, LEFT, RIGHT,
// F1-F4, or one letter or digit. An entry that does not parse stops the program.
#define INPUT_MAX 64
static void input_schedule(uint32_t hwnd) {
    static int done;
    const char *spec = getenv("RT_INPUT");
    if (done || !spec || !*spec) return;
    done = 1;
    static const struct { const char *name; unsigned short code; } names[] = {
        {"RETURN", 0x24}, {"ESCAPE", 0x35}, {"SPACE", 0x31}, {"TAB", 0x30}, {"UP", 0x7E}, {"DOWN", 0x7D}, {"LEFT", 0x7B},
        {"RIGHT", 0x7C}, {"F1", 0x7A}, {"F2", 0x78}, {"F3", 0x63}, {"F4", 0x76},
    };
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static const unsigned short letter_codes[] = {0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25,
        0x2E, 0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06,
        0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19};
    const char *q = spec; int n = 0;
    while (*q) {
        if (n == INPUT_MAX) rt_fatal("RT_INPUT: more than %d entries", INPUT_MAX);
        char *end; double t = strtod(q, &end);
        if (end == q || *end != ':' || !(t >= 0.0 && t <= 3600.0)) rt_fatal("RT_INPUT: bad time in \"%s\"", q);
        q = end + 1;
        size_t len = strcspn(q, ",");
        int code = -1;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (strlen(names[i].name) == len && !strncasecmp(q, names[i].name, len)) code = names[i].code;
        if (code < 0 && len == 1) {
            const char *f = strchr(letters, toupper((unsigned char)*q));
            if (f && *f) code = letter_codes[f - letters];
        }
        if (code < 0) rt_fatal("RT_INPUT: unknown key in \"%.*s\"", (int)len, q);
        unsigned short c = (unsigned short)code;
        // the release is timed from the press, so a late press still stays down for 100 ms
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(t * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            if (rt_verbose) rt_log("[input] key 0x%02x pressed (RT_INPUT)", c);
            key_event(hwnd, c, 1);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                if (rt_verbose) rt_log("[input] key 0x%02x released (RT_INPUT)", c);
                key_event(hwnd, c, 0);
            });
        });
        n++;
        q += len;
        if (*q == ',') q++;
    }
}

@interface RtView : NSView
@property uint32_t hwnd;
@end
@implementation RtView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = rt_metal_device();
    // 10 bits per channel: the presentation dithers the 16-bit back buffer (rt_enh.h reduced banding) into 10 bits
    l.pixelFormat = MTLPixelFormatBGR10A2Unorm;
    l.framebufferOnly = NO;
    l.maximumDrawableCount = 3;
    // a window or full-screen area of another aspect ratio shows the whole image with black bars
    l.contentsGravity = kCAGravityResizeAspect;
    l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    return l;
}
- (void)key:(NSEvent *)e down:(int)down { key_event(self.hwnd, e.keyCode, down); }
- (void)keyDown:(NSEvent *)e { [self key:e down:1]; }
- (void)keyUp:(NSEvent *)e { [self key:e down:0]; }
- (void)flagsChanged:(NSEvent *)e {
    uint8_t vk = vk_of(e.keyCode); if (!vk) return;
    NSEventModifierFlags f = e.modifierFlags;
    int down = vk == 0x10 ? !!(f & NSEventModifierFlagShift) : vk == 0x11 ? !!(f & NSEventModifierFlagControl) : !!(f & NSEventModifierFlagOption);
    [self key:e down:down];
}
- (void)mouse:(NSEvent *)e msg:(uint32_t)msg {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSSize s = self.bounds.size;
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    // client coordinates in the game's back-buffer pixels; the image fills the view with its own aspect ratio
    // (kCAGravityResizeAspect), centered between black bars
    CGSize ds = l.drawableSize;
    double sc = fmin(s.width / ds.width, s.height / ds.height);
    if (!(sc > 0.0)) return;
    double x0 = (s.width - ds.width * sc) / 2.0, y0 = (s.height - ds.height * sc) / 2.0;
    double mx = (p.x - x0) / sc, my = (s.height - p.y - y0) / sc;
    rt_mouse_x = (int)fmin(fmax(mx, 0.0), ds.width - 1.0);
    rt_mouse_y = (int)fmin(fmax(my, 0.0), ds.height - 1.0);
    rt_post_message(self.hwnd, msg, 0, (uint32_t)(rt_mouse_x & 0xFFFF) | ((uint32_t)rt_mouse_y << 16));
}
- (void)mouseMoved:(NSEvent *)e {
    // trackpads report fractional movement: keep the remainder so that slow movement is not lost (main thread only)
    static double fx, fy;
    fx += e.deltaX; fy += e.deltaY;
    int ix = (int)fx, iy = (int)fy;
    fx -= ix; fy -= iy;
    __atomic_add_fetch(&rt_mouse_dx, ix, __ATOMIC_RELAXED); __atomic_add_fetch(&rt_mouse_dy, iy, __ATOMIC_RELAXED);
    [self mouse:e msg:0x0200];
}
- (void)mouseDragged:(NSEvent *)e { [self mouseMoved:e]; }
- (void)scrollWheel:(NSEvent *)e { __atomic_add_fetch(&rt_mouse_dz, (int)(e.scrollingDeltaY * 40), __ATOMIC_RELAXED); }
- (void)mouseDown:(NSEvent *)e { rt_keys[1] = 1; [self mouse:e msg:0x0201]; }
- (void)mouseUp:(NSEvent *)e { rt_keys[1] = 0; [self mouse:e msg:0x0202]; }
- (void)rightMouseDown:(NSEvent *)e { rt_keys[2] = 1; [self mouse:e msg:0x0204]; }
- (void)rightMouseUp:(NSEvent *)e { rt_keys[2] = 0; [self mouse:e msg:0x0205]; }
@end

@interface RtWindowDelegate : NSObject <NSWindowDelegate>
@property uint32_t hwnd;
@end
@implementation RtWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)w { rt_post_message(self.hwnd, 0x0010, 0, 0); return NO; }   // WM_CLOSE
- (void)windowDidBecomeKey:(NSNotification *)n { rt_post_message(self.hwnd, 0x0006, 1, 0); }        // WM_ACTIVATE
- (void)windowDidResignKey:(NSNotification *)n { memset(rt_keys, 0, sizeof rt_keys); rt_post_message(self.hwnd, 0x0006, 0, 0); }
@end

static NSMutableDictionary<NSNumber *, NSWindow *> *windows;

// WM_ACTIVATEAPP (wParam TRUE/FALSE) to every window when the app's activation changes, as Windows sends it to the
// process that gains or loses the foreground; games pause on FALSE. Main thread only; each change is sent once.
static int app_active_sent = -1;   // written on the main thread, read by rt_cocoa_foreground on the game thread
void rt_dinput_foreground(int on);   // dinput8.c
static pthread_mutex_t pause_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cv = PTHREAD_COND_INITIALIZER;
static void post_activate_app(int on) {
    if (on == app_active_sent || !windows.count) return;
    pthread_mutex_lock(&pause_mu);
    __atomic_store_n(&app_active_sent, on, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&pause_cv);
    pthread_mutex_unlock(&pause_mu);
    rt_dinput_foreground(on);
    for (NSNumber *h in windows) rt_post_message(h.unsignedIntValue, 0x001C, (uint32_t)on, 0);
}
int rt_cocoa_foreground(void) { return __atomic_load_n(&app_active_sent, __ATOMIC_RELAXED) == 1; }

// Pause in the background: while another app is active, the game thread waits at each Present (rt_pause_point),
// the audio mixers give silence (rt_paused) and the game's clocks (rt_clock_ns) do not advance. A headless run
// never pauses.
static int paused_now;             // the game thread waits in rt_pause_point
static uint64_t paused_total_ns;   // the time spent in rt_pause_point
static uint64_t mono_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
int rt_paused(void) { return __atomic_load_n(&paused_now, __ATOMIC_RELAXED); }
uint64_t rt_clock_ns(void) { return mono_ns() - __atomic_load_n(&paused_total_ns, __ATOMIC_RELAXED); }
void rt_pause_point(void) {
    if (rt_headless || rt_cocoa_foreground()) return;
    uint64_t t0 = mono_ns();
    __atomic_store_n(&paused_now, 1, __ATOMIC_RELAXED);
    rt_log("[cocoa] the app is in the background: the game is paused");
    pthread_mutex_lock(&pause_mu);
    while (__atomic_load_n(&app_active_sent, __ATOMIC_RELAXED) != 1) pthread_cond_wait(&pause_cv, &pause_mu);
    pthread_mutex_unlock(&pause_mu);
    __atomic_add_fetch(&paused_total_ns, mono_ns() - t0, __ATOMIC_RELAXED);
    __atomic_store_n(&paused_now, 0, __ATOMIC_RELAXED);
    rt_log("[cocoa] the app is active again after %.1f s: the game continues", (double)(mono_ns() - t0) / 1e9);
}

// DirectInput exclusive mouse (dinput8.c): while it is acquired, Windows hides the pointer and keeps it in the game's
// window. Here the pointer is moved to the window's center, hidden and detached from the mouse, which then only
// reports movement. A headless run never changes the user's pointer.
static int pointer_held;   // the pointer is detached and hidden; read by rt_cocoa_release_pointer on any thread
void rt_cocoa_mouse_capture(int on) {
    if (rt_headless) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (on == __atomic_load_n(&pointer_held, __ATOMIC_RELAXED)) return;
        if (on) {
            NSWindow *win = NSApp.keyWindow ?: windows.allValues.firstObject;
            if (win) {   // global display coordinates: origin at the top left of the primary screen
                NSRect f = win.frame;
                CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
                CGError e = CGWarpMouseCursorPosition(CGPointMake(NSMidX(f), top - NSMidY(f)));
                if (e != kCGErrorSuccess) rt_log("[cocoa] CGWarpMouseCursorPosition failed: CGError %d", (int)e);
            }
            CGError e = CGAssociateMouseAndMouseCursorPosition(false);
            if (e != kCGErrorSuccess) { rt_log("[cocoa] cannot detach the pointer from the mouse: CGError %d", (int)e); return; }
            [NSCursor hide];
        } else {
            CGError e = CGAssociateMouseAndMouseCursorPosition(true);
            if (e != kCGErrorSuccess) rt_log("[cocoa] cannot attach the pointer to the mouse: CGError %d", (int)e);
            [NSCursor unhide];
        }
        __atomic_store_n(&pointer_held, on, __ATOMIC_RELAXED);
    });
}
// process exit (ExitProcess, exit): attach the pointer to the mouse again; the hidden state ends with the process
void rt_cocoa_release_pointer(void) {
    if (__atomic_exchange_n(&pointer_held, 0, __ATOMIC_RELAXED)) CGAssociateMouseAndMouseCursorPosition(true);
}

// called from the game thread; returns the window's CAMetalLayer (retained)
void *rt_cocoa_create_window(uint32_t hwnd, const char *title, int w, int h) {
    __block CAMetalLayer *layer = nil;
    NSString *t = [NSString stringWithUTF8String:title ? title : ""] ?: @"";
    dispatch_sync(dispatch_get_main_queue(), ^{
        if (!windows) windows = [NSMutableDictionary new];
        NSRect r = NSMakeRect(0, 0, w, h);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = t;
        win.releasedWhenClosed = NO;
        RtView *v = [[RtView alloc] initWithFrame:r];
        v.hwnd = hwnd; v.wantsLayer = YES;
        win.contentView = v;
        RtWindowDelegate *d = [RtWindowDelegate new]; d.hwnd = hwnd;
        win.delegate = d;
        objc_setAssociatedObject(win, "rtdelegate", d, OBJC_ASSOCIATION_RETAIN);
        win.acceptsMouseMovedEvents = YES;
        // the green button, View > Enter Full Screen and Control-Command-F give macOS full screen
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        win.backgroundColor = NSColor.blackColor;
        [win center];
        [win makeFirstResponder:v];
        if (rt_headless) {
            // never ordered on screen and never activated; the guest gets the messages of an active foreground window
            windows[@(hwnd)] = win;
            __atomic_store_n(&app_active_sent, 1, __ATOMIC_RELAXED);
            rt_post_message(hwnd, 0x001C, 1, 0);   // WM_ACTIVATEAPP
            rt_post_message(hwnd, 0x0006, 1, 0);   // WM_ACTIVATE
        } else {
            [win makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            windows[@(hwnd)] = win;
            if (NSApp.active) { if (app_active_sent == 1) rt_post_message(hwnd, 0x001C, 1, 0); else post_activate_app(1); }
        }
        layer = (CAMetalLayer *)v.layer;
        layer.drawableSize = CGSizeMake(w, h);
        input_schedule(hwnd);
    });
    return (__bridge_retained void *)layer;
}

void rt_cocoa_set_size(uint32_t hwnd, int w, int h) {
    dispatch_async(dispatch_get_main_queue(), ^{
        NSWindow *win = windows[@(hwnd)];
        if (!win) return;
        // in macOS full screen the window keeps the screen's size; the image is scaled into it
        if (!(win.styleMask & NSWindowStyleMaskFullScreen)) [win setContentSize:NSMakeSize(w, h)];
        ((CAMetalLayer *)win.contentView.layer).drawableSize = CGSizeMake(w, h);
    });
}

// a full-screen swap chain (on = 1) puts the window in macOS full screen; a windowed one (on = 0) takes it out. The
// user can change it at any time with the green button, View > Enter Full Screen or Control-Command-F.
void rt_cocoa_full_screen(uint32_t hwnd, int on) {
    if (rt_headless) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSWindow *win = windows[@(hwnd)];
        if (!win || !!(win.styleMask & NSWindowStyleMaskFullScreen) == !!on) return;
        [win toggleFullScreen:nil];
    });
}

// HDR output (rt_enh.h): on = 1 makes the layer an extended dynamic range (EDR) layer (RGBA16Float, 1.0 = SDR
// white); on = 0 gives the SDR layer of makeBackingLayer back. The EDR color space is the extended linear form of
// the screen's own color space. The SDR layer has no color space (Core Animation does no color matching), so the
// game's colors use the primaries of the screen; with the screen's primaries the EDR layer shows the same colors.
// The change is made on the main thread, so the game sees it in the pixel format of a later drawable. Each call also
// refreshes the EDR headroom and the EDR color space of the window's screen (rt_cocoa_edr_headroom).
static float edr_headroom = 1.0f, edr_potential = 1.0f;
void rt_cocoa_layer_edr(void *layer_ref, int on, int refresh_only) {
    CAMetalLayer *l = (__bridge CAMetalLayer *)layer_ref;
    dispatch_async(dispatch_get_main_queue(), ^{
        id dl = l.delegate;
        NSScreen *sc = [dl isKindOfClass:[NSView class]] ? ((NSView *)dl).window.screen : nil;
        if (!refresh_only && !on) {
            l.wantsExtendedDynamicRangeContent = NO;
            l.pixelFormat = MTLPixelFormatBGR10A2Unorm;
            l.colorspace = NULL;
        } else if (!refresh_only || l.wantsExtendedDynamicRangeContent) {
            // a window on no screen (a headless run) takes the main screen's color space
            NSScreen *cs_screen = sc ? sc : NSScreen.mainScreen;
            CGColorSpaceRef base = cs_screen.colorSpace.CGColorSpace;
            CGColorSpaceRef cs = base ? CGColorSpaceCreateExtendedLinearized(base) : NULL;
            if (!cs) rt_fatal("[cocoa] the screen has no color space with an extended linear form; HDR output is not possible");
            if (!refresh_only) {
                l.wantsExtendedDynamicRangeContent = YES;
                l.pixelFormat = MTLPixelFormatRGBA16Float;
            }
            if (!l.colorspace || !CFEqual(l.colorspace, cs)) {
                l.colorspace = cs;
                if (rt_verbose) {
                    CFStringRef n = CGColorSpaceCopyName(base);
                    rt_log("[cocoa] EDR layer color space: the extended linear form of %s", n ? CFStringGetCStringPtr(n, kCFStringEncodingUTF8) ?: "(a name that is not UTF-8)" : "(no name)");
                    if (n) CFRelease(n);
                }
            }
            CGColorSpaceRelease(cs);
        }
        float h = sc ? (float)sc.maximumExtendedDynamicRangeColorComponentValue : 1.0f;
        float hp = sc ? (float)sc.maximumPotentialExtendedDynamicRangeColorComponentValue : 1.0f;
        __atomic_store(&edr_headroom, &h, __ATOMIC_RELAXED);
        __atomic_store(&edr_potential, &hp, __ATOMIC_RELAXED);
    });
}
// the largest EDR headroom of the window's screen (at a lower display brightness the headroom is larger)
float rt_cocoa_edr_potential(void) { float h; __atomic_load(&edr_potential, &h, __ATOMIC_RELAXED); return h < 1.0f ? 1.0f : h; }
float rt_cocoa_edr_headroom(void) { float h; __atomic_load(&edr_headroom, &h, __ATOMIC_RELAXED); return h < 1.0f ? 1.0f : h; }

void rt_cocoa_set_title(uint32_t hwnd, const char *title) {
    NSString *t = [NSString stringWithUTF8String:title] ?: @"";
    dispatch_async(dispatch_get_main_queue(), ^{ windows[@(hwnd)].title = t; });
}

void rt_cocoa_screen_size(int *w, int *h) {
    __block NSSize s;
    if ([NSThread isMainThread]) s = NSScreen.mainScreen.frame.size;
    else dispatch_sync(dispatch_get_main_queue(), ^{ s = NSScreen.mainScreen.frame.size; });
    *w = (int)s.width; *h = (int)s.height;
}

// ---- app ----
static void (*game_main)(void);
static void *game_thread(void *p) { (void)p; game_main(); return NULL; }

void rt_cocoa_run(void (*fn)(void)) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        // RT_HEADLESS: no Dock icon, no menu bar, no activation
        [NSApp setActivationPolicy:rt_headless ? NSApplicationActivationPolicyProhibited : NSApplicationActivationPolicyRegular];
        // a background app is put in App Nap, which delays and coalesces its timers (RT_INPUT) and throttles it
        static id<NSObject> no_nap;
        if (rt_headless)
            no_nap = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                                                                   reason:@"RT_HEADLESS run"];
        NSMenu *bar = [NSMenu new], *app = [NSMenu new];
        NSMenuItem *item = [NSMenuItem new]; item.submenu = app; [bar addItem:item];
        [app addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
        NSMenuItem *vitem = [NSMenuItem new]; vitem.submenu = view; [bar addItem:vitem];
        // AppKit names the item "Enter Full Screen" or "Exit Full Screen" for the key window
        NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        NSApp.mainMenu = bar;
        if (!rt_headless) {
            NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
            [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                        usingBlock:^(NSNotification *n) { post_activate_app(1); }];
            [nc addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil
                        usingBlock:^(NSNotification *n) { post_activate_app(0); }];
        }
        atexit(rt_cocoa_release_pointer);
        [NSApp finishLaunching];
        game_main = fn;
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256u << 20);   // translated frames are large; the guest recurses deeply at load
        // the game thread draws the frames: the QoS class of user-interactive work. With the default class, its
        // timed waits (the frame pacing of d3d9_metal.m) ended 1 to 3.6 ms late (measured).
        int qe = pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
        if (qe != 0) rt_fatal("[cocoa] pthread_attr_set_qos_class_np failed (%d)", qe);
        int ce = pthread_create(&t, &a, game_thread, NULL);
        if (ce != 0) rt_fatal("[cocoa] the game thread was not created (%d)", ce);
        pthread_attr_destroy(&a);
        [NSApp run];
    }
}
