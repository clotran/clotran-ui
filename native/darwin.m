// darwin.m: clotran-ui's few Cocoa-only pieces.
#import <Cocoa/Cocoa.h>
#include "ui.h"

// A window's own pixels, as Cocoa drew them -- the frame with its title
// bar, the widgets and the drawing area -- read back from the view itself
// (cacheDisplayInRect), so a test can show the real window without the
// screen-recording permission screencapture needs.
int clotran_ui_capture(uiWindow *w, const char *path)
{
	@autoreleasepool {
		NSWindow *nw = (NSWindow *) uiControlHandle(uiControl(w));
		NSView *v = [[nw contentView] superview];   // the frame view: title bar too
		if (v == nil) v = [nw contentView];
		NSRect r = [v bounds];
		NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:r];
		if (rep == nil) return 0;
		[v cacheDisplayInRect:r toBitmapImageRep:rep];
		NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
		return [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES] ? 1 : 0;
	}
}

// libui-ng gives an area no scroll wheel: a local monitor hands each
// scroll to clotran_ui.c, which zooms that window's 3D view. Once.
extern void clotran_ui_on_scroll(void *native_window, double dy);
void clotran_ui_watch_scroll(void)
{
	static id monitor;
	if (monitor) return;
	monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel handler:^NSEvent *(NSEvent *e) {
		clotran_ui_on_scroll((__bridge void *) [e window], -[e scrollingDeltaY] * ([e hasPreciseScrollingDeltas] ? 0.1 : 1.0));
		return e;
	}] retain];
}

// uiMainStep returns on an event, and a function uiQueueMain ran (a
// dispatch block) is none: an application-defined event, posted, makes it
// return so the loop looks at its state again.
void clotran_ui_wake(void)
{
	@autoreleasepool {
		NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
		                           modifierFlags:0 timestamp:0 windowNumber:0 context:nil
		                                 subtype:0 data1:0 data2:0];
		[NSApp postEvent:e atStart:NO];
	}
}

// A new window comes to the front, above the terminal or editor that
// started clotran: activation is only a request since macOS 14 (it can be
// refused while another app is active), so the window is also ordered
// front regardless of it.
void clotran_ui_front(uiWindow *w)
{
	@autoreleasepool {
		NSWindow *nw = (NSWindow *) uiControlHandle(uiControl(w));
		if ([NSApp respondsToSelector:@selector(activate)])
			[NSApp performSelector:@selector(activate)];
		else
			[NSApp activateIgnoringOtherApps:YES];
		[nw makeKeyAndOrderFront:nil];
		[nw orderFrontRegardless];
	}
}
