#include <FL/Fl.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Table_Row.H>
#include <FL/fl_draw.H>

#include "Fl_DgFile.h"
#include "Fl_DgTable.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Handle drawing all cells in table
void DGTable::draw_cell(TableContext context, 
			  int R, int C, int X, int Y, int W, int H)
{
  static char s[64];
  switch ( context )
    {
	case CONTEXT_STARTPAGE:
	    fl_font(FL_HELVETICA, 12);
	    return;

	case CONTEXT_ROW_HEADER:
	case CONTEXT_COL_HEADER:
	    fl_push_clip(X, Y, W, H);
	    {
		fl_draw_box(FL_THIN_UP_BOX, X, Y, W, H, color());
		fl_color(FL_BLACK);

		if (context == CONTEXT_COL_HEADER) {
		  snprintf(s, sizeof(s), "%s", DYN_LIST_NAME(DYN_GROUP_LIST(dg, C)));
		} else {
		  snprintf(s, sizeof(s), "%d", R);
		}
		
		fl_draw(s, X, Y, W, H, FL_ALIGN_CENTER);
	    }
	    fl_pop_clip();
	    return;

	case CONTEXT_CELL:
	{
	    fl_push_clip(X, Y, W, H);
	    {
	        // BG COLOR
		fl_color( row_selected(R) ? selection_color() : FL_WHITE);
		fl_rectf(X, Y, W, H);

		// TEXT
		fl_color(FL_BLACK);
		snprintf(s, sizeof(s), "%s", cell_name(R, C));
		fl_draw(s, X, Y, W, H, FL_ALIGN_CENTER);

		// BORDER
		fl_color(FL_LIGHT2); 
		fl_rect(X, Y, W, H);
	    }
	    fl_pop_clip();
	    return;
	}

	default:
	    return;
    }
}
