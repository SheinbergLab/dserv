#include "qtcgraph.hpp"
#include <tcl.h>

// Only include cgraph headers in this module
extern "C" {
#include <cgraph.h>
#include <gbuf.h>
}

// Color name mapping
static QMap<QString, int> colorNameToIndex = {
    {"black", 0},
    {"blue", 1},
    {"dark_green", 2},
    {"cyan", 3},
    {"red", 4},
    {"magenta", 5},
    {"brown", 6},
    {"white", 7},
    {"gray", 8},
    {"grey", 8},
    {"light_blue", 9},
    {"green", 10},
    {"light_cyan", 11},
    {"deep_pink", 12},
    {"medium_purple", 13},
    {"yellow", 14},
    {"navy", 15},
    {"bright_white", 16},
    {"light_gray", 17},
    {"light_grey", 17}
};

// Bridge class to set up callbacks and access private members
class QtCGraphBridge {
public:
    static void setupCallbacks() {
        // Set cgraph callbacks to use QtCGraph methods
        setline((LHANDLER) QtCGraph::Line);
        setclearfunc((HANDLER) QtCGraph::Clearwin);
        setpoint((PHANDLER) QtCGraph::Point);
        setcolorfunc((COHANDLER) QtCGraph::Setcolor);
        setchar((THANDLER) QtCGraph::Char);
        settext((THANDLER) QtCGraph::Text);
        strwidthfunc((SWHANDLER) QtCGraph::Strwidth);
        strheightfunc((SHHANDLER) QtCGraph::Strheight);
        setfontfunc((SFHANDLER) QtCGraph::Setfont);
        setfilledpoly((FHANDLER) QtCGraph::FilledPolygon);
        setcircfunc((CHANDLER) QtCGraph::Circle);
    }
    
    // Set the graphics buffer in the widget
    static void setGraphicsBuffer(QtCGraph* widget, void* gbuf) {
        widget->m_gbuf = gbuf;
    }
    
    // Set the frame pointer in the widget (for read-only access)
    static void setFrame(QtCGraph* widget, void* frame) {
        widget->m_frame = (FRAME*)frame;
    }
};

// Tcl command implementations
static int qtcgraph_init_widget_cmd(ClientData data, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *const objv[])
{
    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_ptr width height");
        return TCL_ERROR;
    }
    
    void* ptr = nullptr;
    int width, height;
    
    if (Tcl_GetLongFromObj(interp, objv[1], (long*)&ptr) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &width) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[3], &height) != TCL_OK) return TCL_ERROR;
    
    QtCGraph* widget = static_cast<QtCGraph*>(ptr);
    
    // Create and initialize the graphics buffer
    GBUF_DATA* gbuf = (GBUF_DATA*) calloc(1, sizeof(GBUF_DATA));
    gbDisableGevents();
    gbInitGeventBuffer(gbuf);
    gbSetGeventBuffer(gbuf);
    gbEnableGevents();
    
    // Store in widget using bridge
    QtCGraphBridge::setGraphicsBuffer(widget, gbuf);
    
    // Set up resolution
    setresol(width, height);
    setwindow(0, 0, width-1, height-1);
    setfviewport(0, 0, 1, 1);
    setcolor(0);
    gbInitGevents();
    
    // Get the current frame pointer for read-only access
    FRAME* currentFrame = getframe();
    if (currentFrame) {
        QtCGraphBridge::setFrame(widget, currentFrame);
    }
    
    return TCL_OK;
}

static int qtcgraph_playback_cmd(ClientData data, Tcl_Interp *interp,
                                int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "gbuf_ptr");
        return TCL_ERROR;
    }
    
    void* ptr = nullptr;
    if (Tcl_GetLongFromObj(interp, objv[1], (long*)&ptr) != TCL_OK) return TCL_ERROR;
    
    GBUF_DATA* gbuf = static_cast<GBUF_DATA*>(ptr);
    QtCGraph* widget = QtCGraph::getCurrentInstance();
    
    if (gbuf && widget) {
        // Set the graphics buffer
        gbSetGeventBuffer(gbuf);
        
        // Make sure resolution matches widget size
        int width = widget->width();
        int height = widget->height();
        FRAME* f = getframe();
        if (f && (f->xsres != width || f->ysres != height)) {
            setresol(width, height);
            setwindow(0, 0, width-1, height-1);
            setfviewport(0, 0, 1, 1);
        }
        
        // Playback the events
        gbPlaybackGevents();
    }
    
    return TCL_OK;
}

static int qtcgraph_resize_cmd(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *const objv[])
{
    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_ptr width height");
        return TCL_ERROR;
    }
    
    void* ptr = nullptr;
    int width, height;
    
    if (Tcl_GetLongFromObj(interp, objv[1], (long*)&ptr) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &width) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[3], &height) != TCL_OK) return TCL_ERROR;
    
    QtCGraph* widget = static_cast<QtCGraph*>(ptr);
    
    if (widget->getGraphicsBuffer()) {
        GBUF_DATA* gbuf = static_cast<GBUF_DATA*>(widget->getGraphicsBuffer());
        gbSetGeventBuffer(gbuf);
        
        // Update cgraph resolution and window
        setresol(width, height);
        setwindow(0, 0, width-1, height-1);
        setfviewport(0, 0, 1, 1);
        
        // Trigger a repaint
        widget->refresh();
    }
    
    return TCL_OK;
}

static int qtcgraph_clear_cmd(ClientData data, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_ptr");
        return TCL_ERROR;
    }
    
    void* ptr = nullptr;
    if (Tcl_GetLongFromObj(interp, objv[1], (long*)&ptr) != TCL_OK) return TCL_ERROR;
    
    QtCGraph* widget = static_cast<QtCGraph*>(ptr);
    
    if (widget->getGraphicsBuffer()) {
        GBUF_DATA* gbuf = static_cast<GBUF_DATA*>(widget->getGraphicsBuffer());
        gbSetGeventBuffer(gbuf);
        gbResetGevents();
    }
    
    return TCL_OK;
}

static int qtcgraph_cleanup_cmd(ClientData data, Tcl_Interp *interp,
                               int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_ptr");
        return TCL_ERROR;
    }
    
    void* ptr = nullptr;
    if (Tcl_GetLongFromObj(interp, objv[1], (long*)&ptr) != TCL_OK) 
        return TCL_ERROR;
    
    QtCGraph* widget = static_cast<QtCGraph*>(ptr);
    
    void* gbuf = widget->getGraphicsBuffer();
    if (gbuf) {
        GBUF_DATA* gbufData = static_cast<GBUF_DATA*>(gbuf);
        
        // Disable and clear any pending events
        gbDisableGevents();
        gbSetGeventBuffer(gbufData);
        gbResetGevents();
        
        // Free the GBUF_DATA
        free(gbufData);
        QtCGraphBridge::setGraphicsBuffer(widget, nullptr);
    }
    
    return TCL_OK;
}

// Enhanced setcolor command that accepts names
static int qtcgraph_setcolor_cmd(ClientData data, Tcl_Interp *interp,
                                int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "color_index_or_name");
        return TCL_ERROR;
    }
    
    int colorIndex = -1;
    
    // Try to parse as integer first
    if (Tcl_GetIntFromObj(nullptr, objv[1], &colorIndex) == TCL_OK) {
        // It's a number, use it directly
    } else {
        // Try as a color name
        QString colorName = QString(Tcl_GetString(objv[1])).toLower();
        
        auto it = colorNameToIndex.find(colorName);
        if (it != colorNameToIndex.end()) {
            colorIndex = it.value();
        } else {
            Tcl_AppendResult(interp, "Unknown color name: ", 
                           Tcl_GetString(objv[1]), NULL);
            return TCL_ERROR;
        }
    }
    
    // Set color
    int oldColor = setcolor(colorIndex);
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(oldColor));
    return TCL_OK;
}

// List available colors
static int qtcgraph_colorlist_cmd(ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *const objv[])
{
    Tcl_Obj* dictObj = Tcl_NewDictObj();
    
    for (auto it = colorNameToIndex.begin(); it != colorNameToIndex.end(); ++it) {
        // Skip alternative spellings
        if (it.key() == "grey" || it.key() == "light_grey") continue;
        
        Tcl_Obj* key = Tcl_NewStringObj(it.key().toUtf8().constData(), -1);
        Tcl_Obj* value = Tcl_NewIntObj(it.value());
        Tcl_DictObjPut(interp, dictObj, key, value);
    }
    
    Tcl_SetObjResult(interp, dictObj);
    return TCL_OK;
}

// Export to PDF with dialog
static int qtcgraph_export_cmd(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *const objv[])
{
    QString filename;
    
    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?filename?");
        return TCL_ERROR;
    }
    
    QtCGraph* widget = static_cast<QtCGraph*>(
        Tcl_GetAssocData(interp, "qtcgraph_widget", nullptr));
    
    if (!widget) {
        Tcl_SetResult(interp, "No widget associated with interpreter", TCL_STATIC);
        return TCL_ERROR;
    }
    
    bool success;
    if (objc == 2) {
        // Filename provided
        filename = Tcl_GetString(objv[1]);
        success = widget->exportToPDF(filename);
    } else {
        // No filename - show dialog
        success = widget->exportToPDFDialog();
    }
    
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(success));
    return TCL_OK;
}

static int qtcgraph_bind_cmd(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "event script");
        return TCL_ERROR;
    }
    
    QtCGraph* widget = static_cast<QtCGraph*>(
        Tcl_GetAssocData(interp, "qtcgraph_widget", nullptr));
    
    if (!widget) {
        Tcl_SetResult(interp, "No widget associated with interpreter", TCL_STATIC);
        return TCL_ERROR;
    }
    
    const char* event = Tcl_GetString(objv[1]);
    const char* script = Tcl_GetString(objv[2]);
    
    // Mouse events
    if (strcmp(event, "<ButtonPress>") == 0 || strcmp(event, "<Button>") == 0) {
        widget->setMouseDownScript(script);
    } else if (strcmp(event, "<ButtonRelease>") == 0) {
        widget->setMouseUpScript(script);
    } else if (strcmp(event, "<Motion>") == 0) {
        widget->setMouseMoveScript(script);
        widget->setMouseTracking(strlen(script) > 0);
    } else if (strcmp(event, "<Double-Button>") == 0) {
        widget->setMouseDoubleClickScript(script);
    } else if (strcmp(event, "<MouseWheel>") == 0) {
        widget->setMouseWheelScript(script);
    }
    // Keyboard events
    else if (strcmp(event, "<KeyPress>") == 0 || strcmp(event, "<Key>") == 0) {
        widget->setKeyPressScript(script);
    } else if (strcmp(event, "<KeyRelease>") == 0) {
        widget->setKeyReleaseScript(script);
    }
    // Focus events
    else if (strcmp(event, "<FocusIn>") == 0) {
        widget->setFocusInScript(script);
    } else if (strcmp(event, "<FocusOut>") == 0) {
        widget->setFocusOutScript(script);
    }
    else {
        Tcl_AppendResult(interp, "Unknown event: ", event, 
            ". Supported events: <ButtonPress>, <ButtonRelease>, <Motion>, "
            "<Double-Button>, <MouseWheel>, <KeyPress>, <KeyRelease>, "
            "<FocusIn>, <FocusOut>", NULL);
        return TCL_ERROR;
    }
    
    return TCL_OK;
}

// Override the cgraph flushwin command
static int cgFlushwinCmd(ClientData data, Tcl_Interp *interp,
                        int objc, Tcl_Obj * const objv[])
{
    // Get widget from interpreter
    QtCGraph* widget = static_cast<QtCGraph*>(
        Tcl_GetAssocData(interp, "qtcgraph_widget", nullptr));
    
    if (widget) {
        widget->refresh();
    }
    return TCL_OK;
}

// Create namespace and convenience commands
static void createConvenienceCommands(Tcl_Interp *interp)
{
    // Convenience binding command
    Tcl_Eval(interp, 
        "proc cgbind {event script} { "
        "    qtcgraph_bind $event $script "
        "}");
    
    // Clear command
    Tcl_Eval(interp,
        "proc cgclear {} { "
        "    qtcgraph_clear "
        "}");

    Tcl_Eval(interp,
        "namespace eval ::cg {}; "
        
        // Graph-specific commands in cg:: namespace
        "proc ::cg::colorlist {} { "
        "    qtcgraph_colorlist "
        "}; "
        
        "proc ::cg::export {{filename {}}} { "
        "    if {$filename eq {}} { "
        "        qtcgraph_export "
        "    } else { "
        "        qtcgraph_export $filename "
        "    } "
        "}; "
        
        // Override global setcolor to support names
        "rename setcolor _original_setcolor; "
        "proc setcolor {color} { "
        "    qtcgraph_setcolor $color "
        "}");
}

// Extension initialization
extern "C" int Qtcgraph_Init(Tcl_Interp *interp)
{
    if (Tcl_InitStubs(interp, "9.0", 0) == nullptr) {
        return TCL_ERROR;
    }
    
    if (Tcl_PkgProvide(interp, "qtcgraph", "1.0") != TCL_OK) {
        return TCL_ERROR;
    }

    // Set up the cgraph callbacks
    QtCGraphBridge::setupCallbacks();

    // Register widget commands
    Tcl_CreateObjCommand(interp, "qtcgraph_init_widget",
                        (Tcl_ObjCmdProc *) qtcgraph_init_widget_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_playback",
                        (Tcl_ObjCmdProc *) qtcgraph_playback_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_resize",
                        (Tcl_ObjCmdProc *) qtcgraph_resize_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_clear",
                        (Tcl_ObjCmdProc *) qtcgraph_clear_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_cleanup",
                        (Tcl_ObjCmdProc *) qtcgraph_cleanup_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_bind",
                        (Tcl_ObjCmdProc *) qtcgraph_bind_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_setcolor",
                        (Tcl_ObjCmdProc *) qtcgraph_setcolor_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_colorlist",
                        (Tcl_ObjCmdProc *) qtcgraph_colorlist_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_export",
                        (Tcl_ObjCmdProc *) qtcgraph_export_cmd,
                        (ClientData) NULL, NULL);    
    
    // Override flushwin command
    Tcl_CreateObjCommand(interp, "flushwin",
                        (Tcl_ObjCmdProc *) cgFlushwinCmd,
                        (ClientData) NULL, NULL);

    // Create convenience commands
    createConvenienceCommands(interp);

    return TCL_OK;
}