#ifndef FL_DATATABLE_H
#define FL_DATATABLE_H

#include <FL/Fl.H>
#include <FL/Fl_Table.H>
#include <FL/fl_draw.H>

#define MAX_ROWS 30
#define MAX_COLS 26

// Derive a class from Fl_Table
class DataTable : public Fl_Table {
  
  int data[MAX_ROWS][MAX_COLS];		// data array for cells

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
  // Handle drawing table's cells
  //     Fl_Table calls this function to draw each visible cell in the table.
  //     It's up to us to use FLTK's drawing functions to draw the cells the way we want.
  //
  void draw_cell(TableContext context, int ROW=0, int COL=0, int X=0, int Y=0, int W=0, int H=0) {
    static char s[40];
    switch ( context ) {
      case CONTEXT_STARTPAGE:                   // before page is drawn..
        fl_font(FL_HELVETICA, 16);              // set the font for our drawing operations
        return; 
      case CONTEXT_COL_HEADER:                  // Draw column headers
        snprintf(s, sizeof(s), "%c",'A'+COL);   // "A", "B", "C", etc.
        DrawHeader(s,X,Y,W,H);
        return; 
      case CONTEXT_ROW_HEADER:                  // Draw row headers
        snprintf(s, sizeof(s), "%03d:",ROW);    // "001:", "002:", etc
        DrawHeader(s,X,Y,W,H);
        return; 
      case CONTEXT_CELL:                        // Draw data in cells
        snprintf(s, sizeof(s), "%d",data[ROW][COL]);
        DrawData(s,X,Y,W,H);
        return;
      default:
        return;
    }
  }
public:
  // Constructor
  //     Make our data array, and initialize the table options.
  //
  DataTable(int X, int Y, int W, int H, const char *L=0) : Fl_Table(X,Y,W,H,L) {
    // Fill data array
    for ( int r=0; r<MAX_ROWS; r++ )
      for ( int c=0; c<MAX_COLS; c++ )
        data[r][c] = 1000+(r*1000)+c;
    // Rows
    rows(MAX_ROWS);             // how many rows
    row_header(1);              // enable row headers (along left)
    row_height_all(20);         // default height of rows
    row_resize(0);              // disable row resizing
    // Cols
    cols(MAX_COLS);             // how many columns
    col_header(1);              // enable column headers (along top)
    col_width_all(80);          // default width of columns
    col_resize(1);              // enable column resizing
    end();			// end the Fl_Table group
  }
  ~DataTable() { }
};

#endif // DL_DATATYPE_H
