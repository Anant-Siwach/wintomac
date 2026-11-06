// Metal layers for windows a program creates off the main thread (docs/ps5_graphics.md).
//
// Cocoa only lets the main thread create windows, so user32.c gives a window created on another thread no SDL window.
// AnyPS5's libSceVideoOut does exactly that: its presenter thread owns the (SDL2, Win32) window and builds the Vulkan
// surface from its HWND while the PS5 program's main() keeps the main thread. For such a window vkCreateWin32SurfaceKHR
// gets a detached CAMetalLayer (no view, any thread may create one), so MoltenVK renders and presents normally.
// To make it visible (not WINTOMAC_HEADLESS), the layer is put into an NSWindow on the main thread: the request is
// queued on the main dispatch queue and runs when the main thread next waits in a Win32 wait/sleep shim
// (gfx_main_service) or pumps messages (SDL drains the main queue). The window shows the image; closing it or Cmd-Q
// quits the program. Keyboard, mouse and focus events of the window go to the program as Win32 messages for its HWND
// (user32_detached_*), so AnyPS5's SDL2 window code and its keyboard/mouse -> pad mapping work; game controllers reach
// it through the XInput shim (xinput.c, SDL3 gamepads), independent of the window.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

typedef struct { CAMetalLayer *layer; NSWindow *win; int visible; uint64_t hwnd; int w, h; unsigned mods; } Detached;
static Detached *registry[16];
void user32_detached_key(uint64_t hwnd, int sdl_scancode, int down, int repeat);   // user32.c
void user32_detached_mouse(uint64_t hwnd, int kind, int x, int y, int arg);
void user32_detached_focus(uint64_t hwnd, int on);

// macOS virtual key code (Carbon kVK_*) -> SDL scancode (USB HID usage); 0 = unmapped
static const unsigned char mac_to_sdl[128] = {
    [0x00] = 4, [0x0B] = 5, [0x08] = 6, [0x02] = 7, [0x0E] = 8, [0x03] = 9, [0x05] = 10, [0x04] = 11, [0x22] = 12,
    [0x26] = 13, [0x28] = 14, [0x25] = 15, [0x2E] = 16, [0x2D] = 17, [0x1F] = 18, [0x23] = 19, [0x0C] = 20, [0x0F] = 21,
    [0x01] = 22, [0x11] = 23, [0x20] = 24, [0x09] = 25, [0x0D] = 26, [0x07] = 27, [0x10] = 28, [0x06] = 29,
    [0x12] = 30, [0x13] = 31, [0x14] = 32, [0x15] = 33, [0x17] = 34, [0x16] = 35, [0x1A] = 36, [0x1C] = 37, [0x19] = 38, [0x1D] = 39,
    [0x24] = 40, [0x35] = 41, [0x33] = 42, [0x30] = 43, [0x31] = 44, [0x1B] = 45, [0x18] = 46, [0x21] = 47, [0x1E] = 48,
    [0x2A] = 49, [0x29] = 51, [0x27] = 52, [0x32] = 53, [0x2B] = 54, [0x2F] = 55, [0x2C] = 56, [0x39] = 57,
    [0x7A] = 58, [0x78] = 59, [0x63] = 60, [0x76] = 61, [0x60] = 62, [0x61] = 63, [0x62] = 64, [0x64] = 65, [0x65] = 66,
    [0x6D] = 67, [0x67] = 68, [0x6F] = 69, [0x73] = 74, [0x74] = 75, [0x75] = 76, [0x77] = 77, [0x79] = 78,
    [0x7C] = 79, [0x7B] = 80, [0x7D] = 81, [0x7E] = 82, [0x4C] = 88,
    [0x3B] = 224, [0x38] = 225, [0x3A] = 226, [0x37] = 227, [0x3E] = 228, [0x3C] = 229, [0x3D] = 230, [0x36] = 231,
};
static Detached *detached_of(NSWindow *w) {
    if (!w) return NULL;
    for (int i = 0; i < 16; i++) if (registry[i] && registry[i]->win == w) return registry[i];
    return NULL;
}
// Forward one event to the program; returns 1 if consumed (keys: not passed to AppKit, which would beep).
static int forward(Detached *d, NSEvent *e) {
    NSView *v = d->win.contentView;
    NSRect b = v.bounds;
    switch (e.type) {
    case NSEventTypeKeyDown: case NSEventTypeKeyUp: {
        int sc = e.keyCode < 128 ? mac_to_sdl[e.keyCode] : 0;
        if (sc) user32_detached_key(d->hwnd, sc, e.type == NSEventTypeKeyDown, e.type == NSEventTypeKeyDown && e.isARepeat);
        return 1;
    }
    case NSEventTypeFlagsChanged: {   // modifier keys: down if its device-dependent flag bit is now set
        static const struct { unsigned short kc; unsigned bit; } m[] = {
            { 0x38, 0x2 }, { 0x3C, 0x4 }, { 0x3B, 0x1 }, { 0x3E, 0x2000 }, { 0x3A, 0x20 }, { 0x3D, 0x40 }, { 0x37, 0x8 }, { 0x36, 0x10 } };
        for (unsigned i = 0; i < sizeof m / sizeof *m; i++) if (m[i].kc == e.keyCode) {
            unsigned now = (unsigned)e.modifierFlags & m[i].bit;
            if (now != (d->mods & m[i].bit)) user32_detached_key(d->hwnd, mac_to_sdl[e.keyCode], now != 0, 0);
            d->mods = (d->mods & ~m[i].bit) | now;
        }
        return 0;
    }
    default: break;
    }
    if (e.window != d->win) return 0;
    NSPoint p = [v convertPoint:e.locationInWindow fromView:nil];
    int x = b.size.width > 0 ? (int)(p.x * d->w / b.size.width) : 0, y = b.size.height > 0 ? (int)((b.size.height - p.y) * d->h / b.size.height) : 0;
    switch (e.type) {
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
        user32_detached_mouse(d->hwnd, 0, x, y, 0); break;
    case NSEventTypeLeftMouseDown: user32_detached_mouse(d->hwnd, 1, x, y, 1); break;
    case NSEventTypeLeftMouseUp: user32_detached_mouse(d->hwnd, 2, x, y, 1); break;
    case NSEventTypeRightMouseDown: user32_detached_mouse(d->hwnd, 1, x, y, 2); break;
    case NSEventTypeRightMouseUp: user32_detached_mouse(d->hwnd, 2, x, y, 2); break;
    case NSEventTypeOtherMouseDown: if (e.buttonNumber == 2) user32_detached_mouse(d->hwnd, 1, x, y, 3); break;
    case NSEventTypeOtherMouseUp: if (e.buttonNumber == 2) user32_detached_mouse(d->hwnd, 2, x, y, 3); break;
    case NSEventTypeScrollWheel: {
        double dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
        if (dy != 0) user32_detached_mouse(d->hwnd, 3, x, y, (int)(dy * 120));
        break;
    }
    default: break;
    }
    return 0;
}
static atomic_int pending;   // main-queue blocks queued and not yet run

static void on_main(void (^blk)(void)) {
    if (pthread_main_np()) { blk(); return; }
    atomic_fetch_add(&pending, 1);
    dispatch_async(dispatch_get_main_queue(), ^{ blk(); atomic_fetch_sub(&pending, 1); });
}

static atomic_int windows_shown;   // detached windows on screen: their events need pumping
static uint64_t last_pump_ns;

@interface GfxWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation GfxWindowDelegate
- (void)windowWillClose:(NSNotification *)n { (void)n; fflush(NULL); _exit(0); }   // closing the window quits the program
- (void)windowDidBecomeKey:(NSNotification *)n { Detached *d = detached_of(n.object); if (d) user32_detached_focus(d->hwnd, 1); }
- (void)windowDidResignKey:(NSNotification *)n { Detached *d = detached_of(n.object); if (d) user32_detached_focus(d->hwnd, 0); }
@end

// Run queued window work and pump window events when the main thread is about to wait (cheap otherwise; at most
// every 4 ms). Cmd-Q quits, like closing the window; other input goes to the program.
void gfx_main_service(void) {
    if ((!atomic_load(&pending) && !atomic_load(&windows_shown)) || !pthread_main_np()) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!atomic_load(&pending) && now - last_pump_ns < 4000000) return;
    last_pump_ns = now;
    @autoreleasepool {
        [NSApplication sharedApplication];
        for (int i = 0; i < 4 && atomic_load(&pending); i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        NSEvent *e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
            if (e.type == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand) && e.keyCode == 12) { fflush(NULL); _exit(0); }
            Detached *d = detached_of(e.window ? e.window : NSApp.keyWindow);
            if (d && forward(d, e)) continue;
            [NSApp sendEvent:e];
        }
    }
}

static void show_now(Detached *d, int w, int h, const char *title) {
    if (d->win) { [d->win orderFront:nil]; return; }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSRect r = NSMakeRect(0, 0, w, h);
    NSWindow *win = [[NSWindow alloc] initWithContentRect:r
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    win.releasedWhenClosed = NO;
    win.title = [NSString stringWithUTF8String:title ? title : ""];
    NSView *v = [[NSView alloc] initWithFrame:r];
    v.wantsLayer = YES;
    v.layer = d->layer;
    d->layer.frame = v.bounds;
    win.contentView = v;
    win.acceptsMouseMovedEvents = YES;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    static GfxWindowDelegate *delegate;
    if (!delegate) delegate = [[GfxWindowDelegate alloc] init];
    win.delegate = delegate;
    d->win = win;
    atomic_fetch_add(&windows_shown, 1);
}

void *gfx_detached_layer(uint64_t hwnd, int w, int h, const char *title, int visible) {
    Detached *d = calloc(1, sizeof *d);
    d->hwnd = hwnd; d->w = w > 0 ? w : 1280; d->h = h > 0 ? h : 720;
    for (int i = 0; i < 16; i++) if (!registry[i]) { registry[i] = d; break; }
    @autoreleasepool {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        CAMetalLayer *l = [[CAMetalLayer alloc] init];
        l.device = MTLCreateSystemDefaultDevice();
        l.drawableSize = CGSizeMake(w > 0 ? w : 1280, h > 0 ? h : 720);
        l.frame = CGRectMake(0, 0, w > 0 ? w : 1280, h > 0 ? h : 720);
        l.contentsScale = 1;
        [CATransaction commit];
        d->layer = l;
    }
    if (visible) {
        char *t = strdup(title ? title : "");
        d->visible = 1;
        on_main(^{ show_now(d, w, h, t); free(t); });
    }
    return d;
}
void *gfx_detached_metal_layer(void *p) { return p ? (__bridge void *)((Detached *)p)->layer : NULL; }
void gfx_detached_show(void *p, int w, int h, const char *title) {
    Detached *d = p;
    if (!d || d->visible) return;
    d->visible = 1;
    char *t = strdup(title ? title : "");
    on_main(^{ show_now(d, w, h, t); free(t); });
}
void gfx_detached_set_title(void *p, const char *title) {
    Detached *d = p;
    if (!d || !d->visible) return;
    char *t = strdup(title ? title : "");
    on_main(^{ if (d->win) d->win.title = [NSString stringWithUTF8String:t]; free(t); });
}
