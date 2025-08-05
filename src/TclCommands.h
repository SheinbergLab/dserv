Tcl_Obj *dpoint_to_tclobj(Tcl_Interp *interp,  ds_datapoint_t *dpoint);
int dserv_exists_command(ClientData data, Tcl_Interp * interp, int objc,
		      Tcl_Obj * const objv[]);
int dserv_get_command(ClientData data, Tcl_Interp * interp, int objc,
		      Tcl_Obj * const objv[]);
int dserv_copy_command(ClientData data, Tcl_Interp * interp, int objc,
		       Tcl_Obj * const objv[]);
int now_command(ClientData data, Tcl_Interp * interp, int objc,
		Tcl_Obj * const objv[]);
int dserv_keys_command(ClientData data, Tcl_Interp * interp, int objc,
		       Tcl_Obj * const objv[]);
int dserv_dgdir_command(ClientData data, Tcl_Interp * interp, int objc,
		       Tcl_Obj * const objv[]);
int dserv_setdata_command (ClientData data, Tcl_Interp *interp,
			   int objc, Tcl_Obj * const objv[]);
int dserv_setdata64_command (ClientData data, Tcl_Interp *interp,
			     int objc, Tcl_Obj * const objv[]);
int dserv_timestamp_command(ClientData data, Tcl_Interp *interp,
			    int objc, Tcl_Obj * const objv[]);
int dserv_touch_command(ClientData data, Tcl_Interp * interp, int objc,
			Tcl_Obj * const objv[]);
int dserv_set_command(ClientData data, Tcl_Interp * interp, int objc,
		      Tcl_Obj * const objv[]);
int dserv_clear_command(ClientData data, Tcl_Interp * interp, int objc,
			Tcl_Obj * const objv[]);
int dserv_eval_command(ClientData data, Tcl_Interp * interp, int objc,
		       Tcl_Obj * const objv[]);
int process_get_param_command(ClientData data, Tcl_Interp * interp, int objc,
			      Tcl_Obj * const objv[]);
int process_set_param_command(ClientData data,
			      Tcl_Interp * interp, int objc,
			      Tcl_Obj * const objv[]);

