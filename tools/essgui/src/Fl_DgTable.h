#ifndef __DGTABLE_H
#define __DGTABLE_H

#include <FL/Fl.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Table_Row.H>
#include <FL/fl_draw.H>

#include <tcl.h>
#include <df.h>
#include <dynio.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

class DGTable : public Fl_Table_Row
{
protected:

  void draw_cell(TableContext context,  		// table cell drawing
		 int R=0, int C=0, int X=0, int Y=0, int W=0, int H=0);
  //    void callback(TableContext context, 		// callback for table events
  //    		   int R, int C);
 
public:
  DYN_GROUP *dg;

  static void table_cb(Fl_Widget* o, void* data)
  {
    Fl_Table *table = (Fl_Table*)data;
    
    auto row = (int)table->callback_row();
#if 0
    fprintf(stderr, "%s callback: row=%d col=%d, context=%d, event=%d clicks=%d\n",
	    (const char*)table->label(),
	    (int)table->callback_row(),
	    (int)table->callback_col(),
	    (int)table->callback_context(),
	    (int)Fl::event(),
	    (int)Fl::event_clicks());
#endif
  }  
  
  
  DGTable(int X, int Y, int W, int H, const char *l=0):
    Fl_Table_Row(X, Y, W, H, l)
  {
    dg = NULL;
  }
  
  ~DGTable() {
    if (dg) dfuFreeDynGroup(dg);
  }

  void clear(const char *labelstr)
  {
    if (dg) dfuFreeDynGroup(dg);
    dg = NULL;
    label(labelstr);
    Fl_Table::clear();
  }
  
  void set(char *filename)
  {
    if (dg) dfuFreeDynGroup(dg);
    dg = DGFile::read_dgz(filename);
    if (!dg) return;

    // setup the row table
    //    selection_color(FL_YELLOW);
    when(FL_WHEN_RELEASE);	// handle table events on release
    rows(dgrows());
    cols(dgcols());
    col_header(1);		// enable col header
    col_resize(4);		// enable col resizing
    row_header(1);		// enable row header
    row_resize(4);		// enable row resizing
    callback(DGTable::table_cb, (void*) this);
    when(FL_WHEN_CHANGED|FL_WHEN_RELEASE);
    end();
  }
  
  void set(DYN_GROUP *indg)
  {
    if (!indg) return;
    if (dg) dfuFreeDynGroup(dg);

    /* copy the dg as the original get's loaded into the Tcl interpreter */
    dg = dfuCopyDynGroup((DYN_GROUP *) indg, (char *) DYN_GROUP_NAME(indg));

    label(DYN_GROUP_NAME(dg));
    // setup the row table
    when(FL_WHEN_RELEASE);	// handle table events on release
    rows(dgrows());
    cols(dgcols());
    col_header(1);		// enable col header
    col_resize(4);		// enable col resizing
    row_header(1);		// enable row header
    row_resize(4);		// enable row resizing
    callback(table_cb, (void*) this);
    when(FL_WHEN_CHANGED|FL_WHEN_RELEASE);
    end();
  }
  
  int dgrows()
  {
    int maxrows = 0;
    DYN_LIST *dl;
    for (int i = 0; i < DYN_GROUP_N(dg); i++) {
      dl = DYN_GROUP_LIST(dg, i);
      if (DYN_LIST_N(dl) > maxrows) maxrows = DYN_LIST_N(dl);
    }
    return maxrows;
  }

  int dgcols()
  {
    return DYN_GROUP_N(dg);
  }

  // return string representing value in cell
  char *cell_name(int R, int C)
  {
    static char valstr[128];
    DYN_LIST *dl;
    valstr[0] = '\0';
    if (C >= DYN_GROUP_N(dg))
      goto done;
    
    dl = DYN_GROUP_LIST(dg, C);
    if (R >= DYN_LIST_N(dl))
      goto done;

    switch (DYN_LIST_DATATYPE(dl)) {
    case DF_LONG:
      {
	int *vals = (int *) DYN_LIST_VALS(dl);
	snprintf(valstr, sizeof(valstr), "%d", vals[R]);
      }
      break;
    case DF_SHORT:
      {
	short *vals = (short *) DYN_LIST_VALS(dl);
	snprintf(valstr, sizeof(valstr), "%d", vals[R]);
      }
      break;
    case DF_FLOAT:
      {
	float *vals = (float *) DYN_LIST_VALS(dl);
	snprintf(valstr, sizeof(valstr), "%f", vals[R]);
      }
      break;
    case DF_CHAR:
      {
	char *vals = (char *) DYN_LIST_VALS(dl);
	snprintf(valstr, sizeof(valstr), "%d", vals[R]);
      }
      break;
    case DF_STRING:
      {
	char **vals = (char **) DYN_LIST_VALS(dl);
	snprintf(valstr, sizeof(valstr), "%s", vals[R]);
      }
      break;
    case DF_LIST:
      {
	DYN_LIST **vals = (DYN_LIST **) DYN_LIST_VALS(dl);
	const char *listtype;
	switch (DYN_LIST_DATATYPE(vals[R])) {
	case DF_LONG: listtype = "long"; break;
	case DF_SHORT: listtype = "short"; break;
	case DF_FLOAT: listtype = "float"; break;
	case DF_CHAR: listtype = "char"; break;
	case DF_STRING: listtype = "string"; break;
	case DF_LIST: listtype = "list"; break;
	}
	snprintf(valstr, sizeof(valstr), "%s (%d)", listtype, DYN_LIST_N(vals[R]));
      }
      break;
    }
  done:
    return &valstr[0];
  }
};


#endif // DGTable
