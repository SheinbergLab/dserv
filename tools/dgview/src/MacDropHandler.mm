/**
 * MacDropHandler.mm - macOS-specific file drop handling
 * 
 * FLTK's default DND handling doesn't register for file URL pasteboard types.
 * This helper registers the app's windows to accept file drops.
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

// Callback function type for when files are dropped
typedef void (*DropCallback)(const char* filename);

static DropCallback g_dropCallback = nullptr;

// Custom view that accepts file drops
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

// C++ interface
extern "C" {

void macSetDropCallback(DropCallback callback) {
    g_dropCallback = callback;
}

void macEnableFileDrop(Fl_Window* window) {
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

} // extern "C"

#endif // __APPLE__
