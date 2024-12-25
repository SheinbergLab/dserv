#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Float_Input.H>
#include <FL/fl_string_functions.h>

#include <iostream>
#include <cassert>
#include <algorithm> 
#include <cctype>
#include <locale>
#include <vector>
#include <unordered_map>

#include <tcl.h>
#include <df.h>
#include <dfana.h>
#include <dynio.h>
#include <tcl_dl.h>

#include "b64.h"

#include "Fl_Console.h"
#include "Fl_DgFile.h"
#include "Fl_DgTable.h"
#include "Fl_PerfTable.h"

#include "EyeTouchWin.hpp"

#include "DservSocket.h"
#include "TclInterp.h"
#include "setup_ui.h"

#include <jansson.h>
#include <tcl.h>

#include "fort.hpp"

#include "essgui.h"
#include "mdns.h"

int add_tcl_commands(Tcl_Interp *interp);

class App *g_App;

class App
{
private:
  TclInterp *_interp;
  Tcl_HashTable widget_table;
  const char *WidgetKey = "widgets";
  std::unordered_map<std::string, Fl_Widget *> params;
  
public:
  bool auto_reload = true;		// reload variant immediately upon setting change
  std::thread dsnet_thread;
  DservSocket *ds_sock;
  Fl_Double_Window *win;
  int initfull = 0;
  const char *inithost = NULL;
  std::string host = std::string();
  void *drawable = nullptr;
  
public:
  App (int argc, char *argv[]) {
    // set the global app pointer
    g_App = this;
    int i=0;

#if 1
    if (Fl::args(argc, argv, i, argparse) < argc)
      Fl::fatal("Options are:\n -f = startup fullscreen\n\n -h = initial host\n%s", Fl::help);
    #else
    Fl::args(argc, argv);
    #endif
    
    Fl::lock(); /* "start" the FLTK lock mechanism */
    
    _interp = new TclInterp(argc, argv);

    ds_sock = new DservSocket();
    dsnet_thread = ds_sock->start_server();
    
    win = setup_ui(argc, argv);

    add_tcl_commands(interp());

    Tcl_InitHashTable(&widget_table, TCL_STRING_KEYS);
    Tcl_SetAssocData(interp(), WidgetKey, NULL, &widget_table);

    if (initfull) win->fullscreen();
    win->show(argc, argv);
  }

  ~App() {
    delete ds_sock;
    delete _interp;
  }

  void clear_params(void) { params.clear(); }
  void add_param(std::string key, Fl_Object *o) { params[key] = o; }
  Fl_Widget *find_param(std::string key)
  {
    if (params.find(key) == params.end()) return NULL;
    return params[key];
  }
  
  int disconnect_from_host(std::string hoststr)
  {
    ds_sock->unreg(host.c_str());

    host.clear();
    
    return 1;
  }

  /*
   * send command to ess server to "touch" region setting info for each window
   */
  void update_em_regions(void)
  {
    int result;
    std::string rstr;
    std::string cmd("for {set i 0} {$i < 8} {incr i} {ainGetRegionInfo $i}");
    
    if (!host.empty()) {
      result = ds_sock->esscmd(host, cmd, rstr);
    }
  }

  
  int connect_to_host(std::string hoststr)
  {
    host = hoststr;
    ds_sock->reg(hoststr.c_str());
    ds_sock->add_match(hoststr.c_str(), "ess/*");
    ds_sock->add_match(hoststr.c_str(), "system/*");
    ds_sock->add_match(hoststr.c_str(), "stimdg");
    ds_sock->add_match(hoststr.c_str(), "trialdg");

    /* touch variables to update interface */
    std::string rstr;
    ds_sock->esscmd(hoststr,
		    std::string("foreach v {ess/systems ess/protocols ess/variants ess/system ess/protocol "
				"ess/variant ess/subject ess/state ess/em_pos ess/obs_id ess/obs_total "
				"ess/block_pct_complete ess/block_pct_correct ess/variant_info "
				"ess/param_settings ess/params stimdg trialdg system/hostname "
				"system/os} { dservTouch $v }"), rstr);

    update_em_regions();

    return 1;
  }
    
  int eval(const char *command, std::string &resultstr)
  {
    return _interp->eval(command, resultstr);
  }

  static int argparse(int argc, char **argv, int &i) {
    if (argv[i][1] == 'f') { g_App->initfull = 1; i++; return 1; }
    if (argv[i][1] == 'h') {
      if (i+1 >= argc) return 0;
      g_App->inithost = argv[i+1];
      i += 2;
    }
    return 2;
  }

  void add_widget(char *name, Fl_Widget *o)
  {
    Tcl_HashEntry *entryPtr;
    int newentry;
    entryPtr = Tcl_CreateHashEntry(&widget_table, name, &newentry);
    Tcl_SetHashValue(entryPtr, o);
  }
  
  Tcl_Interp *interp(void) { return _interp->interp(); }
  int putGroup(DYN_GROUP *dg) { return _interp->tclPutGroup(dg); }
  DYN_LIST *findDynList(DYN_GROUP *dg, char *name) { return _interp->findDynList(dg, name); }
};

void linenoise_write(const char *buf, size_t n);

int linenoise_getch(void) {
  return output_term->getch();
}

void linenoise_write(const char *buf, size_t n) {
  output_term->append(buf, n);
}

  
static void clear_counter_widgets(void) {
  obscount_widget->value("");
  obscount_widget->redraw();
}

static void clear_widgets(void) {
  clear_counter_widgets();

  system_status_widget->value("");
  system_status_widget->redraw_label();
  
  system_widget->clear();
  protocol_widget->clear();
  variant_widget->clear();

  system_widget->redraw();
  protocol_widget->redraw();
  variant_widget->redraw();

  /* clear the table but leave tab label as stimdg */
  const char *l = "stimdg";
  stimdg_widget->clear(l);

  /* hide sorters */
  sorters_widget->hide();
  
  sysname_widget->value("");
  sysname_widget->redraw_label();

  sysos_widget->value("");
  sysos_widget->redraw_label();

  general_perf_widget->clear("");
  general_perf_widget->redraw();

  perftable_widget->clear("");
  perftable_widget->redraw();
  Tcl_VarEval(g_App->interp(), "if [dg_exists trialdg] { dg_delete trialdg; }", NULL);

  virtual_eye_widget->initialized = false;

  options_widget->clear();
  options_widget->redraw();
}  


/**
 Return an Fl_Tree_Reason as a text string name
*/
const char* reason_as_name(Fl_Tree_Reason reason) {
  switch ( reason ) {
        case FL_TREE_REASON_NONE:       return("none");
        case FL_TREE_REASON_SELECTED:   return("selected");
        case FL_TREE_REASON_DESELECTED: return("deselected");
        case FL_TREE_REASON_OPENED:     return("opened");
        case FL_TREE_REASON_CLOSED:     return("closed");
        case FL_TREE_REASON_DRAGGED:    return("dragged");
        case FL_TREE_REASON_RESELECTED: return("reselected");
        default:                        return("???");
      }
}

std::string get_system_name(const char *host)
{
  std::string hostname;
  int result =  g_App->ds_sock->esscmd(host, "dservGet system/hostname", hostname);
  if (!result || !strncmp(hostname.c_str(), "!TCL_ERROR", 10)) hostname.clear();
  return hostname;
}

void host_cb(Fl_Tree*, void*) {
  Fl_Tree_Item *item = host_widget->callback_item();
  if ( item ) {
    if (host_widget->callback_reason() == FL_TREE_REASON_DESELECTED) {
      g_App->disconnect_from_host(item->label());
      clear_widgets();
    }
    else if (host_widget->callback_reason() == FL_TREE_REASON_SELECTED) {
      g_App->connect_to_host(item->label());
      std::string system_name = get_system_name(item->label());
    }
#if 0  
    output_term->printf("TREE CALLBACK: label='%s' userdata=%ld reason=%s, changed=%d",
			item->label(),
			(long)(fl_intptr_t)host_widget->user_data(),
			reason_as_name(host_widget->callback_reason()),
			host_widget->changed() ? 1 : 0);
    // More than one click? show click count
    //    Should only happen if reason==FL_TREE_REASON_RESELECTED.
    //
    if ( Fl::event_clicks() > 0 ) {
      output_term->printf(", clicks=%d\n", (Fl::event_clicks()+1));
    } else {
      output_term->printf("\n");
    }
#endif    
  }
#if 0  
  else {
    output_term->printf("TREE CALLBACK: reason=%s, changed=%d, item=(no item -- probably multiple items were changed at once)\n",
            reason_as_name(host_widget->callback_reason()),
            host_widget->changed() ? 1 : 0);
  }
#endif
  host_widget->clear_changed();
}

int eval(char *command, void *cbdata) {
  std::string resultstr;
  auto result = g_App->eval(command, resultstr);

  if (resultstr.length()) {
    if (result != TCL_OK) output_term->append_ascii("\033[31m");
    output_term->append(resultstr.c_str());
    if (result != TCL_OK) output_term->append_ascii("\033[0m");
    output_term->append("\n");
  }
  output_term->redraw();
  return result;
}


int esscmd(char *cmd, std::string &rstr) {
  
  int result =  g_App->ds_sock->esscmd(g_App->host,
				       std::string(cmd),
				       rstr);
  return result;
}

int esscmd(const char *cmd) {
  std::string rstr;
  int result =  g_App->ds_sock->esscmd(g_App->host,
				       std::string(cmd), rstr);
  return result;
}

int set_subject(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "ess::set_subject %s",
	   subject_widget->text());
  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host,
			 std::string(cmd), rstr);
  return 0;
}

int set_system(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "ess::load_system %s",
	   system_widget->text());

  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host,
			 std::string(cmd), rstr);
  return 0;
}

int set_protocol(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "ess::load_system %s %s",
	   system_widget->text(),
	   protocol_widget->text());
  
  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host,
			 std::string(cmd), rstr);
  return 0;
}

int set_variant(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "ess::load_system %s %s %s",
	   system_widget->text(),
	   protocol_widget->text(),
	   variant_widget->text());

  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host,
			 std::string(cmd), rstr);
  return 0;
}

int add_host(const char *host)
{
  if (!host_widget->find_item(host)) {
    auto item = host_widget->add(host);
    return 1;
  }
  return 0;
}

void select_host(const char *host)
{
  Fl_Tree_Item *item;
  if ((item = host_widget->find_item(host))) {
    host_widget->select(host);
  }
}

int refresh_hosts(int timeout = 500)
{
  int timeout_ms = 500;
  const char *service = "_dserv._tcp";
  
  char buf[4096];
  send_mdns_query_service((char *) service, buf, sizeof(buf), timeout_ms);

  host_widget->clear();
  host_widget->showroot(0);

  /* no hosts found */
  if (!strlen(buf)) return 0;
  
  /* parse hosts */
  Tcl_Size argc;
  char *string;
  const char **argv;
  if (Tcl_SplitList(g_App->interp(), buf, &argc, &argv) == TCL_OK) {
    for (int i = 0; i < argc; i++) {
      char host[32];
      int in_addr, k;
      for (int j = 0, k = 0, in_addr = 0; k < sizeof(host); j++) {
	if (argv[i][j] == ' ') {
	  if (in_addr) {
	    host[k] = '\0';
	    break;
	  }
	}
	else {
	  in_addr = 1;
	  host[k++] = argv[i][j];
	}
      }
      host_widget->add(host);
    }
    Tcl_Free((char *) argv);
  }
  return 0;
}

void refresh_cb(Fl_Button *, void *)
{
  /* store current host so we can restore if still available */
  char *current_host = NULL;
  Fl_Tree_Item *item = host_widget->first_selected_item();

  if (item) {
    current_host = strdup(item->label());
    g_App->disconnect_from_host(current_host);
    //    output_term->printf("disconnected from host %s\n", current_host);
  }

  /* refresh host list */
  refresh_hosts();

  if (current_host) {
    //    output_term->printf("attempting to reconnect to host %s\n", current_host);
    select_host(current_host);
    free(current_host);
  }
  return;
}

void do_sortby(void)
{  
  Tcl_VarEval(g_App->interp(),
	      "setPerfTable {*}[do_sortby ",	      
	      sortby_1->text() ? sortby_1->text() : "", " ",
	      sortby_2->text() ? sortby_2->text() : "", "]", NULL);
}  

void sortby_cb(Fl_Choice *c, void *)
{
  do_sortby();
}

void configure_sorters(DYN_GROUP *dg)
{
  const char *reflistname = "stimtype";
  const char *remaining = "remaining";
  DYN_LIST *stimtype = g_App->findDynList(dg, (char *) reflistname);
  const int max_unique = 6;
  bool sortby1_set = false, sortby2_set = false;

  /* show them! */
  sorters_widget->show();
  
  std::string sortby_1_selection;
  std::string sortby_2_selection;
  
  if (sortby_1->text()) sortby1_set = true;
  if (sortby_2->text()) sortby2_set = true;
  
  if (sortby1_set) sortby_1_selection = std::string(sortby_1->text());
  if (sortby2_set) sortby_2_selection = std::string(sortby_2->text());

  sortby_1->clear();
  sortby_2->clear();
  
  if (!stimtype) return;
  int n = DYN_LIST_N(stimtype);

  // blanks for "unselecting" sortby
  sortby_1->add(" ");
  sortby_2->add(" ");

  for (int i = 0; i < DYN_GROUP_NLISTS(dg); i++) {
    if (DYN_LIST_N(DYN_GROUP_LIST(dg,i)) == n) {
      DYN_LIST *dl = DYN_GROUP_LIST(dg,i);
      char *name = DYN_LIST_NAME(dl);
      if (strcmp(name, reflistname) &&
	  strcmp(name, remaining) &&
	  DYN_LIST_DATATYPE(dl) != DF_LIST) {
	/* see if there are a reasonable number of "levels" */
	DYN_LIST *u = dynListUniqueList(dl);
	int nunique = DYN_LIST_N(u);
	dfuFreeDynList(u);
	if (nunique <= max_unique) {
	  sortby_1->add(name);
	  sortby_2->add(name);
	}
      }
    }    
  }

  /* restore choices if they still apply */
  int idx;
  if (sortby1_set) {
    if ((idx = sortby_1->find_index(sortby_1_selection.c_str())) >= 0) {
      sortby_1->value(idx);
    }
  }
  if (sortby2_set) {
    if ((idx = sortby_2->find_index(sortby_2_selection.c_str())) >= 0 ) {
      sortby_2->value(idx);
    }
  }
}


void virtual_eye_cb (VirtualEye *w, void *data)
{
  int result;
  std::string rstr;
  
  std::string cmd("set d [binary format s2 {");
  cmd += std::to_string(w->adc[1])+" "+std::to_string(w->adc[0])+"}];";
  cmd += "for {set i 0} {$i < 20} {incr i} {dservSetData ain/vals 0 4 $d};";
  cmd += "unset d";

  //  std::cout << cmd << std::endl;

  if (!g_App->host.empty()) {
    result = g_App->ds_sock->esscmd(g_App->host, cmd, rstr);
  }
}

static DYN_GROUP *decode_dg(const char *data, int length)
{
  DYN_GROUP *dg;
  unsigned int decoded_length = length;
  unsigned char *decoded_data;
  int result;

  if (!(dg = dfuCreateDynGroup(4))) {
    return NULL;
  }
  
  decoded_data = (unsigned char *) calloc(decoded_length, sizeof(char));
  result = base64decode((char *) data, length, decoded_data, &decoded_length);
  
  if (result) {
    free(decoded_data);
    return NULL;
  }
  
  if (dguBufferToStruct(decoded_data, decoded_length, dg) != DF_OK) {
    free(decoded_data);
    return NULL;
  }
  
  free(decoded_data);
  return dg;
}

class VariantSettingUserData: public Fl_Callback_User_Data
{
private:
  std::string _arg;
  std::vector<std::string> _settings;

public:
  VariantSettingUserData(const char *arg)
  {
    _arg      = std::string(arg);
  }

  const char *arg(void)      { return _arg.c_str(); }
  const char *setting(int i)  { return _settings[i].c_str(); }
  std::vector<std::string> *settings(void)  { return &_settings; }
  void add_setting(const char *s) { _settings.push_back(std::string(s)); }
  int find(const char *s)
  {
    for(std::vector<std::string>::size_type i = 0; i != _settings.size(); i++) {
      if (!strcmp(s, _settings[i].c_str())) return i;
    }
    return -1;
  }
};


void variant_setting_callback(Fl_Widget* o, void* data) {
  Fl_Choice *c = (Fl_Choice *) o;
  VariantSettingUserData *setting_info = (VariantSettingUserData *) data;

  clear_counter_widgets();
  
  /* clear performance widgets */
  perftable_widget->clear("");
  perftable_widget->redraw();

  general_perf_widget->clear("");
  general_perf_widget->redraw();
  
  std::string cmd("ess::set_variant_args {");
  cmd += std::string(setting_info->arg());
  cmd += " {";
  cmd += setting_info->settings()->at(c->value());
  cmd += "} }; ";

  if (g_App->auto_reload) {
    cmd += "ess::reload_variant";
  }

  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host, cmd, rstr);

}

int set_variant_options(Tcl_Obj *loader_args, Tcl_Obj *loader_options)
{
  Tcl_DictSearch search;
  Tcl_Obj *key, *value;
  int done;

  options_widget->clear();
  options_widget->begin();

  int row = 0;
  int height = 30;
  int xoff = options_widget->x();
  int yoff = options_widget->y();
  int label_width = 170;
  
  if (Tcl_DictObjFirst(g_App->interp(), loader_options, &search,
		       &key, &value, &done) != TCL_OK) {
    return TCL_ERROR;
  }

  /* split loader args to use below */
  Tcl_Size la_argc;
  const char **la_argv;
  Tcl_SplitList(g_App->interp(), Tcl_GetString(loader_args),
		&la_argc, &la_argv);
    
  for (; !done ; row++, Tcl_DictObjNext(&search, &key, &value, &done)) {
    Fl_Choice *choice = new Fl_Choice(xoff+label_width,
				      yoff+10+row*height,
				      options_widget->w()-(label_width+20), height, 0);
    choice->copy_label(Tcl_GetString(key));
    choice->align(Fl_Align(FL_ALIGN_LEFT));
    choice->labeltype(FL_NORMAL_LABEL);

    /* now add menu items */
    VariantSettingUserData *userdata;
    Tcl_Size argc, argcc;
    char *string;
    const char **argv, **argvv;
    if (Tcl_SplitList(g_App->interp(), Tcl_GetString(value),
		      &argc, &argv) == TCL_OK) {
      userdata = new VariantSettingUserData(Tcl_GetString(key));
      for (int i = 0; i < argc; i++) {
	if (Tcl_SplitList(g_App->interp(), argv[i],
			  &argcc, &argvv) == TCL_OK) {
	  if (argcc == 2) {
	    choice->add(argvv[0]);
	    userdata->add_setting(argvv[1]);
	  }
	  Tcl_Free((char *) argvv);
	}
      }
      choice->callback(variant_setting_callback, userdata, true);
      choice->when(FL_WHEN_RELEASE_ALWAYS);
      Tcl_Free((char *) argv);
    }

    /* now set the input choice to match the current setting */
    if (la_argc > row) {
      int val = userdata->find(la_argv[row]);
      if (val > 0) choice->value(val);
      else choice->value(0);
    }
    else choice->value(0);
  }
  
  Tcl_Free((char *) la_argv);
  
  Tcl_DictObjDone(&search);  

  options_widget->end();
  options_widget->redraw();

  return TCL_OK;
}

void param_setting_callback(Fl_Widget *o, void *data)
{
  Fl_Input *input = (Fl_Input *) o;
  std::string cmd = "::ess::set_param ";
  cmd += input->label();
  cmd += " ";
  cmd += input->value();

  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host, cmd, rstr);
}

int add_params(const char *param_list)
{
  Tcl_Interp *interp = g_App->interp();
  Tcl_DictSearch search;
  Tcl_Obj *key, *value;
  int done;

  g_App->clear_params();
  
  settings_widget->clear();
  settings_widget->begin();

  int row = 0;
  int height = 30;
  int xoff = settings_widget->x();
  int yoff = settings_widget->y();
  int label_width = 170;
    
  Tcl_Obj *dict = Tcl_NewStringObj(param_list, -1);
  if (Tcl_DictObjFirst(g_App->interp(), dict, &search,
		       &key, &value, &done) != TCL_OK) {
    Tcl_DecrRefCount(dict);
    return -1;
  }
  for (; !done ; Tcl_DictObjNext(&search, &key, &value, &done), row++) {
   Tcl_Size argc, argcc;
   char *string;
   const char **argv, **argvv;
   if (Tcl_SplitList(g_App->interp(), Tcl_GetString(value),
		     &argc, &argv) == TCL_OK) {
     Fl_Widget *input;
     /*
      * argv[0] = value
      * argv[1] = variable_type (1=time,2=variable)
      * argv[2] = datatype
     */
     if (!strcmp(argv[2],"int")) {
       input = (Fl_Widget *) new Fl_Int_Input(xoff+label_width,
					      yoff+10+row*height,
					      options_widget->w()-(label_width+20), height, 0);
     }
     else if (!strcmp(argv[2],"float")) {
       input = (Fl_Widget *) new Fl_Float_Input(xoff+label_width,
						yoff+10+row*height,
						options_widget->w()-(label_width+20), height, 0);
     }
     else {
       input = (Fl_Widget *) new Fl_Input(xoff+label_width,
					  yoff+10+row*height,
					  options_widget->w()-(label_width+20), height, 0);
     }
     g_App->add_param(Tcl_GetString(key), input);
     
     input->copy_label(Tcl_GetString(key));
     input->align(Fl_Align(FL_ALIGN_LEFT));
     input->labeltype(FL_NORMAL_LABEL);

     /* differentiate time from variable settings by color */
     if (!strcmp(argv[1], "1")) input->labelcolor(fl_rgb_color(60, 50, 30));
     else input->labelcolor(fl_rgb_color(0, 0, 0));
     
     ((Fl_Input *) input)->value(argv[0]);
     input->callback(param_setting_callback);
     input->when(FL_WHEN_ENTER_KEY);
     Tcl_Free((char *) argv);
   }
  }
  Tcl_DictObjDone(&search);
  
  Tcl_DecrRefCount(dict);

  settings_widget->end();
  settings_widget->redraw();

  return TCL_OK;
  
}

int update_param(const char *pstr)
{
  int retval = -21;
  Fl_Input *o;
  Tcl_Size argc;
  const char **argv;
  if (Tcl_SplitList(g_App->interp(), pstr, &argc, &argv) == TCL_OK) {
    if (argc != 2) {
      retval = -1;
      goto clean_and_return;
    }
    o = (Fl_Input *) g_App->find_param(argv[0]);
    if (!o) {
      retval = 0;
      goto clean_and_return;
    }
    o->value(argv[1]);
    retval = 1;
    
  clean_and_return:
    Tcl_Free((char *) argv);
  }
  return retval;
}

void update_general_perf_widget(int complete, int correct)
{
  std::string cmd = "setGeneralPerfTable {";
  cmd += std::to_string(correct);
  cmd += " ";
  cmd += std::to_string(complete);
  cmd += "} {{% correct} {% complete}}";

  Tcl_VarEval(g_App->interp(), cmd.c_str(), NULL);
}

void process_dpoint_cb(void *cbdata) {
  const char *dpoint = (const char *) cbdata;
  // JSON parsing variables

  static int obs_id = 0, obs_total;
  static int block_percent_correct, block_percent_complete;
  
  json_error_t error;
  json_t *root;

  // Parse the JSON string
  root = json_loads(dpoint, 0, &error);

  // Check for parsing errors
  if (!root) {
       return;
  }

  // Extract and print values
  json_t *name = json_object_get(root, "name");
  json_t *dtype = json_object_get(root, "dtype");
  json_t *timestamp = json_object_get(root, "timestamp");
  json_t *data = json_object_get(root, "data");

  //  std::cout << dpoint << std::endl;
  
  if (!strcmp(json_string_value(name), "ess/obs_active")) {
   if (!strcmp(json_string_value(data), "0")) {
   	obscount_widget->textcolor(FL_FOREGROUND_COLOR);
	//	stimid_widget->value("");
	//	stimid_widget->redraw_label();
   } else {
   	obscount_widget->textcolor(FL_FOREGROUND_COLOR);
	obscount_widget->redraw();
   }
  }

  else if (!strcmp(json_string_value(name), "ess/em_pos")) {
    float x, y;
    int d1, d2;
    if (sscanf(json_string_value(data), "%d %d %f %f",
	       &d1, &d2, &x, &y) == 4) {
      eyetouch_widget->em_pos(x, y);
      if (!virtual_eye_widget->initialized) {
	virtual_eye_widget->set_em_pos(x, y);
	virtual_eye_widget->initialized = true;
      }
    }
  }

  else if (!strcmp(json_string_value(name), "ess/reset")) {
    clear_counter_widgets();
  }
  
  else if (!strcmp(json_string_value(name), "ess/state")) {
   if (!strcmp(json_string_value(data), "Stopped")) {
   	system_status_widget->textcolor(FL_RED);
   } else if (!strcmp(json_string_value(data),"Running")) {
   	system_status_widget->textcolor(fl_rgb_color(40, 200, 20));
   } else system_status_widget->textcolor(FL_BLACK);
    system_status_widget->value(json_string_value(data));
    system_status_widget->redraw_label();
  }

  // obs_id always precedes obs_total
  else if (!strcmp(json_string_value(name), "ess/obs_id")) {
    obs_id = atoi(json_string_value(data));
  }

  else if (!strcmp(json_string_value(name), "ess/obs_total")) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%s", obs_id+1, json_string_value(data));
    obscount_widget->value(buf);
    obscount_widget->redraw_label();
  }

  else if (!strcmp(json_string_value(name), "ess/subject")) {
    int idx;
    if ((idx = subject_widget->find_index(json_string_value(data))) >= 0) {
      subject_widget->value(idx);
    } else {
      idx = subject_widget->add(json_string_value(data));
      subject_widget->value(idx);
    }
    subject_widget->redraw();
  }

  else if (!strcmp(json_string_value(name), "ess/block_pct_complete")) {
    block_percent_complete = (int) (atof(json_string_value(data))*100);
    update_general_perf_widget(block_percent_complete, block_percent_correct);
  }
  
  else if (!strcmp(json_string_value(name), "ess/block_pct_correct")) {
    block_percent_correct = (int) (atof(json_string_value(data))*100);
    update_general_perf_widget(block_percent_complete, block_percent_correct);
  }

  else if (!strcmp(json_string_value(name), "ess/em_region_setting")) {
    int settings[8];
    if (sscanf(json_string_value(data), "%d %d %d %d %d %d %d %d",
	       &settings[0], &settings[1], &settings[2], &settings[3],
	       &settings[4], &settings[5], &settings[6], &settings[7]) == 8) {
      eyetouch_widget->eye_region_set(settings);
    }
  }

  else if (!strcmp(json_string_value(name), "ess/em_region_status")) {
    int status[4];
    if (sscanf(json_string_value(data), "%d %d %d %d",
	       &status[0], &status[1], &status[2], &status[3]) == 4) {
      eyetouch_widget->eye_status_set(status);
    }
  }
  
  else if (!strcmp(json_string_value(name), "ess/touch_region_setting")) {
    int settings[8];
    if (sscanf(json_string_value(data), "%d %d %d %d %d %d %d %d",
	       &settings[0], &settings[1], &settings[2], &settings[3],
	       &settings[4], &settings[5], &settings[6], &settings[7]) == 8) {
      eyetouch_widget->touch_region_set(settings);
    }
  }

  else if (!strcmp(json_string_value(name), "ess/touch_region_status")) {
    int status[4];
    if (sscanf(json_string_value(data), "%d %d %d %d",
	       &status[0], &status[1], &status[2], &status[3]) == 4) {
      eyetouch_widget->touch_status_set(status);
    }
  }

  else if (!strcmp(json_string_value(name), "ess/screen_w")) {
    int w;
    if (sscanf(json_string_value(data), "%d", &w) == 1)
      eyetouch_widget->screen_w(w);
  }
  else if (!strcmp(json_string_value(name), "ess/screen_h")) {
    int h;
    if (sscanf(json_string_value(data), "%d", &h) == 1)
      eyetouch_widget->screen_h(h);
  }
  else if (!strcmp(json_string_value(name), "ess/screen_halfx")) {
    float halfx;
    if (sscanf(json_string_value(data), "%f", &halfx) == 1)
      eyetouch_widget->screen_halfx(halfx);
  }
  else if (!strcmp(json_string_value(name), "ess/screen_halfy")) {
    float halfy;
    if (sscanf(json_string_value(data), "%f", &halfy) == 1)
      eyetouch_widget->screen_halfy(halfy);
  }

  else if (!strcmp(json_string_value(name), "ess/system")) {
    system_widget->value(system_widget->find_index(json_string_value(data)));
    clear_counter_widgets();

    /* clear performance widget */
    perftable_widget->clear("");
    perftable_widget->redraw();

    /* clear performance widget */
    general_perf_widget->clear("");
    general_perf_widget->redraw();
  }
  else if (!strcmp(json_string_value(name), "ess/protocol")) {
    protocol_widget->value(protocol_widget->find_index(json_string_value(data)));
  }
  else if (!strcmp(json_string_value(name), "ess/variant")) {
    variant_widget->value(variant_widget->find_index(json_string_value(data)));
  }
  else if (!strcmp(json_string_value(name), "ess/stimtype")) {
    //    stimid_widget->value(json_string_value(data));
    //    stimid_widget->redraw_label();
  }

  else if (!strcmp(json_string_value(name), "ess/variant_info")) {
    Tcl_Obj *options_key = Tcl_NewStringObj("loader_arg_options", -1);
    Tcl_Obj *args_key = Tcl_NewStringObj("loader_args", -1);
    Tcl_Obj *dict = Tcl_NewStringObj(json_string_value(data), -1);
    Tcl_Obj *loader_arg_options, *loader_args;
    if (Tcl_DictObjGet(g_App->interp(), dict, options_key, &loader_arg_options) == TCL_OK) {
      if (Tcl_DictObjGet(g_App->interp(), dict, args_key, &loader_args) == TCL_OK) {
	if (loader_arg_options && loader_args) {
	  set_variant_options(loader_args, loader_arg_options);
	}
      }
    }
    Tcl_DecrRefCount(dict);
    Tcl_DecrRefCount(args_key);
    Tcl_DecrRefCount(options_key);
  }

  else if (!strcmp(json_string_value(name), "ess/param_settings")) {
    add_params(json_string_value(data));
  }

  else if (!strcmp(json_string_value(name), "ess/param")) {
    update_param(json_string_value(data));
  }

  else if (!strcmp(json_string_value(name), "ess/params")) {
    update_param(json_string_value(data));
  }

  else if (!strcmp(json_string_value(name), "ess/systems")) {
    Tcl_Size argc;
    char *string;
    const char **argv;
    if (Tcl_SplitList(g_App->interp(),
		      json_string_value(data), &argc, &argv) == TCL_OK) {
      system_widget->clear();
      for (int i = 0; i < argc; i++) {
	system_widget->add(argv[i]);
      }
      Tcl_Free((char *) argv);
    }
  }

  else if (!strcmp(json_string_value(name), "ess/protocols")) {
    Tcl_Size argc;
    char *string;
    const char **argv;
    if (Tcl_SplitList(g_App->interp(),
		      json_string_value(data), &argc, &argv) == TCL_OK) {
      protocol_widget->clear();
      for (int i = 0; i < argc; i++) protocol_widget->add(argv[i]);
      Tcl_Free((char *) argv);
    }
  }

  else if (!strcmp(json_string_value(name), "ess/variants")) {
    Tcl_Size argc;
    char *string;
    const char **argv;
    if (Tcl_SplitList(g_App->interp(),
		      json_string_value(data), &argc, &argv) == TCL_OK) {
      variant_widget->clear();
      for (int i = 0; i < argc; i++) variant_widget->add(argv[i]);
      Tcl_Free((char *) argv);
    }
  }

  else if (!strcmp(json_string_value(name), "system/os")) {
    sysos_widget->value(json_string_value(data));
    sysos_widget->redraw_label();
  }
  
  else if (!strcmp(json_string_value(name), "system/hostname")) {
    sysname_widget->value(json_string_value(data));
    sysname_widget->redraw_label();
  }

  else if (!strcmp(json_string_value(name), "stimdg")) {
    const char *dgdata = json_string_value(data);
    DYN_GROUP *dg = decode_dg(dgdata, strlen(dgdata));
    Tcl_VarEval(g_App->interp(),
		"if [dg_exists stimdg] { dg_delete stimdg; }", NULL);
    g_App->putGroup(dg);
    stimdg_widget->set(dg);
    configure_sorters(dg);
  }

  else if (!strcmp(json_string_value(name), "ess/trialinfo")) {
    // use trialdg instead
  }
  
  else if (!strcmp(json_string_value(name), "trialdg")) {
    const char *dgdata = json_string_value(data);
    DYN_GROUP *dg = decode_dg(dgdata, strlen(dgdata));
    Tcl_VarEval(g_App->interp(),
		"if [dg_exists trialdg] { dg_delete trialdg; }", NULL);
    g_App->putGroup(dg);
    do_sortby();
  }
  
  else {
    output_term->append(json_string_value(name));
    output_term->append("=");
    output_term->append(json_string_value(data));
    output_term->append("\n");
  }

  // Free the JSON object to prevent memory leaks
  json_decref(root);
  return;
}

int findServersCmd(ClientData data, Tcl_Interp *interp,
		   int objc, Tcl_Obj * const objv[])
{
  int timeout_ms = 500;
  const char *service = "_dserv._tcp";
  
  if (objc < 1) {
    Tcl_WrongNumArgs(interp, 1, objv, "[timeout_ms]");
    return TCL_ERROR;
  }

  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[1], &timeout_ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  char buf[4096];
  send_mdns_query_service(Tcl_GetString(objv[1]), buf, sizeof(buf), timeout_ms);
  
  if (strlen(buf)) Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, strlen(buf)));
  return TCL_OK;
}

int esscmdCmd(ClientData data, Tcl_Interp *interp,
	      int objc, Tcl_Obj * const objv[])
{
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "cmd");
    return TCL_ERROR;
  }

  std::string rstr;
  std::string cmd(Tcl_GetString(objv[1]));
  int result = g_App->ds_sock->esscmd(g_App->host, cmd, rstr);
  int tclerror = 0;
  const char *errorstr = "!TCL_ERROR ";

  if (result && rstr.length()) {
    if (!strncmp(rstr.c_str(), errorstr, strlen(errorstr))) tclerror = 1;
    if (tclerror) {
      output_term->append_ascii("\033[31m");
      output_term->append(rstr.substr(strlen(errorstr)).c_str());
      output_term->append_ascii("\033[0m");
    }
    else {
      output_term->append(rstr.c_str());
    }
    output_term->append("\n");
  }
  
  return TCL_OK;
}

int terminalOutCmd(ClientData data, Tcl_Interp *interp,
		 int objc, Tcl_Obj * const objv[])
{
  Fl_Console *term = (Fl_Console *) data;
  
  if (objc < 2) {
    return TCL_OK;
  }

  term->append(Tcl_GetString(objv[1]));
  
  return TCL_OK;
}

int terminalResetCmd(ClientData data, Tcl_Interp *interp,
		 int objc, Tcl_Obj * const objv[])
{
  Fl_Console *term = (Fl_Console *) data;
  term->reset_terminal();
  term->redraw();
  return TCL_OK;
}

int createTableCmd(ClientData data, Tcl_Interp *interp,
		   int objc, Tcl_Obj * const objv[])
{
  PerfTable *table = (PerfTable *) data;
  
  Tcl_Size lcount;		// number of sublists
  Tcl_Size hcount;		// number of header elements
  Tcl_Size nrows;		// length of each sublist
  Tcl_Obj **sublists;		// array of sublists
  bool have_header = false;

  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "table_values header_row");
    return TCL_ERROR;
  }

  have_header = true;
		  
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

  Tcl_Obj *o;
  
  /* add header if specified */
  if (have_header) {
    for (int i = 0; i < hcount; i++) {
      Tcl_ListObjIndex(interp, objv[2], i, &o);  
      header.push_back(Tcl_GetString(o));
    }
  }

  /* fill table with data */
  rows.resize(nrows);
  for (int i = 0; i < nrows; i++) {
    for (int j = 0; j < lcount; j++) {
      Tcl_ListObjIndex(interp, sublists[j], i, &o);  
      rows[i].push_back(Tcl_GetString(o));
    }
  }

  table->set("", header, rows);
  
  return TCL_OK;
}

int cgwinFlushwinCmd(ClientData data, Tcl_Interp *interp,
		     int objc, Tcl_Obj * const objv[])
{
  cgwin_widget->redraw();
  return TCL_OK;
}


int add_tcl_commands(Tcl_Interp *interp)
{

  Tcl_CreateObjCommand(interp, "findServers",
		       (Tcl_ObjCmdProc *) findServersCmd,
		       (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "esscmd",
		       (Tcl_ObjCmdProc *) esscmdCmd,
		       (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);
#if 0
  Tcl_CreateObjCommand(interp, "behaviorPrint",
		       (Tcl_ObjCmdProc *) terminalOutCmd,
		       (ClientData) behavior_terminal,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "behaviorReset",
		       (Tcl_ObjCmdProc *) terminalResetCmd,
		       (ClientData) behavior_terminal,
		       (Tcl_CmdDeleteProc *) NULL);
#endif  
  Tcl_CreateObjCommand(interp, "setPerfTable",
		       (Tcl_ObjCmdProc *) createTableCmd,
		       (ClientData) perftable_widget,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "setGeneralPerfTable",
		       (Tcl_ObjCmdProc *) createTableCmd,
		       (ClientData) general_perf_widget,
		       (Tcl_CmdDeleteProc *) NULL);

  /* override the cgraph flushwin to call CGWin::draw */
  Tcl_CreateObjCommand(interp, "flushwin",
		       (Tcl_ObjCmdProc *) cgwinFlushwinCmd,
		       (ClientData) NULL,
		       (Tcl_CmdDeleteProc *) NULL);  
  
  return TCL_OK;
}

void initialize_subjects(void)
{
  subject_widget->add("sally");
  subject_widget->add("momo");
  subject_widget->add("riker");
  subject_widget->add("glenn");
  subject_widget->add("human");
}



void add_tcl_code(void)
{
const char *tclcode =
R"delim(
proc do_sortby { args } {
    set nargs [llength $args]
    if { $nargs > 2 } return
    set curdg [dg_copySelected trialdg [dl_oneof trialdg:status [dl_ilist 0 1]]]
    if { $nargs == 0 } {
	set pc [format %d [expr int(100*[dl_mean $curdg:status])]]
	set rt [format %.2f [dl_mean $curdg:rt]]
	set  n [dl_length $curdg:status]
	set headers "{% correct} rt n"
        dg_delete $curdg
	return [list [list $pc $rt $n] $headers]
    } elseif { $nargs == 1 } {
	set sortby $args
	dl_local pc [dl_selectSortedFunc $curdg:status \
			 "$curdg:$sortby" \
			 "stimdg:$sortby" \
			 dl_means]
	dl_local rt [dl_selectSortedFunc $curdg:rt \
			 "$curdg:$sortby" \
			 "stimdg:$sortby" \
			 dl_means]
	dl_local n [dl_selectSortedFunc $curdg:status \
			"$curdg:$sortby" \
			"stimdg:$sortby" \
			dl_lengths]
	dl_local result [dl_llist [dl_unique stimdg:$sortby]]
	dl_local pc [dl_slist \
                        {*}[lmap v [dl_tcllist [dl_int [dl_mult 100 $pc:1]]] {format %d $v}]]
	dl_local rt [dl_slist {*}[lmap v [dl_tcllist $rt:1] {format %.2f $v}]]
	dl_append $result $pc
	dl_append $result $rt
	dl_append $result $n:1
	
	set headers "$sortby {% correct} rt n"
        dg_delete $curdg
	return [list [dl_tcllist $result] $headers]
    } else {
	lassign $args s1 s2
	dl_local pc [dl_selectSortedFunc $curdg:status \
			 "$curdg:$s2 $curdg:$s1" \
			 "stimdg:$s2 stimdg:$s1" \
			 dl_means]
	dl_local rt [dl_selectSortedFunc $curdg:rt \
			 "$curdg:$s2 $curdg:$s1" \
			 "stimdg:$s2 stimdg:$s1" \
			 dl_means]
	dl_local n [dl_selectSortedFunc $curdg:status \
			 "$curdg:$s2 $curdg:$s1" \
			 "stimdg:$s2 stimdg:$s1" \
			 dl_lengths]
	dl_local result [dl_reverse [dl_uniqueCross stimdg:$s1 stimdg:$s2]]

	dl_local pc [dl_slist \
                         {*}[lmap v [dl_tcllist [dl_int [dl_mult 100 $pc:2]]] {format %d $v}]]
	dl_local rt [dl_slist {*}[lmap v [dl_tcllist $rt:2] {format %.2f $v}]]
	dl_append $result $pc
	dl_append $result $rt
	dl_append $result $n:2

	set headers "$s1 $s2 {% correct} rt n"
        dg_delete $curdg
	return [list [dl_tcllist $result] $headers]
    }
}
)delim";
 Tcl_Eval(g_App->interp(), tclcode);
 return;
}

int main(int argc, char *argv[]) {

  App app(argc, argv);

  add_tcl_code();

  initialize_subjects();
  refresh_hosts();
  if (g_App->inithost) {
    add_host(g_App->inithost);
    select_host(g_App->inithost);
  }

  Fl::run();
  
  app.dsnet_thread.detach();
  
  return 1;
}
