#include "EssGraphicsWidget.h"
#include <tcl.h>

// cgraph headers available at compile time
extern "C" {
#include <cgraph.h>
#include <gbuf.h>
}

// Bridge class implementation - NOW IN TCL PACKAGE where cgraph functions are available
void EssGraphicsBridge::setupCallbacks() {
    // Set cgraph callbacks to use EssGraphicsWidget methods
    setline((LHANDLER) EssGraphicsWidget::Line);
    setclearfunc((HANDLER) EssGraphicsWidget::Clearwin);
    setpoint((PHANDLER) EssGraphicsWidget::Point);
    setcolorfunc((COHANDLER) EssGraphicsWidget::Setcolor);
    setchar((THANDLER) EssGraphicsWidget::Char);
    settext((THANDLER) EssGraphicsWidget::Text);
    strwidthfunc((SWHANDLER) EssGraphicsWidget::Strwidth);
    strheightfunc((SHHANDLER) EssGraphicsWidget::Strheight);
    setfontfunc((SFHANDLER) EssGraphicsWidget::Setfont);
    setfilledpoly((FHANDLER) EssGraphicsWidget::FilledPolygon);
    setcircfunc((CHANDLER) EssGraphicsWidget::Circle);
}

void EssGraphicsBridge::setGraphicsBuffer(EssGraphicsWidget* widget, void* gbuf) {
    widget->m_gbuf = gbuf;
}

void EssGraphicsBridge::setFrame(EssGraphicsWidget* widget, void* frame) {
    widget->m_frame = (FRAME*)frame;
}

// Enhanced setcolor command that accepts names (from original QtCGraph)
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
        
        static QMap<QString, int> colorNameToIndex = {
            {"black", 0}, {"blue", 1}, {"dark_green", 2}, {"cyan", 3},
            {"red", 4}, {"magenta", 5}, {"brown", 6}, {"white", 7},
            {"gray", 8}, {"grey", 8}, {"light_blue", 9}, {"green", 10},
            {"light_cyan", 11}, {"deep_pink", 12}, {"medium_purple", 13},
            {"yellow", 14}, {"navy", 15}, {"bright_white", 16},
            {"light_gray", 17}, {"light_grey", 17}
        };
        
        auto it = colorNameToIndex.find(colorName);
        if (it != colorNameToIndex.end()) {
            colorIndex = it.value();
        } else {
            Tcl_AppendResult(interp, "Unknown color name: ", 
                           Tcl_GetString(objv[1]), NULL);
            return TCL_ERROR;
        }
    }
    
    // Set color using cgraph
    int oldColor = setcolor(colorIndex);
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(oldColor));
    return TCL_OK;
}

// List available colors (from original QtCGraph)
static int qtcgraph_colorlist_cmd(ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *const objv[])
{
    static QMap<QString, int> colorNameToIndex = {
        {"black", 0}, {"blue", 1}, {"dark_green", 2}, {"cyan", 3},
        {"red", 4}, {"magenta", 5}, {"brown", 6}, {"white", 7},
        {"gray", 8}, {"light_blue", 9}, {"green", 10},
        {"light_cyan", 11}, {"deep_pink", 12}, {"medium_purple", 13},
        {"yellow", 14}, {"navy", 15}, {"bright_white", 16},
        {"light_gray", 17}
    };
    
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

// Export to PDF with dialog (from original QtCGraph)
static int qtcgraph_export_cmd(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *const objv[])
{
    QString filename;
    
    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?filename?");
        return TCL_ERROR;
    }
    
    EssGraphicsWidget* widget = static_cast<EssGraphicsWidget*>(
        Tcl_GetAssocData(interp, "scriptable_widget", nullptr));
    
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

// Event binding command (from original QtCGraph)
static int qtcgraph_bind_cmd(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "event script");
        return TCL_ERROR;
    }
    
    EssGraphicsWidget* widget = static_cast<EssGraphicsWidget*>(
        Tcl_GetAssocData(interp, "scriptable_widget", nullptr));
    
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

// Updated Tcl command implementations that work with EssGraphicsWidget
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
    
    EssGraphicsWidget* widget = static_cast<EssGraphicsWidget*>(ptr);
    
    // Create and initialize the graphics buffer
    GBUF_DATA* gbuf = (GBUF_DATA*) calloc(1, sizeof(GBUF_DATA));
    gbDisableGevents();
    gbInitGeventBuffer(gbuf);
    gbSetGeventBuffer(gbuf);
    gbEnableGevents();
    
    // Store in widget using bridge
    EssGraphicsBridge::setGraphicsBuffer(widget, gbuf);
    
    // Set up resolution
    setresol(width, height);
    setwindow(0, 0, width-1, height-1);
    setfviewport(0, 0, 1, 1);
    setcolor(0);
    gbInitGevents();
    
    // Get the current frame pointer for read-only access
    FRAME* currentFrame = getframe();
    if (currentFrame) {
        EssGraphicsBridge::setFrame(widget, currentFrame);
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
    EssGraphicsWidget* widget = EssGraphicsWidget::getCurrentInstance();
    
    if (gbuf && widget) {
        // Set the graphics buffer
        gbSetGeventBuffer(gbuf);
        
        // Make sure resolution matches widget size
        int width = widget->graphWidget()->width();
        int height = widget->graphWidget()->height();

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
    
    EssGraphicsWidget* widget = static_cast<EssGraphicsWidget*>(ptr);
    
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
    
    EssGraphicsWidget* widget = static_cast<EssGraphicsWidget*>(ptr);
    
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
    
    EssGraphicsWidget* widget = static_cast<EssGraphicsWidget*>(ptr);
    
    void* gbuf = widget->getGraphicsBuffer();
    if (gbuf) {
        GBUF_DATA* gbufData = static_cast<GBUF_DATA*>(gbuf);
        
        // Disable and clear any pending events
        gbDisableGevents();
        gbSetGeventBuffer(gbufData);
        gbResetGevents();
        
        // Free the GBUF_DATA
        free(gbufData);
        EssGraphicsBridge::setGraphicsBuffer(widget, nullptr);
    }
    
    return TCL_OK;
}

// Override the cgraph flushwin command to work with EssGraphicsWidget
static int cgFlushwinCmd(ClientData data, Tcl_Interp *interp,
                        int objc, Tcl_Obj * const objv[])
{
    // Get widget from interpreter
    EssGraphicsWidget* widget = static_cast<EssGraphicsWidget*>(
        Tcl_GetAssocData(interp, "scriptable_widget", nullptr));
    
    if (widget) {
        widget->refresh();
    }
    return TCL_OK;
}

static QColor parseColorString(const QString& colorString, QString* errorMsg = nullptr)
{
    QColor color;
    
    // First try Qt's built-in color name recognition
    color = QColor::fromString(colorString);
    
    if (color.isValid()) {
        return color;
    }
    
    // If Qt doesn't recognize it, try cgraph-specific mappings
    QString lowerColor = colorString.toLower();
    
    // Try parsing as cgraph color index
    bool isNumber;
    int colorIndex = colorString.toInt(&isNumber);
    
    if (isNumber) {
        // Convert cgraph color index to Qt color
        switch (colorIndex) {
            case 0: return QColor::fromString("black");
            case 1: return QColor::fromString("blue");
            case 2: return QColor::fromString("darkgreen");
            case 3: return QColor::fromString("cyan");
            case 4: return QColor::fromString("red");
            case 5: return QColor::fromString("magenta");
            case 6: return QColor::fromString("brown");
            case 7: return QColor::fromString("white");
            case 8: return QColor::fromString("gray");
            case 9: return QColor::fromString("lightblue");
            case 10: return QColor::fromString("green");
            case 11: return QColor::fromString("lightcyan");
            case 12: return QColor::fromString("deeppink");
            case 13: return QColor::fromString("mediumpurple");
            case 14: return QColor::fromString("yellow");
            case 15: return QColor::fromString("navy");
            case 16: return QColor::fromString("white");
            case 17: return QColor::fromString("lightgray");
            default:
                if (errorMsg) *errorMsg = QString("Invalid color index: %1").arg(colorIndex);
                return QColor(); // Invalid
        }
    }
    
    // Try cgraph-style color name aliases that Qt might not recognize
    static QMap<QString, QString> cgraphAliases = {
        {"grey", "gray"},
        {"dark_green", "darkgreen"},
        {"light_blue", "lightblue"},
        {"light_cyan", "lightcyan"},
        {"deep_pink", "deeppink"},
        {"medium_purple", "mediumpurple"},
        {"light_gray", "lightgray"},
        {"light_grey", "lightgray"},
        {"bright_white", "white"}
    };
    
    auto it = cgraphAliases.find(lowerColor);
    if (it != cgraphAliases.end()) {
        color = QColor::fromString(it.value());
        if (color.isValid()) {
            return color;
        }
    }
    
    // Color not recognized
    if (errorMsg) {
        *errorMsg = QString("Unknown color: %1").arg(colorString);
    }
    return QColor(); // Invalid
}

// Helper to get cgraph color index from Qt color (for reverse lookup)
static int qtColorToCgraphIndex(const QColor& color)
{
    // Map common Qt colors back to cgraph indices
    static QMap<QRgb, int> colorToIndex = {
        {QColor("black").rgb(), 0},
        {QColor("blue").rgb(), 1},
        {QColor("darkgreen").rgb(), 2},
        {QColor("cyan").rgb(), 3},
        {QColor("red").rgb(), 4},
        {QColor("magenta").rgb(), 5},
        {QColor("brown").rgb(), 6},
        {QColor("white").rgb(), 7},
        {QColor("gray").rgb(), 8},
        {QColor("lightblue").rgb(), 9},
        {QColor("green").rgb(), 10},
        {QColor("lightcyan").rgb(), 11},
        {QColor("deeppink").rgb(), 12},
        {QColor("mediumpurple").rgb(), 13},
        {QColor("yellow").rgb(), 14},
        {QColor("navy").rgb(), 15},
        {QColor("lightgray").rgb(), 17}
    };
    
    auto it = colorToIndex.find(color.rgb());
    return (it != colorToIndex.end()) ? it.value() : -1; // -1 = no cgraph equivalent
}

// Now simplify the background color command using the utility
static int qtcgraph_setbgcolor_cmd(ClientData data, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "color_name_or_hex");
        return TCL_ERROR;
    }
    
    EssGraphicsWidget* widget = static_cast<EssGraphicsWidget*>(
        Tcl_GetAssocData(interp, "scriptable_widget", nullptr));
    
    if (!widget) {
        Tcl_SetResult(interp, "No widget associated with interpreter", TCL_STATIC);
        return TCL_ERROR;
    }
    
    QString colorString = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString errorMsg;
    QColor bgColor = parseColorString(colorString, &errorMsg);
    
    if (!bgColor.isValid()) {
        Tcl_SetResult(interp, errorMsg.toUtf8().data(), TCL_VOLATILE);
        return TCL_ERROR;
    }
    
    // Set the background color on the widget
    widget->setBackgroundColor(bgColor);
    
    // Return the color that was actually set (useful for debugging)
    Tcl_SetObjResult(interp, Tcl_NewStringObj(bgColor.name().toUtf8().constData(), -1));
    return TCL_OK;
}

// Get list of Qt's built-in color names
static int qtcgraph_colornames_cmd(ClientData data, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *const objv[])
{
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    // Get Qt's built-in color names
    QStringList colorNames = QColor::colorNames();
    
    Tcl_Obj* listObj = Tcl_NewListObj(0, nullptr);
    for (const QString& name : colorNames) {
        Tcl_ListObjAppendElement(interp, listObj, 
            Tcl_NewStringObj(name.toUtf8().constData(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}


// Create namespace and convenience commands
static void createConvenienceCommands(Tcl_Interp *interp)
{
    // Create the essqt::graphics namespace for enhanced commands
    Tcl_Eval(interp, "namespace eval ::essqt {}; namespace eval ::essqt::graphics {};");
    
    // Enhanced graphics commands in essqt::graphics namespace
    Tcl_Eval(interp,
        "proc ::essqt::graphics::init {} { "
        "    graphics_init "
        "}; "
        
        "proc ::essqt::graphics::clear {} { "
        "    graphics_clear "
        "}; "
        
        "proc ::essqt::graphics::export {{filename {}}} { "
        "    if {$filename eq {}} { "
        "        graphics_export "
        "    } else { "
        "        graphics_export $filename "
        "    } "
        "}; "
        
        "proc ::essqt::graphics::bind {event script} { "
        "    graphics_bind $event $script "
        "}; "
        
        "proc ::essqt::graphics::colorlist {} { "
        "    graphics_colorlist "
        "}; "
        
        "proc ::essqt::graphics::setcolor {color} { "
        "    graphics_setcolor $color "
        "}; "
        
        // Development helpers
        "proc ::essqt::graphics::demo {} { "
        "    local_log \"Running graphics demo...\" "
        "    graphics_clear "
        "    setcolor red "
        "    line 10 10 100 100 "
        "    setcolor blue "
        "    circle 50 50 20 1 "
        "    setcolor black "
        "    text 75 75 \"Demo\" "
        "    flushwin "
        "    local_log \"Demo complete\" "
        "}; "
        
        "proc ::essqt::graphics::test_events {} { "
        "    local_log \"Setting up event test bindings...\" "
        "    graphics_bind \"<ButtonPress>\" { "
        "        local_log \"Mouse pressed at %x, %y\" "
        "        setcolor red "
        "        circle %x %y 5 1 "
        "        flushwin "
        "    } "
        "    graphics_bind \"<Motion>\" { "
        "        # Uncomment for mouse tracking "
        "        # local_log \"Mouse at %x, %y\" "
        "    } "
        "    local_log \"Event bindings set up. Click to draw red circles!\" "
        "}; ");
}

// Extension initialization - updated for EssGraphicsWidget integration
extern "C" int Qtcgraph_Init(Tcl_Interp *interp)
{
    if (Tcl_InitStubs(interp, "9.0", 0) == nullptr) {
        return TCL_ERROR;
    }
    
    if (Tcl_PkgProvide(interp, "qtcgraph", "2.0") != TCL_OK) {
        return TCL_ERROR;
    }

    // Set up the cgraph callbacks (now for EssGraphicsWidget)
    EssGraphicsBridge::setupCallbacks();

    // Register bridge commands that EssGraphicsWidget needs
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
    
    // Enhanced commands that accept names and provide enhanced functionality
    Tcl_CreateObjCommand(interp, "qtcgraph_setcolor",
                        (Tcl_ObjCmdProc *) qtcgraph_setcolor_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_colorlist",
                        (Tcl_ObjCmdProc *) qtcgraph_colorlist_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_setbgcolor",
                        (Tcl_ObjCmdProc *) qtcgraph_setbgcolor_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_colornames",
                        (Tcl_ObjCmdProc *) qtcgraph_colornames_cmd,
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "qtcgraph_export",
                        (Tcl_ObjCmdProc *) qtcgraph_export_cmd,
                        (ClientData) NULL, NULL);    
    Tcl_CreateObjCommand(interp, "qtcgraph_bind",
                        (Tcl_ObjCmdProc *) qtcgraph_bind_cmd,
                        (ClientData) NULL, NULL);
    
    // Override flushwin command to work with EssGraphicsWidget
    Tcl_CreateObjCommand(interp, "flushwin",
                        (Tcl_ObjCmdProc *) cgFlushwinCmd,
                        (ClientData) NULL, NULL);

    // Create convenience commands and namespaces
    createConvenienceCommands(interp);

    return TCL_OK;
}

