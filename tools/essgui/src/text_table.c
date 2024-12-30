
int createTableCmd(ClientData data, Tcl_Interp *interp,
		   int objc, Tcl_Obj * const objv[])
{
  Tcl_Size lcount;		// number of sublists
  Tcl_Size hcount;		// number of header elements
  Tcl_Size nrows;		// length of each sublist
  Tcl_Obj **sublists;		// array of sublists
  bool have_header = false;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "table_values [header_row]");
    return TCL_ERROR;
  }

  if (objc > 2) have_header = true;
		  
  /* ensure lists are all of same length */
  if (Tcl_ListObjGetElements(interp, objv[1], &lcount, &sublists) == TCL_OK) {
    Tcl_Size l;
    if (Tcl_ListObjLength(interp, sublists[0], &nrows) != TCL_OK)
      return TCL_ERROR;
    for (int i = 1; i < lcount; i++) {
      if (Tcl_ListObjLength(interp, sublists[i], &l) != TCL_OK)
	return TCL_ERROR;
      if (l != nrows) {
	Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
			 ": lists must be equal length", NULL);
	return TCL_ERROR;
      }
    }
  }

  if (have_header) {
    if (Tcl_ListObjLength(interp, objv[2], &hcount) != TCL_OK) return TCL_ERROR;
    if (hcount != lcount) {
      Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		       ": invalid header row", NULL);
      return TCL_ERROR;
    }
  }
  
  ft_table_t *table = ft_create_table();
  Tcl_Obj *o;
  
  ft_set_border_style(table, FT_NICE_STYLE);
  
  const char **elts = (const char **) calloc(lcount, sizeof(char *));

  /* add header if specified */
  if (have_header) {
    ft_set_cell_prop(table, 0, FT_ANY_COLUMN, FT_CPROP_ROW_TYPE, FT_ROW_HEADER);
    ft_set_cell_prop(table, 0, FT_ANY_COLUMN,
		     FT_CPROP_CONT_TEXT_STYLE, FT_TSTYLE_BOLD);
    for (int i = 0; i < hcount; i++) {
      Tcl_ListObjIndex(interp, objv[2], i, &o);  
      elts[i] = Tcl_GetString(o);
    }
    ft_row_write_ln(table, hcount, elts);
  }

  /* fill table with data */
  for (int i = 0; i < nrows; i++) {
    for (int j = 0; j < lcount; j++) {
      Tcl_ListObjIndex(interp, sublists[j], i, &o);  
      elts[j] = Tcl_GetString(o);
    }
    ft_row_write_ln(table, lcount, elts);
  }

  free(elts);

  /* Move table to the center of the screen */
  ft_set_tbl_prop(table, FT_TPROP_LEFT_MARGIN, 1);
  
  const char *table_str = (const char *) ft_to_string(table);
  Tcl_SetObjResult(interp, Tcl_NewStringObj(table_str, -1));
  ft_destroy_table(table);
  return TCL_OK;
}
