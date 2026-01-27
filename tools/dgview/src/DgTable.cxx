/**
 * DgTable.cxx - Implementation of DgTable widget
 */

#include "DgTable.h"
#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <cmath>

// Platform-specific clipboard
#ifdef __APPLE__
#include <FL/Fl_Copy_Surface.H>
#endif

DgTable::DgTable(int X, int Y, int W, int H, const char* L)
    : Fl_Table_Row(X, Y, W, H, L)
    , m_dg(nullptr)
    , m_lastClickRow(-1)
    , m_lastClickCol(-1)
    , m_font(FL_HELVETICA)
    , m_fontSize(13)
    , m_headerBg(FL_BACKGROUND_COLOR)
    , m_headerFg(FL_BLACK)
    , m_cellBg(FL_WHITE)
    , m_cellFg(FL_BLACK)
    , m_nestedBg(fl_rgb_color(240, 248, 255))  // Light blue for nested
    , m_selectedBg(FL_SELECTION_COLOR)
    , m_currentRow(-1)
{
    // Default table configuration
    col_header(1);
    col_resize(1);
    col_header_height(25);
    row_header(1);
    row_header_width(60);
    row_resize(0);
    row_height_all(22);
    
    type(SELECT_SINGLE);
    
    when(FL_WHEN_RELEASE | FL_WHEN_CHANGED);
    
    end();
}

DgTable::~DgTable() {
    // We don't own m_dg, so don't free it
}

void DgTable::setData(DYN_GROUP* dg) {
    m_dg = dg;
    m_colWidths.clear();
    m_currentRow = -1;
    
    if (!dg) {
        rows(0);
        cols(0);
        redraw();
        return;
    }
    
    // Calculate dimensions
    int numCols = DYN_GROUP_N(dg);
    int maxRows = 0;
    for (int i = 0; i < numCols; i++) {
        DYN_LIST* dl = DYN_GROUP_LIST(dg, i);
        if (DYN_LIST_N(dl) > maxRows) {
            maxRows = DYN_LIST_N(dl);
        }
    }
    
    cols(numCols);
    rows(maxRows);
    
    autoSizeColumns();
    redraw();
}

void DgTable::clear() {
    m_dg = nullptr;
    m_colWidths.clear();
    m_currentRow = -1;
    rows(0);
    cols(0);
    redraw();
}

std::string DgTable::cellValue(int row, int col) const {
    char buf[256];
    formatCellValue(buf, sizeof(buf), row, col);
    return std::string(buf);
}

bool DgTable::isNestedList(int row, int col) const {
    if (!m_dg || col >= DYN_GROUP_N(m_dg)) return false;
    DYN_LIST* dl = DYN_GROUP_LIST(m_dg, col);
    if (row >= DYN_LIST_N(dl)) return false;
    return DYN_LIST_DATATYPE(dl) == DF_LIST;
}

DYN_LIST* DgTable::getNestedList(int row, int col) const {
    if (!isNestedList(row, col)) return nullptr;
    DYN_LIST* dl = DYN_GROUP_LIST(m_dg, col);
    DYN_LIST** vals = (DYN_LIST**)DYN_LIST_VALS(dl);
    return vals[row];
}

const char* DgTable::columnName(int col) const {
    if (!m_dg || col >= DYN_GROUP_N(m_dg)) return "";
    return DYN_LIST_NAME(DYN_GROUP_LIST(m_dg, col));
}

int DgTable::columnDataType(int col) const {
    if (!m_dg || col >= DYN_GROUP_N(m_dg)) return -1;
    return DYN_LIST_DATATYPE(DYN_GROUP_LIST(m_dg, col));
}

void DgTable::formatCellValue(char* buf, size_t bufsize, int row, int col) const {
    buf[0] = '\0';
    
    if (!m_dg || col >= DYN_GROUP_N(m_dg)) return;
    
    DYN_LIST* dl = DYN_GROUP_LIST(m_dg, col);
    if (row >= DYN_LIST_N(dl)) return;
    
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
            // Smart formatting: use fewer decimals for whole numbers
            float v = vals[row];
            if (v == (int)v) {
                snprintf(buf, bufsize, "%.1f", v);
            } else if (fabs(v) < 0.001 || fabs(v) >= 10000) {
                snprintf(buf, bufsize, "%.3e", v);
            } else {
                snprintf(buf, bufsize, "%.4g", v);
            }
            break;
        }
        case DF_CHAR: {
            char* vals = (char*)DYN_LIST_VALS(dl);
            snprintf(buf, bufsize, "%d", (int)vals[row]);
            break;
        }
        case DF_STRING: {
            char** vals = (char**)DYN_LIST_VALS(dl);
            snprintf(buf, bufsize, "%s", vals[row] ? vals[row] : "");
            break;
        }
        case DF_LIST: {
            DYN_LIST** vals = (DYN_LIST**)DYN_LIST_VALS(dl);
            DYN_LIST* nested = vals[row];
            const char* typeStr;
            switch (DYN_LIST_DATATYPE(nested)) {
                case DF_LONG:   typeStr = "int";    break;
                case DF_SHORT:  typeStr = "short";  break;
                case DF_FLOAT:  typeStr = "float";  break;
                case DF_CHAR:   typeStr = "char";   break;
                case DF_STRING: typeStr = "string"; break;
                case DF_LIST:   typeStr = "list";   break;
                default:        typeStr = "?";      break;
            }
            snprintf(buf, bufsize, "[%s × %d]", typeStr, DYN_LIST_N(nested));
            break;
        }
    }
}

void DgTable::draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H) {
    static char buf[256];
    
    switch (context) {
        case CONTEXT_STARTPAGE:
            fl_font(m_font, m_fontSize);
            return;
            
        case CONTEXT_COL_HEADER:
            if (m_dg && C < DYN_GROUP_N(m_dg)) {
                snprintf(buf, sizeof(buf), "%s", DYN_LIST_NAME(DYN_GROUP_LIST(m_dg, C)));
            } else {
                snprintf(buf, sizeof(buf), "C%d", C);
            }
            drawHeader(buf, X, Y, W, H, true);
            return;
            
        case CONTEXT_ROW_HEADER:
            snprintf(buf, sizeof(buf), "%d", R);
            drawHeader(buf, X, Y, W, H, false);
            return;
            
        case CONTEXT_CELL:
            formatCellValue(buf, sizeof(buf), R, C);
            drawData(buf, X, Y, W, H, row_selected(R), isNestedList(R, C));
            return;
            
        default:
            return;
    }
}

void DgTable::drawHeader(const char* s, int X, int Y, int W, int H, bool isColHeader) {
    fl_push_clip(X, Y, W, H);
    
    // Draw background
    fl_draw_box(FL_THIN_UP_BOX, X, Y, W, H, m_headerBg);
    
    // Draw text
    fl_color(m_headerFg);
    fl_font(m_font, m_fontSize);
    fl_draw(s, X + 4, Y, W - 8, H, isColHeader ? FL_ALIGN_CENTER : FL_ALIGN_RIGHT);
    
    fl_pop_clip();
}

void DgTable::drawData(const char* s, int X, int Y, int W, int H, bool selected, bool isNested) {
    fl_push_clip(X, Y, W, H);
    
    // Background
    Fl_Color bg = selected ? m_selectedBg : (isNested ? m_nestedBg : m_cellBg);
    fl_color(bg);
    fl_rectf(X, Y, W, H);
    
    // Text
    fl_color(selected ? FL_WHITE : m_cellFg);
    fl_font(m_font, m_fontSize);
    
    // Different alignment for nested lists (center) vs regular data (left)
    Fl_Align align = isNested ? FL_ALIGN_CENTER : FL_ALIGN_LEFT;
    fl_draw(s, X + 4, Y, W - 8, H, align | FL_ALIGN_CLIP);
    
    // Border
    fl_color(FL_LIGHT2);
    fl_rect(X, Y, W, H);
    
    fl_pop_clip();
}

void DgTable::autoSizeColumns() {
    if (!m_dg) return;
    
    fl_font(m_font, m_fontSize);
    
    int numCols = DYN_GROUP_N(m_dg);
    m_colWidths.resize(numCols);
    
    for (int c = 0; c < numCols; c++) {
        DYN_LIST* dl = DYN_GROUP_LIST(m_dg, c);
        
        // Start with header width
        int maxW = fl_width(DYN_LIST_NAME(dl)) + 16;
        
        // Sample some rows to estimate width (don't check all for huge files)
        int numRows = DYN_LIST_N(dl);
        int step = std::max(1, numRows / 100);  // Sample ~100 rows max
        
        char buf[256];
        for (int r = 0; r < numRows; r += step) {
            formatCellValue(buf, sizeof(buf), r, c);
            int w = fl_width(buf) + 12;
            if (w > maxW) maxW = w;
        }
        
        // Clamp to reasonable range
        maxW = std::max(60, std::min(maxW, 300));
        m_colWidths[c] = maxW;
        col_width(c, maxW);
    }
}

int DgTable::handle(int event) {
    switch (event) {
        case FL_FOCUS:
            return 1;
            
        case FL_UNFOCUS:
            return 1;
            
        case FL_KEYDOWN: {
            int key = Fl::event_key();
            
            // Cmd+C for copy
            if (key == 'c' && (Fl::event_state() & FL_COMMAND)) {
                copySelection();
                return 1;
            }
            
            // Arrow key navigation
            if (key == FL_Up || key == FL_Down || key == FL_Left || key == FL_Right) {
                return handleArrowKey(key);
            }
            
            // Home/End for first/last row
            if (key == FL_Home) {
                if (rows() > 0) {
                    m_currentRow = 0;
                    select_row(0, 1);
                    row_position(0);
		    Fl_Widget::do_callback();
                }
                return 1;
            }
            if (key == FL_End) {
                if (rows() > 0) {
                    m_currentRow = rows() - 1;
                    select_row(m_currentRow, 1);
                    row_position(m_currentRow);
		    Fl_Widget::do_callback();
                }
                return 1;
            }
            
            // Page Up/Down
            if (key == FL_Page_Up || key == FL_Page_Down) {
                if (m_currentRow < 0) {
                    int top, left, bot, right;
                    get_selection(top, left, bot, right);
                    m_currentRow = (top >= 0) ? top : 0;
                }
                
                int visibleRows = (h() - col_header_height()) / row_height(0);
                int newRow;
                
                if (key == FL_Page_Up) {
                    newRow = std::max(0, m_currentRow - visibleRows);
                } else {
                    newRow = std::min(rows() - 1, m_currentRow + visibleRows);
                }
                
                if (newRow >= 0 && newRow < rows()) {
                    m_currentRow = newRow;
                    select_row(newRow, 1);
                    row_position(newRow);
		    Fl_Widget::do_callback();
                }
                return 1;
            }
            break;
        }
            
        case FL_MOUSEWHEEL:
            if (rows() > 0) {
                Fl_Table_Row::handle(event);
            }
            return 1;
            
        case FL_PUSH:
            take_focus();
            m_currentRow = -1;  // Reset so we read from mouse selection
            if (Fl::event_button() == FL_RIGHT_MOUSE) {
                int R, C;
                ResizeFlag resizeFlag;
                TableContext ctx = cursor2rowcol(R, C, resizeFlag);
                if (ctx == CONTEXT_CELL) {
                    showContextMenu(R, C);
                    return 1;
                }
            }
            break;
            
        case FL_RELEASE:
            if (Fl::event_clicks() > 0) {
                int R, C;
                ResizeFlag resizeFlag;
                TableContext ctx = cursor2rowcol(R, C, resizeFlag);
                if (ctx == CONTEXT_CELL && isNestedList(R, C) && m_nestedCb) {
                    DYN_LIST* nested = getNestedList(R, C);
                    if (nested) {
                        char name[128];
                        snprintf(name, sizeof(name), "%s[%d]", columnName(C), R);
                        m_nestedCb(nested, name);
                        return 1;
                    }
                }
            }
            break;
    }
    
    return Fl_Table_Row::handle(event);
}

int DgTable::handleArrowKey(int key) {
    if (rows() == 0 || cols() == 0) return 1;
    
    // If m_currentRow not set, get from selection (after mouse click)
    if (m_currentRow < 0) {
        int top, left, bot, right;
        get_selection(top, left, bot, right);
        m_currentRow = (top >= 0) ? top : 0;
    }
    
    int currentCol = 0;
    {
        int top, left, bot, right;
        get_selection(top, left, bot, right);
        currentCol = (left >= 0) ? left : 0;
    }
    
    int newRow = m_currentRow;
    int newCol = currentCol;
    
    switch (key) {
        case FL_Up:
            newRow = std::max(0, m_currentRow - 1);
            break;
        case FL_Down:
            newRow = std::min(rows() - 1, m_currentRow + 1);
            break;
        case FL_Left:
            newCol = std::max(0, currentCol - 1);
            break;
        case FL_Right:
            newCol = std::min(cols() - 1, currentCol + 1);
            break;
    }
    
    // For row selection mode, just move rows with Up/Down
    if (key == FL_Up || key == FL_Down) {
        if (newRow != m_currentRow) {
            m_currentRow = newRow;
            select_row(newRow, 1);
            
            // Scroll to make row visible
            int visTop = row_position();
            int visibleRows = (h() - col_header_height()) / row_height(0);
            if (newRow < visTop) {
                row_position(newRow);
            } else if (newRow >= visTop + visibleRows - 1) {
                row_position(newRow - visibleRows + 2);
            }
            
            redraw();
	    Fl_Widget::do_callback();
        }
    } else {
        // Left/Right scrolls columns into view
        if (newCol != currentCol) {
            col_position(newCol);
            redraw();
        }
    }
    
    return 1;
}

void DgTable::showContextMenu(int row, int col) {
    Fl_Menu_Button menu(Fl::event_x(), Fl::event_y(), 0, 0);
    menu.add("Copy Selection\tCmd+C", 0, nullptr);
    
    if (isNestedList(row, col)) {
        menu.add("View Nested List", 0, nullptr);
    }
    
    const Fl_Menu_Item* picked = menu.popup();
    if (picked) {
        if (strcmp(picked->label(), "Copy Selection\tCmd+C") == 0) {
            copySelection();
        } else if (strcmp(picked->label(), "View Nested List") == 0) {
            if (m_nestedCb) {
                DYN_LIST* nested = getNestedList(row, col);
                if (nested) {
                    char name[128];
                    snprintf(name, sizeof(name), "%s[%d]", columnName(col), row);
                    m_nestedCb(nested, name);
                }
            }
        }
    }
}

void DgTable::copySelection() {
    if (!m_dg) return;
    
    std::string text;
    char buf[256];
    
    // Get selection bounds
    int top, bot, left, right;
    get_selection(top, left, bot, right);
    
    if (top < 0) return;  // No selection
    
    // Header row
    for (int c = left; c <= right; c++) {
        if (c > left) text += '\t';
        text += columnName(c);
    }
    text += '\n';
    
    // Data rows
    for (int r = top; r <= bot; r++) {
        for (int c = left; c <= right; c++) {
            if (c > left) text += '\t';
            formatCellValue(buf, sizeof(buf), r, c);
            text += buf;
        }
        text += '\n';
    }
    
    Fl::copy(text.c_str(), text.length(), 1);
}
