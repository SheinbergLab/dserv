#ifndef _PERFTABLE_H_
#define _PERFTABLE_H_

#include <FL/Fl.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Table_Row.H>
#include <FL/fl_draw.H>

#include <vector>

#include <cstdio>
#include <cstring>
#include <cstdlib>

class PerfTable : public Fl_Table_Row
{
protected:

  //    void callback(TableContext context, 		// callback for table events
  //    		   int R, int C);

  std::vector<std::string>         _colnames;
  std::vector<std::vector<std::string>> _rows;
  
public:

  static void table_cb(Fl_Widget* o, void* data)
  {
    Fl_Table *table = (Fl_Table*) data;
    
    auto row = (int)table->callback_row();
  }  
  
  
  PerfTable(int X, int Y, int W, int H, const char *l=0):
    Fl_Table_Row(X, Y, W, H, l)
  {
    
  }
  
  ~PerfTable()
  {

  }

  void clear(const char *labelstr)
  {
    label(labelstr);
    Fl_Table::clear();
  }
  
  void set(std::string name, std::vector<std::string> colnames, std::vector<std::vector<std::string>> rowdata)
    {
      _colnames = colnames;
      _rows = rowdata;
      copy_label(name.c_str());
      rows(datarows());
      cols(datacols());
      col_header(1);		// enable col header
      col_resize(4);		// enable col resizing
      row_header(0);		// enable row header
      callback(PerfTable::table_cb, (void*) this);
      when(FL_WHEN_CHANGED|FL_WHEN_RELEASE);
      end();
    }
  
  int datarows()
  {
    int maxrows = 0;
    return _rows.size();
  }

  int datacols()
  {
    return _rows.at(0).size();
  }

  // return std::string representing value in cell
  // Draw the row/col headings
  //    Make this a dark thin upbox with the text inside.
  //
  void DrawHeader(const char *s, int X, int Y, int W, int H) {
    fl_push_clip(X,Y,W,H);
    fl_draw_box(FL_THIN_UP_BOX, X,Y,W,H, row_header_color());
    fl_color(FL_BLACK);
    fl_draw(s, X,Y,W,H, FL_ALIGN_CENTER);
    fl_pop_clip();
  } 
  // Draw the cell data
  //    Dark gray text on white background with subtle border
  //
  void DrawData(const char *s, int X, int Y, int W, int H) {
    fl_push_clip(X,Y,W,H);
    // Draw cell bg
    fl_color(FL_WHITE); fl_rectf(X,Y,W,H);
    // Draw cell data
    fl_color(FL_GRAY0); fl_draw(s, X,Y,W,H, FL_ALIGN_CENTER);
    // Draw box border
    fl_color(color()); fl_rect(X,Y,W,H);
    fl_pop_clip();
  } 
  
  void draw_cell(TableContext context, 
		 int R, int C, int X, int Y, int W, int H)
  {
    static char s[64];

    switch ( context ) {
      case CONTEXT_STARTPAGE:                   // before page is drawn..
        fl_font(FL_HELVETICA, 14);              // set the font for our drawing operations
        return; 
      case CONTEXT_COL_HEADER:                  // Draw column headers
        snprintf(s, sizeof(s), "%s", _colnames.at(C).c_str());
        DrawHeader(s,X,Y,W,H);
        return; 
      case CONTEXT_ROW_HEADER:                  // Draw row headers
        snprintf(s, sizeof(s), "%03d:",R);      // "001:", "002:", etc
        DrawHeader(s,X,Y,W,H);
        return; 
      case CONTEXT_CELL:                        // Draw data in cells
        snprintf(s, sizeof(s), "%s", _rows.at(R).at(C).c_str());
        DrawData(s,X,Y,W,H);
        return;
      default:
        return;
    }
  }
};


#endif // PerfTable
