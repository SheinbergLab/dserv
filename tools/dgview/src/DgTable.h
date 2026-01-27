#ifndef DGTABLE_H
#define DGTABLE_H

/**
 * DgTable - FLTK Table widget for displaying DYN_GROUP data
 * 
 * Handles all data types including nested lists.
 * Efficient rendering for large datasets (only draws visible cells).
 */

#include <FL/Fl.H>
#include <FL/Fl_Table_Row.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Menu_Button.H>

#include <df.h>
#include <dynio.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

class DgTable : public Fl_Table_Row {
public:
  // Callback for when a nested list cell is double-clicked
  using NestedListCallback = std::function<void(DYN_LIST* dl, const char* name)>;
  
  DgTable(int X, int Y, int W, int H, const char* L = nullptr);
  ~DgTable();
  
  // Set the data to display (does NOT take ownership)
  void setData(DYN_GROUP* dg);
  
  // Clear the display
  void clear() override;
  
  // Get current data
  DYN_GROUP* data() const { return m_dg; }
  
  // Get cell value as string
  std::string cellValue(int row, int col) const;
  
  // Get current row
  int currentRow() const { return m_currentRow; }
  
  // Check if cell contains a nested list
  bool isNestedList(int row, int col) const;
  
  // Get nested list at cell (nullptr if not a list)
  DYN_LIST* getNestedList(int row, int col) const;
  
  // Set callback for nested list double-click
  void setNestedListCallback(NestedListCallback cb) { m_nestedCb = cb; }
  
  // Column info
  const char* columnName(int col) const;
  int columnDataType(int col) const;
  
  // Copy selection to clipboard
  void copySelection();
  
protected:
  void draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H) override;
  int handle(int event) override;
  
private:
  void drawHeader(const char* s, int X, int Y, int W, int H, bool isColHeader);
  void drawData(const char* s, int X, int Y, int W, int H, bool selected, bool isNested);
  void formatCellValue(char* buf, size_t bufsize, int row, int col) const;
  void autoSizeColumns();
  void showContextMenu(int row, int col);
  int handleArrowKey(int key);
  
  DYN_GROUP* m_dg;
  NestedListCallback m_nestedCb;
  int m_lastClickRow;
  int m_lastClickCol;
  int m_currentRow;
  
  // Column width cache
  std::vector<int> m_colWidths;
  
  // Font settings
  Fl_Font m_font;
  int m_fontSize;
  Fl_Color m_headerBg;
  Fl_Color m_headerFg;
  Fl_Color m_cellBg;
  Fl_Color m_cellFg;
  Fl_Color m_nestedBg;
  Fl_Color m_selectedBg;
};

#endif // DGTABLE_H
