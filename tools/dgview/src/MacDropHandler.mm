/**
 * MacDropHandler.mm - macOS-specific file drop handling and app activation
 * 
 * FLTK's default DND handling doesn't register for file URL pasteboard types.
 * This helper registers the app's windows to accept file drops.
 * 
 * Also provides app activation support for single-instance behavior.
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

// Callback function type for when files are dropped
typedef void (*DropCallback)(const char* filename);

static DropCallback g_dropCallback = nullptr;

//============================================================================
// Application Activation Support
//============================================================================

extern "C" void macActivateApp(void) {
    @autoreleasepool {
        NSWindow* window = [NSApp mainWindow] ?: [NSApp keyWindow];
        
        if (window) {
            // Force the window to move to the current Space
            [window setCollectionBehavior:NSWindowCollectionBehaviorMoveToActiveSpace];
            [window makeKeyAndOrderFront:nil];
            
            // Reset to default behavior after a brief delay so window 
            // stays put if user switches Spaces manually later
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)), 
                          dispatch_get_main_queue(), ^{
                [window setCollectionBehavior:NSWindowCollectionBehaviorDefault];
            });
        }
        
        [NSApp activateIgnoringOtherApps:YES];
    }
}

extern "C" void macBringWindowToFront(Fl_Window* flWindow) {
    @autoreleasepool {
        [NSApp activateIgnoringOtherApps:YES];
        
        if (flWindow && flWindow->shown()) {
            NSWindow* nswin = (NSWindow*)fl_xid(flWindow);
            if (nswin) {
                [nswin makeKeyAndOrderFront:nil];
            }
        }
    }
}

//============================================================================
// Custom Application Delegate for Dock Drop and Reopen Handling
//============================================================================

@interface DgViewAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation DgViewAppDelegate

- (void)bringWindowToCurrentSpace {
    // Small delay to let window state settle (FLTK windows need this)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.01 * NSEC_PER_SEC)), 
                  dispatch_get_main_queue(), ^{
        [self doBringWindowToCurrentSpace];
    });
}

- (void)doBringWindowToCurrentSpace {
    // Try multiple ways to find our window
    NSWindow* window = [NSApp mainWindow];
    
    if (!window) {
        window = [NSApp keyWindow];
    }
    
    if (!window) {
        // FLTK windows don't register as mainWindow/keyWindow
        // Find the FLWindow in the app's window list
        NSArray* windows = [NSApp windows];
        for (NSWindow* w in windows) {
            // Skip system windows like TUINSWindow (text input)
            NSString* className = NSStringFromClass([w class]);
            if ([className isEqualToString:@"FLWindow"]) {
                window = w;
                break;
            }
        }
        
        // Fallback: any visible window that can become main
        if (!window) {
            for (NSWindow* w in windows) {
                if ([w isVisible] || [w canBecomeMainWindow]) {
                    window = w;
                    break;
                }
            }
        }
    }
    
    if (window) {
        [window setCollectionBehavior:NSWindowCollectionBehaviorMoveToActiveSpace];
        [window makeKeyAndOrderFront:nil];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)), 
                      dispatch_get_main_queue(), ^{
            [window setCollectionBehavior:NSWindowCollectionBehaviorDefault];
        });
    }
    
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)application:(NSApplication *)sender openFile:(NSString *)filename {
    if (g_dropCallback && filename) {
        g_dropCallback([filename UTF8String]);
    }
    
    // Bring to current Space after file is opened (slight delay to let window update)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)), 
                  dispatch_get_main_queue(), ^{
        [self bringWindowToCurrentSpace];
    });
    
    return YES;
}

- (void)application:(NSApplication *)sender openFiles:(NSArray<NSString *> *)filenames {
    if (g_dropCallback) {
        for (NSString* filename in filenames) {
            if (filename) {
                g_dropCallback([filename UTF8String]);
            }
        }
    }
    
    // Bring to current Space after files are opened
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)), 
                  dispatch_get_main_queue(), ^{
        [self bringWindowToCurrentSpace];
    });
    
    [sender replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)flag {
    // When clicking dock icon with no visible windows, bring to current Space
    if (!flag) {
        [self bringWindowToCurrentSpace];
    }
    return YES;
}

@end

static DgViewAppDelegate* g_appDelegate = nil;

//============================================================================
// Custom View for Window Drop Target
//============================================================================

@interface DropTargetView : NSView
@end

@implementation DropTargetView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    }
    return self;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([pb canReadObjectForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]) {
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    NSArray<NSURL*> *urls = [pb readObjectsForClasses:@[[NSURL class]] 
                                              options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    
    if (urls && g_dropCallback) {
        for (NSURL *url in urls) {
            if ([url isFileURL]) {
                const char* path = [[url path] UTF8String];
                if (path) {
                    g_dropCallback(path);
                }
            }
        }
        return YES;
    }
    return NO;
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    return YES;
}

@end

//============================================================================
// C++ Interface
//============================================================================

extern "C" {

void macSetDropCallback(DropCallback callback) {
    g_dropCallback = callback;
}

void macEnableFileDrop(Fl_Window* window) {
    @autoreleasepool {
        // Install our custom app delegate if not already done
        // This handles dock icon drops and file open events
        if (g_appDelegate == nil) {
            g_appDelegate = [[DgViewAppDelegate alloc] init];
            [NSApp setDelegate:g_appDelegate];
        }
        
        if (!window->shown()) return;
        
        NSWindow* nswin = (NSWindow*)fl_xid(window);
        if (!nswin) return;
        
        NSView* contentView = [nswin contentView];
        if (!contentView) return;
        
        // Check if we already added our drop target
        for (NSView* subview in [contentView subviews]) {
            if ([subview isKindOfClass:[DropTargetView class]]) {
                return; // Already set up
            }
        }
        
        // Create invisible drop target view covering the whole window
        DropTargetView* dropView = [[DropTargetView alloc] initWithFrame:[contentView bounds]];
        [dropView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [dropView setWantsLayer:YES];
        [dropView.layer setBackgroundColor:[[NSColor clearColor] CGColor]];
        
        // Insert at back so it doesn't interfere with other views
        [contentView addSubview:dropView positioned:NSWindowBelow relativeTo:nil];
    }
}

} // extern "C"

#endif // __APPLE__
