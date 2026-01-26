/**
 * dgview - Standalone DG/DGZ File Viewer
 * 
 * A fast, efficient viewer for DYN_GROUP data files.
 * Supports drag-and-drop, file list management, and export to CSV/JSON.
 * 
 * Usage:
 *   dgview [files...]              Open files in GUI
 *   dgview --batch -o DIR files... Batch convert without GUI
 *   dgview --help                  Show help
 * 
 * Copyright (c) SheinbergLab
 */

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Tree.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Native_File_Chooser.H>

#include "CLI11.hpp"
#include "DgFile.h"
#include "DgTable.h"
#include "DgExport.h"

#include <vector>
#include <string>
#include <memory>
#include <map>
#include <algorithm>
#include <iostream>

// Forward declarations
class OpenFile;
class DgViewerApp;

// Global app pointer for callbacks
static DgViewerApp* g_app = nullptr;

//============================================================================
// Batch Conversion (CLI mode)
//============================================================================

static std::string replaceExtension(const std::string& filename, const std::string& newExt) {
    std::string basename = filename;
    size_t slash = basename.find_last_of("/\\");
    if (slash != std::string::npos) basename = basename.substr(slash + 1);
    size_t dot = basename.rfind('.');
    if (dot != std::string::npos) basename = basename.substr(0, dot);
    return basename + newExt;
}

static int runBatchConvert(const std::vector<std::string>& files,
                           const std::string& outDir,
                           const std::string& format,
                           bool prettyPrint,
                           bool verbose) {
    int success = 0, failed = 0;
    
    DgExport::Options opts;
    opts.prettyJson = prettyPrint;
    opts.delimiter = '\t';
    
    for (const auto& inFile : files) {
        std::string error;
        DYN_GROUP* dg = DgFile::load(inFile.c_str(), &error);
        
        if (!dg) {
            std::cerr << "Error: Failed to load " << inFile << ": " << error << "\n";
            failed++;
            continue;
        }
        
        // Build output filename
        std::string ext = (format == "csv") ? ".csv" : ".json";
        std::string outFile = outDir + "/" + replaceExtension(inFile, ext);
        
        if (format == "json") {
            error = DgExport::toJSON(dg, outFile.c_str(), opts);
        } else {
            error = DgExport::toCSV(dg, outFile.c_str(), opts);
        }
        
        dfuFreeDynGroup(dg);
        
        if (error.empty()) {
            if (verbose) {
                std::cout << inFile << " -> " << outFile << "\n";
            }
            success++;
        } else {
            std::cerr << "Error: Failed to export " << inFile << ": " << error << "\n";
            failed++;
        }
    }
    
    std::cout << "Converted " << success << " file" << (success != 1 ? "s" : "");
    if (failed > 0) {
        std::cout << " (" << failed << " failed)";
    }
    std::cout << "\n";
    
    return (failed > 0) ? 1 : 0;
}

//============================================================================
// Command Line Parsing
//============================================================================

struct CLIOptions {
    bool guiMode = true;
    bool batchMode = false;
    bool verbose = false;
    bool prettyPrint = true;
    std::string outputDir = ".";
    std::string format = "json";
    std::vector<std::string> inputFiles;
    int exitCode = 0;
};

static CLIOptions parseCommandLine(int argc, char** argv) {
    CLIOptions opts;
    
    CLI::App app{"dgview - DG/DGZ File Viewer and Converter"};
    app.set_version_flag("--version,-V", "dgview 0.1.0");
    
    // Positional args (files to open)
    app.add_option("files", opts.inputFiles, "Input files (.dg, .dgz, .lz4)");
    
    // Batch conversion options
    app.add_flag("--batch,-b", opts.batchMode, "Batch convert mode (no GUI)");
    app.add_option("--outdir,-o", opts.outputDir, "Output directory for batch conversion")
        ->check(CLI::ExistingDirectory);
    app.add_option("--format,-f", opts.format, "Output format: json, csv (default: json)")
        ->check(CLI::IsMember({"json", "csv"}));
    app.add_flag("--verbose,-v", opts.verbose, "Verbose output (show each file converted)");
    app.add_flag("--compact,-c", [&opts](int64_t) { opts.prettyPrint = false; }, 
                 "Compact JSON output (no pretty printing)");
    
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        opts.exitCode = app.exit(e);
        opts.guiMode = false;
        return opts;
    }
    
    // Handle batch mode
    if (opts.batchMode) {
        opts.guiMode = false;
        
        if (opts.inputFiles.empty()) {
            std::cerr << "Error: No input files specified for batch conversion\n";
            std::cerr << "Usage: dgview --batch -o OUTDIR files...\n";
            opts.exitCode = 1;
            return opts;
        }
        
        opts.exitCode = runBatchConvert(opts.inputFiles, opts.outputDir, 
                                         opts.format, opts.prettyPrint, opts.verbose);
    }
    
    return opts;
}

//============================================================================
// OpenFile - Represents a single open DG file
//============================================================================

class OpenFile {
public:
    OpenFile(const std::string& path) : m_path(path), m_dg(nullptr) {
        // Extract basename for display
        const char* p = path.c_str();
        const char* slash = strrchr(p, '/');
        if (slash) p = slash + 1;
#ifdef WIN32
        const char* backslash = strrchr(p, '\\');
        if (backslash && backslash > p) p = backslash + 1;
#endif
        m_basename = p;
    }
    
    ~OpenFile() {
        if (m_dg) {
            dfuFreeDynGroup(m_dg);
            m_dg = nullptr;
        }
    }
    
    bool load() {
        if (m_dg) return true;  // Already loaded
        
        std::string error;
        m_dg = DgFile::load(m_path.c_str(), &error);
        if (!m_dg) {
            m_error = error;
            return false;
        }
        return true;
    }
    
    const std::string& path() const { return m_path; }
    const std::string& basename() const { return m_basename; }
    const std::string& error() const { return m_error; }
    DYN_GROUP* data() const { return m_dg; }
    
    std::string displayName() const {
        if (!m_dg) return m_basename + " (not loaded)";
        char buf[256];
        snprintf(buf, sizeof(buf), "%s (%dx%d)", 
                 m_basename.c_str(), 
                 DgFile::getMaxRows(m_dg), 
                 DgFile::getListCount(m_dg));
        return buf;
    }
    
private:
    std::string m_path;
    std::string m_basename;
    std::string m_error;
    DYN_GROUP* m_dg;
};

//============================================================================
// Scroll-contained widgets - prevent mousewheel fall-through
//============================================================================

class ContainedBrowser : public Fl_Hold_Browser {
public:
    ContainedBrowser(int X, int Y, int W, int H) : Fl_Hold_Browser(X, Y, W, H) {}
    
    int handle(int event) override {
        if (event == FL_MOUSEWHEEL && Fl::event_inside(this)) {
            // Always consume mousewheel events, even if we can't scroll
            Fl_Hold_Browser::handle(event);
            return 1;
        }
        return Fl_Hold_Browser::handle(event);
    }
};

class ContainedTree : public Fl_Tree {
public:
    ContainedTree(int X, int Y, int W, int H) : Fl_Tree(X, Y, W, H) {}
    
    int handle(int event) override {
        if (event == FL_MOUSEWHEEL && Fl::event_inside(this)) {
            // Always consume mousewheel events, even if we can't scroll
            Fl_Tree::handle(event);
            return 1;
        }
        return Fl_Tree::handle(event);
    }
};

//============================================================================
// FileListPanel - Left panel showing open files
//============================================================================

class FileListPanel : public Fl_Group {
public:
    using SelectionCallback = std::function<void(int index)>;
    using CloseCallback = std::function<void(int index)>;
    
    FileListPanel(int X, int Y, int W, int H)
        : Fl_Group(X, Y, W, H)
    {
        box(FL_FLAT_BOX);
        
        // Title
        Fl_Box* title = new Fl_Box(X, Y, W, 20, "Open Files");
        title->box(FL_FLAT_BOX);
        title->labelfont(FL_HELVETICA_BOLD);
        title->labelsize(12);
        title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        
        // File browser
        m_browser = new ContainedBrowser(X, Y + 20, W, H - 50);
        m_browser->textsize(12);
        m_browser->callback(browserCallback, this);
        m_browser->when(FL_WHEN_CHANGED);
        
        // Close button
        m_closeBtn = new Fl_Button(X + 2, Y + H - 28, W/2 - 4, 24, "Close");
        m_closeBtn->labelsize(11);
        m_closeBtn->callback(closeCallback, this);
        
        // Close All button  
        m_closeAllBtn = new Fl_Button(X + W/2 + 2, Y + H - 28, W/2 - 4, 24, "Close All");
        m_closeAllBtn->labelsize(11);
        m_closeAllBtn->callback(closeAllCallback, this);
        
        end();
        resizable(m_browser);
    }
    
    void addFile(const std::string& displayName) {
        m_browser->add(displayName.c_str());
        m_browser->select(m_browser->size());  // Select the new item
        m_browser->redraw();
    }
    
    void removeFile(int index) {
        if (index >= 1 && index <= m_browser->size()) {
            m_browser->remove(index);
            // Select next or previous
            if (m_browser->size() > 0) {
                int newSel = std::min(index, m_browser->size());
                m_browser->select(newSel);
            }
            m_browser->redraw();
        }
    }
    
    void updateFile(int index, const std::string& displayName) {
        if (index >= 1 && index <= m_browser->size()) {
            m_browser->text(index, displayName.c_str());
            m_browser->redraw();
        }
    }
    
    void clear() {
        m_browser->clear();
        m_browser->redraw();
    }
    
    int selectedIndex() const {
        return m_browser->value();  // 1-based, 0 if none
    }
    
    void select(int index) {
        m_browser->select(index);
        m_browser->redraw();
    }
    
    int count() const {
        return m_browser->size();
    }
    
    void setSelectionCallback(SelectionCallback cb) { m_selectCb = cb; }
    void setCloseCallback(CloseCallback cb) { m_closeCb = cb; }
    
private:
    static void browserCallback(Fl_Widget* w, void* data) {
        FileListPanel* panel = static_cast<FileListPanel*>(data);
        if (panel->m_selectCb) {
            panel->m_selectCb(panel->m_browser->value());
        }
    }
    
    static void closeCallback(Fl_Widget* w, void* data) {
        FileListPanel* panel = static_cast<FileListPanel*>(data);
        int sel = panel->m_browser->value();
        if (sel > 0 && panel->m_closeCb) {
            panel->m_closeCb(sel);
        }
    }
    
    static void closeAllCallback(Fl_Widget* w, void* data) {
        FileListPanel* panel = static_cast<FileListPanel*>(data);
        if (panel->m_closeCb) {
            // Close all by calling close callback with -1
            panel->m_closeCb(-1);
        }
    }
    
    ContainedBrowser* m_browser;
    Fl_Button* m_closeBtn;
    Fl_Button* m_closeAllBtn;
    SelectionCallback m_selectCb;
    CloseCallback m_closeCb;
};

//============================================================================
// ContentPanel - Main content area with table and detail panel
//============================================================================

class ContentPanel : public Fl_Group {
public:
    ContentPanel(int X, int Y, int W, int H)
        : Fl_Group(X, Y, W, H)
        , m_currentFile(nullptr)
        , m_detailVisible(true)
        , m_savedDetailWidth(0)
        , m_currentDetailRow(-1)
    {
        // Header bar showing active file
        int headerH = 24;
        m_header = new Fl_Box(X, Y, W, headerH);
        m_header->box(FL_FLAT_BOX);
        m_header->color(fl_rgb_color(225, 225, 225));
        m_header->labelfont(FL_HELVETICA_BOLD);
        m_header->labelsize(12);
        m_header->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        m_header->label("  No file selected");
        
        // Create tile for resizable split below header
        m_tile = new Fl_Tile(X, Y + headerH, W, H - headerH);
        
        // Table (75% width)
        int detailW = W / 4;
        int tableW = W - detailW;
        m_table = new DgTable(X, Y + headerH, tableW, H - headerH);
        
        // Detail panel on right
        m_detailTree = new ContainedTree(X + tableW, Y + headerH, detailW, H - headerH);
        m_detailTree->showroot(0);  // Hide the "ROOT" label
        m_detailTree->selectmode(FL_TREE_SELECT_SINGLE);
        m_detailTree->connectorstyle(FL_TREE_CONNECTOR_SOLID);
        m_detailTree->item_labelfont(FL_HELVETICA);
        m_detailTree->item_labelsize(12);
        m_detailTree->callback(detailTreeCallback, this);
        
        m_tile->end();
        
        end();
        resizable(m_tile);
        
        // Set up table callbacks
        m_table->setNestedListCallback([this](DYN_LIST* dl, const char* name) {
            showNestedListInDetail(dl, name);
        });
        m_table->callback(tableCallback, this);
        m_table->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
        
        // Show placeholder
        showPlaceholder();
    }
    
    void setFile(OpenFile* file) {
        m_currentFile = file;
        m_columnMap.clear();
        m_currentDetailRow = -1;
        
        if (!file || !file->data()) {
            m_table->clear();
            updateHeader();
            showPlaceholder();
            return;
        }
        
        updateHeader();
        m_table->setData(file->data());
        showFileOverview();
    }
    
    void clear() {
        m_currentFile = nullptr;
        m_table->clear();
        m_columnMap.clear();
        updateHeader();
        showPlaceholder();
    }
    
    DgTable* table() const { return m_table; }
    OpenFile* currentFile() const { return m_currentFile; }
    
    void updateHeader() {
        if (m_currentFile && m_currentFile->data()) {
            char buf[512];
            snprintf(buf, sizeof(buf), "  %s  ·  %d rows × %d columns",
                     m_currentFile->basename().c_str(),
                     DgFile::getMaxRows(m_currentFile->data()),
                     DgFile::getListCount(m_currentFile->data()));
            m_header->copy_label(buf);
        } else {
            m_header->label("  No file selected");
        }
        m_header->redraw();
    }
    
    void toggleDetailPanel() {
        if (m_detailVisible) {
            m_savedDetailWidth = m_detailTree->w();
            showDetailPanel(false);
        } else {
            showDetailPanel(true);
        }
    }
    
    void showDetailPanel(bool show) {
        if (show == m_detailVisible) return;
        
        int W = m_tile->w();
        int H = m_tile->h();
        int X = m_tile->x();
        int Y = m_tile->y();
        
        if (show) {
            int detailW = (m_savedDetailWidth > 50) ? m_savedDetailWidth : W / 4;
            detailW = std::min(detailW, W / 2);
            int tableW = W - detailW;
            m_table->resize(X, Y, tableW, H);
            m_detailTree->resize(X + tableW, Y, detailW, H);
            m_detailTree->show();
        } else {
            m_savedDetailWidth = m_detailTree->w();
            m_table->resize(X, Y, W, H);
            m_detailTree->resize(X + W, Y, 0, H);
            m_detailTree->hide();
        }
        m_detailVisible = show;
        m_tile->init_sizes();
        m_tile->redraw();
    }
    
    void showFileOverview() {
        if (!m_currentFile || !m_currentFile->data()) return;
        
        DYN_GROUP* dg = m_currentFile->data();
        m_detailTree->clear();
        m_columnMap.clear();
        m_currentDetailRow = -1;
        
        char rootLabel[256];
        snprintf(rootLabel, sizeof(rootLabel), "File: %s", m_currentFile->basename().c_str());
        Fl_Tree_Item* root = m_detailTree->add(rootLabel);
        
        char path[512];
        snprintf(path, sizeof(path), "%s/Rows: %d", rootLabel, DgFile::getMaxRows(dg));
        m_detailTree->add(path);
        snprintf(path, sizeof(path), "%s/Columns: %d", rootLabel, DgFile::getListCount(dg));
        m_detailTree->add(path);
        
        snprintf(path, sizeof(path), "%s/Columns", rootLabel);
        Fl_Tree_Item* colsItem = m_detailTree->add(path);
        
        for (int i = 0; i < DYN_GROUP_N(dg); i++) {
            DYN_LIST* dl = DYN_GROUP_LIST(dg, i);
            const char* typeStr = getTypeString(DYN_LIST_DATATYPE(dl));
            char colPath[512];
            snprintf(colPath, sizeof(colPath), "%s/Columns/%s (%s, %d)", 
                     rootLabel, DYN_LIST_NAME(dl), typeStr, DYN_LIST_N(dl));
            Fl_Tree_Item* item = m_detailTree->add(colPath);
            if (item) {
                m_columnMap[item] = i;
            }
        }
        
        if (root) root->open();
        if (colsItem) colsItem->open();
        m_detailTree->redraw();
        
        if (!m_detailVisible) showDetailPanel(true);
    }
    
    void showRowDetail(int row) {
        if (!m_currentFile || !m_currentFile->data() || row < 0) {
            showFileOverview();
            return;
        }
        
        DYN_GROUP* dg = m_currentFile->data();
        m_detailTree->clear();
        m_columnMap.clear();
        m_currentDetailRow = row;
        
        char rootLabel[64];
        snprintf(rootLabel, sizeof(rootLabel), "Row %d", row);
        Fl_Tree_Item* root = m_detailTree->add(rootLabel);
        
        for (int c = 0; c < DYN_GROUP_N(dg); c++) {
            DYN_LIST* dl = DYN_GROUP_LIST(dg, c);
            const char* colName = DYN_LIST_NAME(dl);
            
            Fl_Tree_Item* item = nullptr;
            
            if (row >= DYN_LIST_N(dl)) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s: (empty)", rootLabel, colName);
                item = m_detailTree->add(path);
            } else if (DYN_LIST_DATATYPE(dl) == DF_LIST) {
                DYN_LIST** vals = (DYN_LIST**)DYN_LIST_VALS(dl);
                DYN_LIST* nested = vals[row];
                item = addNestedListToTree(rootLabel, colName, nested);
            } else {
                std::string val = m_table->cellValue(row, c);
                char path[512];
                snprintf(path, sizeof(path), "%s/%s: %s", rootLabel, colName, val.c_str());
                item = m_detailTree->add(path);
            }
            
            if (item) {
                m_columnMap[item] = c;
            }
        }
        
        if (root) root->open();
        m_detailTree->redraw();
        
        if (!m_detailVisible) showDetailPanel(true);
    }
    
private:
    void showPlaceholder() {
        m_detailTree->clear();
        m_detailTree->add("No file selected");
        m_detailTree->add("No file selected/Drop files or use File > Open");
        m_detailTree->redraw();
    }
    
    Fl_Tree_Item* addNestedListToTree(const char* rootLabel, const char* name, DYN_LIST* dl) {
        if (!dl) return nullptr;
        
        const char* typeStr = getTypeString(DYN_LIST_DATATYPE(dl));
        int n = DYN_LIST_N(dl);
        
        char parentPath[512];
        snprintf(parentPath, sizeof(parentPath), "%s/%s (%s, %d items)", 
                 rootLabel, name, typeStr, n);
        Fl_Tree_Item* parent = m_detailTree->add(parentPath);
        
        int showMax = 100;
        char buf[256];
        
        for (int i = 0; i < std::min(n, showMax); i++) {
            formatNestedValue(buf, sizeof(buf), dl, i);
            char path[768];
            snprintf(path, sizeof(path), "%s/[%d] %s", parentPath, i, buf);
            m_detailTree->add(path);
        }
        
        if (n > showMax) {
            char path[512];
            snprintf(path, sizeof(path), "%s/... (%d more)", parentPath, n - showMax);
            m_detailTree->add(path);
        }
        
        if (parent) parent->close();
        return parent;
    }
    
    void formatNestedValue(char* buf, size_t bufsize, DYN_LIST* dl, int row) {
        buf[0] = '\0';
        if (!dl || row >= DYN_LIST_N(dl)) return;
        
        switch (DYN_LIST_DATATYPE(dl)) {
            case DF_LONG: {
                int* vals = (int*)DYN_LIST_VALS(dl);
                snprintf(buf, bufsize, "%d", vals[row]);
                break;
            }
            case DF_SHORT: {
                short* vals = (short*)DYN_LIST_VALS(dl);
                snprintf(buf, bufsize, "%d", vals[row]);
                break;
            }
            case DF_FLOAT: {
                float* vals = (float*)DYN_LIST_VALS(dl);
                snprintf(buf, bufsize, "%.6g", vals[row]);
                break;
            }
            case DF_STRING: {
                char** vals = (char**)DYN_LIST_VALS(dl);
                snprintf(buf, bufsize, "%s", vals[row] ? vals[row] : "");
                break;
            }
            case DF_LIST: {
                DYN_LIST** vals = (DYN_LIST**)DYN_LIST_VALS(dl);
                snprintf(buf, bufsize, "[nested, %d items]", DYN_LIST_N(vals[row]));
                break;
            }
            default:
                snprintf(buf, bufsize, "?");
                break;
        }
    }
    
    const char* getTypeString(int dtype) {
        switch (dtype) {
            case DF_LONG:   return "int";
            case DF_SHORT:  return "short";
            case DF_FLOAT:  return "float";
            case DF_CHAR:   return "char";
            case DF_STRING: return "string";
            case DF_LIST:   return "list";
            default:        return "?";
        }
    }
    
    void showNestedListInDetail(DYN_LIST* dl, const char* name) {
        if (!dl) return;
        
        m_detailTree->clear();
        m_columnMap.clear();
        addNestedListToTree("", name, dl);
        
        Fl_Tree_Item* root = m_detailTree->first();
        if (root) root->open();
        
        m_detailTree->redraw();
        if (!m_detailVisible) showDetailPanel(true);
    }
    
    static void detailTreeCallback(Fl_Widget* w, void* data) {
        ContentPanel* panel = static_cast<ContentPanel*>(data);
        Fl_Tree* tree = static_cast<Fl_Tree*>(w);
        
        Fl_Tree_Item* item = tree->callback_item();
        if (!item) return;
        
        if (tree->callback_reason() != FL_TREE_REASON_SELECTED) return;
        
        auto it = panel->m_columnMap.find(item);
        if (it != panel->m_columnMap.end()) {
            int col = it->second;
            panel->m_table->col_position(col);
            panel->m_table->redraw();
        }
    }
    
    static void tableCallback(Fl_Widget* w, void* data) {
        ContentPanel* panel = static_cast<ContentPanel*>(data);
        DgTable* table = static_cast<DgTable*>(w);
        
        int top, left, bot, right;
        table->get_selection(top, left, bot, right);
        
        if (top >= 0 && top == bot) {
            // Single row selected
            panel->showRowDetail(top);
        } else {
            // No selection or multiple rows - show overview
            panel->showFileOverview();
        }
    }
    
    Fl_Box* m_header;
    Fl_Tile* m_tile;
    DgTable* m_table;
    ContainedTree* m_detailTree;
    OpenFile* m_currentFile;
    bool m_detailVisible;
    int m_savedDetailWidth;
    int m_currentDetailRow;
    std::map<Fl_Tree_Item*, int> m_columnMap;
};

//============================================================================
// DgViewerApp - Main application window
//============================================================================

class DgViewerApp : public Fl_Double_Window {
public:
    DgViewerApp(int W, int H, const char* title)
        : Fl_Double_Window(W, H, title)
    {
        // Menu bar
        m_menubar = new Fl_Menu_Bar(0, 0, W, 25);
        setupMenus();
        
        // Main tile for resizable left/right split
        m_mainTile = new Fl_Tile(0, 25, W, H - 50);
        
        // File list panel on left
        int listW = 180;
        m_fileList = new FileListPanel(0, 25, listW, H - 50);
        m_fileList->setSelectionCallback([this](int index) {
            onFileSelected(index);
        });
        m_fileList->setCloseCallback([this](int index) {
            if (index == -1) {
                closeAllFiles();
            } else {
                closeFile(index);
            }
        });
        
        // Content panel (table + detail)
        m_content = new ContentPanel(listW, 25, W - listW, H - 50);
        
        m_mainTile->end();
        
        // Status bar
        m_status = new Fl_Box(0, H - 25, W, 25);
        m_status->box(FL_THIN_UP_BOX);
        m_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        m_status->label("Drop DG/DGZ files here or use File > Open");
        
        end();
        resizable(m_mainTile);
        
        updateStatus();
    }
    
    ~DgViewerApp() {
        // Clean up open files
        for (auto* file : m_files) {
            delete file;
        }
        m_files.clear();
    }
    
    void openFile(const char* filename) {
        // Check if already open
        for (size_t i = 0; i < m_files.size(); i++) {
            if (m_files[i]->path() == filename) {
                m_fileList->select(i + 1);  // 1-based
                selectFile(i);
                return;
            }
        }
        
        // Create new file entry
        OpenFile* file = new OpenFile(filename);
        if (!file->load()) {
            fl_alert("Failed to load %s:\n%s", filename, file->error().c_str());
            delete file;
            return;
        }
        
        m_files.push_back(file);
        m_fileList->addFile(file->displayName());
        selectFile(m_files.size() - 1);
        
        updateStatus();
        
        // Force redraw of entire window
        Fl::flush();
        redraw();
    }
    
    void closeFile(int index) {
        // Convert from 1-based browser index to 0-based vector index
        int vecIndex = index - 1;
        if (vecIndex < 0 || vecIndex >= (int)m_files.size()) return;
        
        delete m_files[vecIndex];
        m_files.erase(m_files.begin() + vecIndex);
        m_fileList->removeFile(index);
        
        // Select another file or clear
        if (m_files.empty()) {
            m_content->clear();
        } else {
            int newSel = m_fileList->selectedIndex();
            if (newSel > 0) {
                selectFile(newSel - 1);
            }
        }
        
        updateStatus();
        redraw();
    }
    
    void closeAllFiles() {
        for (auto* file : m_files) {
            delete file;
        }
        m_files.clear();
        m_fileList->clear();
        m_content->clear();
        updateStatus();
        redraw();
    }
    
    void exportCSV() {
        OpenFile* file = m_content->currentFile();
        if (!file || !file->data()) {
            fl_alert("No data to export");
            return;
        }
        
        Fl_Native_File_Chooser chooser;
        chooser.title("Export to CSV");
        chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
        chooser.filter("CSV Files\t*.csv\nAll Files\t*");
        
        std::string defaultName = file->basename();
        size_t dot = defaultName.rfind('.');
        if (dot != std::string::npos) {
            defaultName = defaultName.substr(0, dot);
        }
        defaultName += ".csv";
        chooser.preset_file(defaultName.c_str());
        
        if (chooser.show() == 0) {
            DgExport::Options opts;
            opts.delimiter = '\t';
            std::string error = DgExport::toCSV(file->data(), chooser.filename(), opts);
            if (!error.empty()) {
                fl_alert("Export failed: %s", error.c_str());
            } else {
                setStatus("Exported to %s", chooser.filename());
            }
        }
    }
    
    void exportJSON() {
        OpenFile* file = m_content->currentFile();
        if (!file || !file->data()) {
            fl_alert("No data to export");
            return;
        }
        
        Fl_Native_File_Chooser chooser;
        chooser.title("Export to JSON");
        chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
        chooser.filter("JSON Files\t*.json\nAll Files\t*");
        
        std::string defaultName = file->basename();
        size_t dot = defaultName.rfind('.');
        if (dot != std::string::npos) {
            defaultName = defaultName.substr(0, dot);
        }
        defaultName += ".json";
        chooser.preset_file(defaultName.c_str());
        
        if (chooser.show() == 0) {
            DgExport::Options opts;
            std::string error = DgExport::toJSON(file->data(), chooser.filename(), opts);
            if (!error.empty()) {
                fl_alert("Export failed: %s", error.c_str());
            } else {
                setStatus("Exported to %s", chooser.filename());
            }
        }
    }
    
    void copyToClipboard() {
        if (m_content->table()) {
            m_content->table()->copySelection();
            setStatus("Copied selection to clipboard");
        }
    }
    
    void toggleDetailPanel() {
        m_content->toggleDetailPanel();
    }
    
    void showFileOverview() {
        m_content->showFileOverview();
    }
    
protected:
    int handle(int event) override {
        switch (event) {
            case FL_DND_ENTER:
            case FL_DND_DRAG:
            case FL_DND_LEAVE:
            case FL_DND_RELEASE:
                return 1;
                
            case FL_PASTE: {
                const char* text = Fl::event_text();
                if (text && text[0]) {
                    std::string paths(text);
                    size_t pos = 0;
                    while (pos < paths.length()) {
                        size_t end = paths.find('\n', pos);
                        if (end == std::string::npos) end = paths.length();
                        
                        std::string path = paths.substr(pos, end - pos);
                        
                        if (path.compare(0, 7, "file://") == 0) {
                            path = path.substr(7);
                        }
                        
                        while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) {
                            path.pop_back();
                        }
                        
                        std::string decoded;
                        for (size_t i = 0; i < path.length(); i++) {
                            if (path[i] == '%' && i + 2 < path.length()) {
                                int hex;
                                if (sscanf(path.c_str() + i + 1, "%2x", &hex) == 1) {
                                    decoded += (char)hex;
                                    i += 2;
                                    continue;
                                }
                            }
                            decoded += path[i];
                        }
                        
                        if (!decoded.empty()) {
                            openFile(decoded.c_str());
                        }
                        
                        pos = end + 1;
                    }
                }
                return 1;
            }
            
            case FL_SHORTCUT:
                if (Fl::event_state() & FL_COMMAND) {
                    switch (Fl::event_key()) {
                        case 'o': showOpenDialog(); return 1;
                        case 'w': closeCurrentFile(); return 1;
                        case 'c': copyToClipboard(); return 1;
                        case 'd': toggleDetailPanel(); return 1;
                        case 'q': hide(); return 1;
                    }
                }
                break;
        }
        
        return Fl_Double_Window::handle(event);
    }
    
private:
    void setupMenus() {
        m_menubar->add("&File/&Open...\tCmd+O", FL_COMMAND + 'o', menuOpenCb, this);
        m_menubar->add("&File/&Close\tCmd+W", FL_COMMAND + 'w', menuCloseCb, this);
        m_menubar->add("&File/Close All", 0, menuCloseAllCb, this);
        m_menubar->add("&File/Export CSV...", 0, menuExportCSVCb, this);
        m_menubar->add("&File/Export JSON...", 0, menuExportJSONCb, this);
        m_menubar->add("&File/&Quit\tCmd+Q", FL_COMMAND + 'q', menuQuitCb, this);
        
        m_menubar->add("&Edit/&Copy\tCmd+C", FL_COMMAND + 'c', menuCopyCb, this);
        
        m_menubar->add("&View/Toggle Detail Panel\tCmd+D", FL_COMMAND + 'd', menuToggleDetailCb, this);
        m_menubar->add("&View/Show File Overview", 0, menuShowOverviewCb, this);
        
        m_menubar->add("&Help/About dgview", 0, menuAboutCb, this);
    }
    
    void showOpenDialog() {
        Fl_Native_File_Chooser chooser;
        chooser.title("Open DG/DGZ File");
        chooser.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
        chooser.filter("DG Files\t*.{dg,dgz,lz4}\nAll Files\t*");
        
        if (chooser.show() == 0) {
            for (int i = 0; i < chooser.count(); i++) {
                openFile(chooser.filename(i));
            }
        }
    }
    
    void closeCurrentFile() {
        int sel = m_fileList->selectedIndex();
        if (sel > 0) {
            closeFile(sel);
        }
    }
    
    void onFileSelected(int index) {
        if (index > 0 && index <= (int)m_files.size()) {
            selectFile(index - 1);
        }
    }
    
    void selectFile(int vecIndex) {
        if (vecIndex >= 0 && vecIndex < (int)m_files.size()) {
            m_content->setFile(m_files[vecIndex]);
            updateStatus();
        }
    }
    
    void updateStatus() {
        OpenFile* file = m_content->currentFile();
        if (file && file->data()) {
            setStatus("%s: %d columns, %d rows",
                     file->path().c_str(),
                     DgFile::getListCount(file->data()),
                     DgFile::getMaxRows(file->data()));
        } else if (m_files.empty()) {
            m_status->label("Drop DG/DGZ files here or use File > Open");
        } else {
            m_status->label("Select a file from the list");
        }
    }
    
    void setStatus(const char* fmt, ...) {
        static char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        m_status->copy_label(buf);
    }
    
    // Menu callbacks
    static void menuOpenCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->showOpenDialog();
    }
    static void menuCloseCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->closeCurrentFile();
    }
    static void menuCloseAllCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->closeAllFiles();
    }
    static void menuExportCSVCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->exportCSV();
    }
    static void menuExportJSONCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->exportJSON();
    }
    static void menuQuitCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->hide();
    }
    static void menuCopyCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->copyToClipboard();
    }
    static void menuToggleDetailCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->toggleDetailPanel();
    }
    static void menuShowOverviewCb(Fl_Widget*, void* data) {
        ((DgViewerApp*)data)->showFileOverview();
    }
    static void menuAboutCb(Fl_Widget*, void*) {
        fl_message("dgview - DG/DGZ File Viewer\n\n"
                   "A fast viewer for DYN_GROUP data files.\n\n"
                   "SheinbergLab\n"
                   "https://github.com/SheinbergLab");
    }
    
    Fl_Menu_Bar* m_menubar;
    Fl_Tile* m_mainTile;
    FileListPanel* m_fileList;
    ContentPanel* m_content;
    Fl_Box* m_status;
    std::vector<OpenFile*> m_files;
};

//============================================================================
// macOS file open callback (for double-click, Open With, drag to dock icon)
//============================================================================

#ifdef __APPLE__
#include <FL/platform.H>

extern "C" {
    typedef void (*DropCallback)(const char* filename);
    void macSetDropCallback(DropCallback callback);
    void macEnableFileDrop(Fl_Window* window);
}

static void macOpenCallback(const char* filename) {
    if (g_app && filename) {
        g_app->openFile(filename);
    }
}

static void macDropCallback(const char* filename) {
    if (g_app && filename) {
        g_app->openFile(filename);
        // Force redraw after drop
        Fl::flush();
        g_app->redraw();
    }
}
#endif

//============================================================================
// Main
//============================================================================

static void initStyling() {
    // Use gtk+ for flatter look (no gradient shadows)
    Fl::scheme("gtk+");
    
    // Clean, neutral colors
    Fl::set_color(FL_BACKGROUND_COLOR, 240, 240, 240);
    Fl::set_color(FL_BACKGROUND2_COLOR, 255, 255, 255);
    Fl::set_color(FL_SELECTION_COLOR, 55, 120, 200);
    Fl::set_color(FL_INACTIVE_COLOR, 180, 180, 180);
    
    FL_NORMAL_SIZE = 13;
    
#ifdef __APPLE__
    Fl::set_font(FL_HELVETICA, "Helvetica Neue");
#endif
}

int main(int argc, char** argv) {
    // Parse command line first
    CLIOptions opts = parseCommandLine(argc, argv);
    
    // If CLI mode handled everything (batch, help, error), exit
    if (!opts.guiMode) {
        return opts.exitCode;
    }
    
    // GUI mode
    initStyling();
    
#ifdef __APPLE__
    fl_open_callback(macOpenCallback);
#endif
    
    DgViewerApp app(1000, 700, "dgview");
    g_app = &app;
    
    app.show();
    
#ifdef __APPLE__
    macSetDropCallback(macDropCallback);
    macEnableFileDrop(&app);
#endif
    
    // Open any files specified on command line
    for (const auto& file : opts.inputFiles) {
        app.openFile(file.c_str());
    }
    
    return Fl::run();
}
