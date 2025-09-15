#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Float_Input.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/fl_string_functions.h>
#include <FL/Fl_Check_Button.H>

#include <iostream>
#include <sstream>
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

#include "Fl_Console.hpp"
#include "Fl_DgFile.h"
#include "Fl_DgTable.h"
#include "Fl_PerfTable.h"

#include "MeshDiscovery.h"

#include "EyeTouchWin.hpp"

#include "TclEditor.h"
#include "EssguiFileDialog.h"

#include "DservSocket.h"
#include "TclInterp.h"
#include "setup_ui.h"

#include <jansson.h>
#include <tcl.h>

#include "fort.hpp"

#include "essgui.h"
#include "mdns.h"


void menu_cb(Fl_Widget*, void*) {
}


void suggest_cb(EssguiFileDialog* dialog, void* data) {
  std::string suggested;
  const char *cmd = "ess::file_suggest";
  auto result = esscmd((char *) cmd, suggested);
  if (!suggested.empty() && suggested[0] != '!') {
    dialog->set_suggested_filename(suggested.c_str());
  }
}

static void show_file_dialog() {
  EssguiFileDialog dialog("Open Data File");
  dialog.set_suggest_callback(suggest_cb);

  int result = dialog.show(); 
  
  // Handle final result (OK or Cancel)
  switch (result) {
  case 0: // Cancel
    break;
    
  case 1: // OK
    if (strlen(dialog.filename()) > 0) {
      std::string rstr;
      std::string open_cmd("ess::file_open ");
      open_cmd += dialog.filename();
      
      auto result = esscmd(open_cmd, rstr);
      if (rstr[0] != '1') {
	fl_message("%s", rstr.c_str());
      }
    }
    break;
  }  
}

void open_cb(Fl_Widget*, void*) {
    show_file_dialog();
}

void close_cb(Fl_Widget*, void*) {
  std::string rstr;
  std::string close_cmd("ess::file_close");
  auto result = esscmd(close_cmd, rstr);
  if (rstr[0] != '1') {
    fl_message("%s", rstr.c_str());
  }
}

void exit_cb(Fl_Widget*, void*) {
  exit(0);
}

Fl_Menu_Item menuitems[] = {
  { "&File",              FL_COMMAND + 'f', 0, 0, FL_SUBMENU },
    { "&Open Datafile...",    FL_COMMAND + 'o', (Fl_Callback *)open_cb },
    { "&Close Datafile...",   FL_COMMAND + 'c', (Fl_Callback *)close_cb },
    { "&Save Script...",   FL_COMMAND + 's', (Fl_Callback *)save_script_cb },
    { "E&xit",            FL_COMMAND + 'q', (Fl_Callback *)exit_cb, 0 },
    { 0 },

  { "&Edit", 0, 0, 0, FL_SUBMENU },
    { "Cu&t",             FL_COMMAND + 'x', (Fl_Callback *)menu_cb },
    { "&Copy",            FL_COMMAND + 'c', (Fl_Callback *)menu_cb },
    { "&Paste",           FL_COMMAND + 'v', (Fl_Callback *)menu_cb },
    { "&Delete",          0, (Fl_Callback *)menu_cb },
    { "Preferences",      0, 0, 0, FL_SUBMENU },
      { "Line Numbers",   FL_COMMAND + 'l', (Fl_Callback *)menu_cb, 0, FL_MENU_TOGGLE },
      { "Word Wrap",      0,                (Fl_Callback *)menu_cb, 0, FL_MENU_TOGGLE },
      { 0 },
    { 0 },
  { 0 }
};

class App *g_App;

int add_tcl_commands(Tcl_Interp *interp);

class App
{
private:
  TclInterp *_interp;
  Tcl_HashTable widget_table;
  const char *WidgetKey = "widgets";
  std::unordered_map<std::string, Fl_Widget *> params;
  std::unordered_map<std::string, Fl_Widget *> states;
  std::unordered_map<std::string, std::vector<Fl_Widget *>> stim_params;
  std::unordered_map<std::string, Fl_Text_Buffer *> text_buffers;
  std::string current_prompt;

public:
  typedef enum {
    TERM_LOCAL, TERM_STIM, TERM_ESS, TERM_GIT, TERM_OPENIRIS, TERM_MSG
  } TerminalMode;
  bool auto_reload = true; // reload variant immediately upon setting change
  std::thread dsnet_thread;
  DservSocket *ds_sock;
  Fl_Double_Window *win;
  int initfull = 0;
  const char *inithost = NULL;
  std::string host = std::string();
  void *drawable = nullptr;
  TerminalMode terminal_mode = TERM_LOCAL;
  MeshDiscovery *meshDiscovery;

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
    
    meshDiscovery = new MeshDiscovery();

#if 0
    meshDiscovery->setDiscoveryCallback([this](const MeshDiscovery::PeerInfo& peer) {
      output_term->append("Found: ");
      output_term->append(peer.name.c_str());
      output_term->append("\n");
      output_term->redraw();
      Fl::check();
    });
#endif
    
    win = setup_ui(argc, argv);

    // After initialization, you can set up command completion:
    std::vector<std::string> commands = {
      "exit", "/ess", "/stim", "/essgui", "/git", "/openiris", "/msg"
      // Add other commands as needed
    };
    output_term->update_command_list(commands);
    output_term->set_prompt("essgui> ");
    
    add_tcl_commands(interp());

    init_text_widgets();
    
    Tcl_InitHashTable(&widget_table, TCL_STRING_KEYS);
    Tcl_SetAssocData(interp(), WidgetKey, NULL, &widget_table);

    if (initfull) win->fullscreen();
    win->show(argc, argv);
  }

  ~App() {
    meshDiscovery->stopDiscovery();
    delete meshDiscovery;
    delete ds_sock;
    delete _interp;
  }

  void set_controls_enabled(bool enabled, bool disconnect = false) {
    if (enabled) {
      subject_group->activate();
      load_combos_group->activate();
      settings_group->activate();
      virtual_checkbox_group->activate();
    } else {
      subject_group->deactivate();
      load_combos_group->deactivate();
      settings_group->deactivate();
    }

    if (disconnect) {
      eyetouch_widget->set_virtual_eye_enabled(false);
      eyetouch_widget->set_virtual_touch_enabled(false);
      virtual_touch_checkbox->value(false);
      virtual_eye_checkbox->value(false);
      virtual_checkbox_group->deactivate();

      // clear out the current system's param settings
      clear_params();
      settings_widget->clear();
    }
    
    win->redraw();
  }

  
  // Helper function to get the currently active editor
  TclEditor* get_current_editor() {
    if (!editor_tabs) return nullptr;
    
    Fl_Group* current_tab = static_cast<Fl_Group*>(editor_tabs->value());
    if (!current_tab) return nullptr;
    
    for (int i = 0; i < current_tab->children(); ++i) {
      TclEditor* editor = dynamic_cast<TclEditor*>(current_tab->child(i));
      if (editor) return editor;
    }
    
    return nullptr;
  }
  
  void init_text_widgets(void)
  {
    text_buffers["system"] = new Fl_Text_Buffer();
    configure_editor(system_editor, text_buffers["system"]);
    
    text_buffers["protocol"] = new Fl_Text_Buffer();
    configure_editor(protocol_editor, text_buffers["protocol"]);
    
    text_buffers["loaders"] = new Fl_Text_Buffer();
    configure_editor(loaders_editor, text_buffers["loaders"]);

    text_buffers["variants"] = new Fl_Text_Buffer();
    configure_editor(variants_editor, text_buffers["variants"]);

    text_buffers["stim"] = new Fl_Text_Buffer();
    configure_editor(stim_editor, text_buffers["stim"]);
  }

  void reset_text_widgets(void)
  {
    clear_editor_buffer("system");
    clear_editor_buffer("protocol");
    clear_editor_buffer("loaders");
    clear_editor_buffer("variants");
    clear_editor_buffer("stim");
  }
  
  void set_editor_buffer(TclEditor *editor, const char *name, const char *buf)
  {
    editor->track_modifications(false);
    text_buffers[name]->text(buf);
    editor->mark_modified(false);
    initial_styling(editor);
    editor->format_code();
    editor->track_modifications(true);
  }
  
  void clear_editor_buffer(const char *name)
  {
    text_buffers[name]->text("");
  }

  char *editor_buffer_contents(const char *name)
  {
    return text_buffers[name]->text();
  }

  void clear_params(void) { params.clear(); }
  void add_param(std::string key, Fl_Object *o) { params[key] = o; }
  Fl_Widget *find_param(std::string key)
  {
    if (params.find(key) == params.end()) return NULL;
    return params[key];
  }

  void clear_stim_params(void) { stim_params.clear(); }
  void add_stim_param(std::string key, Fl_Object *o) { stim_params[key].push_back(o); }
  std::vector<Fl_Widget *> find_stim_params(std::string key)
  {
    if (stim_params.find(key) == stim_params.end()) return std::vector<Fl_Widget *>();
    return stim_params[key];
  }
  
  void clear_states(void) { states.clear(); }
  void add_state(std::string key, Fl_Object *o) { states[key] = o; }
  Fl_Widget *find_state(std::string key)
  {
    if (states.find(key) == states.end()) return NULL;
    return states[key];
  }
  void select_action_state(std::string a_statename) {
    std::string statename = a_statename.substr(0, a_statename.length() - 2);
    for (auto it : states) {
      std::string key = it.first;
      Fl_OpBox *b = (Fl_OpBox *) it.second;
      if (key == statename) {
	if (!b->GetSelected()) { b->SetSelected(1); b->redraw(); }
      }
      else {
	if (b->GetSelected()) { b->SetSelected(0); b->redraw(); }
      }
    }
  };
  
  void select_transition_state(std::string t_statename) {
    std::string statename = t_statename.substr(0, t_statename.length() - 2);
    for (auto it : states) {
      std::string key = it.first;
      Fl_OpBox *b = (Fl_OpBox *) it.second;
      if (key == statename) {
	if (!b->GetSelected()) { b->SetSelected(1); b->redraw(); }
      }
      else {
	if (b->GetSelected()) { b->SetSelected(0); b->redraw(); }
      }
    }
  };

  void obs_on(void) { obs_widget->color(FL_RED); obs_widget->redraw(); }
  void obs_off(void) { obs_widget->color(FL_BACKGROUND_COLOR); obs_widget->redraw(); }

  int disconnect_from_host(std::string hoststr)
  {
    ds_sock->unreg(host.c_str());

    event_widget->onHostDisconnected();
    
    set_controls_enabled(false, true);

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

  /*
   * send command to ess server to "touch" region setting info for each touch window
   */
  void update_touch_regions(void)
  {
    int result;
    std::string rstr;
    std::string cmd("for {set i 0} {$i < 8} {incr i} {touchGetRegionInfo $i}");

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
    ds_sock->add_match(hoststr.c_str(), "eventlog/events");
    ds_sock->add_match(hoststr.c_str(), "stimdg");
    ds_sock->add_match(hoststr.c_str(), "trialdg");
    ds_sock->add_match(hoststr.c_str(), "openiris/settings");
    ds_sock->add_match(hoststr.c_str(), "print");
    ds_sock->add_match(hoststr.c_str(), "graphics/*");

    /* touch variables to update interface (check spaces at EOL!) */
    std::string rstr;
    ds_sock->esscmd(hoststr,
		    std::string("foreach v {ess/systems ess/protocols "
				"ess/variants ess/system ess/protocol "
				"ess/variant ess/subject ess/state ess/em_pos "
				"ess/obs_id ess/obs_total "
				"ess/block_pct_complete ess/block_pct_correct "
				"ess/variant_info_json ess/screen_w ess/screen_h "
				"ess/screen_halfx ess/screen_halfy "
				"ess/state_table ess/rmt_cmds "
				"ess/system_script ess/protocol_script "
				"ess/variants_script ess/loaders_script "
				"ess/stim_script ess/param_settings "
				"ess/state_table ess/params stimdg trialdg "
				"ess/git/branches ess/git/branch "
				"ess/viz_config "
				"system/hostname system/os openiris/settings} "
				"{ dservTouch $v }"),
		    rstr);

    update_em_regions();
    update_touch_regions();

    set_controls_enabled(true);

    event_widget->onHostConnected();
    
    return 1;
  }
    
  int eval(const char *command, std::string &resultstr)
  {
    return _interp->eval(command, resultstr);
  }

  int ess_eval(const char *command, std::string &resultstr)
  {
    int retval = ds_sock->esscmd(g_App->host,
					std::string(command),
					resultstr);
    return retval;
  }

  int git_eval(const char *command, std::string &resultstr)
  {
    int retval = ds_sock->gitcmd(g_App->host,
				 std::string(command),
				 resultstr);
    return retval;
  }

  int msg_eval(const char *command, std::string &resultstr)
  {
    int retval = ds_sock->msgcmd(g_App->host,
				 std::string(command),
				 resultstr);
    return retval;
  }
  
  int openiris_eval(const char *command, std::string &resultstr)
  {
    int retval = ds_sock->openiriscmd(g_App->host,
				      std::string(command),
				      resultstr);
    return retval;
  }
  
  int stim_eval(const char *command, std::string &resultstr)
  {
    int retval = ds_sock->stimcmd(g_App->host,
				  std::string(command),
				  resultstr);
    return retval;
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
  system_widget->redraw();

  protocol_widget->clear();
  protocol_widget->redraw();

  variant_widget->clear();
  variant_widget->redraw();

  branch_widget->clear();
  branch_widget->redraw();
  
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

  options_widget->clear();
  options_widget->redraw();

  g_App->reset_text_widgets();
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

void file_open_cb(Fl_Button*, void*)
{

}

void file_close_cb(Fl_Button*, void*)
{

}

void host_cb(Fl_Tree*, void*) {
    Fl_Tree_Item *item = host_widget->callback_item();
    if (item) {
        if (host_widget->callback_reason() == FL_TREE_REASON_DESELECTED) {
            g_App->disconnect_from_host(item->label());
            clear_widgets();
        }
        else if (host_widget->callback_reason() == FL_TREE_REASON_RESELECTED) {
            // User clicked on already selected item - deselect it
            item->deselect();
            g_App->disconnect_from_host(item->label());
            clear_widgets();
        }	
        else if (host_widget->callback_reason() == FL_TREE_REASON_SELECTED) {
            std::string selectedText = item->label();
            
            // Extract IP address from display text
            std::string host = MeshDiscovery::extractIpFromDisplayText(selectedText);
            
            g_App->connect_to_host(host);
            std::string system_name = get_system_name(host.c_str());
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
  int result;

  Fl_Console *term = output_term;

  if (!strcmp(command, "exit")) g_App->eval(command, resultstr);
  
  else if (!strcmp(command, "/ess")) {
    output_term->set_prompt("ess> ");
    g_App->terminal_mode = App::TERM_ESS;
  }
  
  else if (!strcmp(command, "/stim")) {
    output_term->set_prompt("stim> ");
    g_App->terminal_mode = App::TERM_STIM;
  }
  
  else if (!strcmp(command, "/essgui")) {
    output_term->set_prompt("essgui> ");
    g_App->terminal_mode = App::TERM_LOCAL;
  }

  else if (!strcmp(command, "/git")) {
    output_term->set_prompt("git> ");
    g_App->terminal_mode = App::TERM_GIT;
  }
  
  else if (!strcmp(command, "/openiris")) {
    output_term->set_prompt("openiris> ");
    g_App->terminal_mode = App::TERM_OPENIRIS;
  }
  
  else if (!strcmp(command, "/msg")) {
    output_term->set_prompt("msg> ");
    g_App->terminal_mode = App::TERM_MSG;
  }
  
  else {
    switch (g_App->terminal_mode) {
    case App::TERM_LOCAL:
      result = g_App->eval(command, resultstr);
      break;
    case App::TERM_ESS:
      result = g_App->ess_eval(command, resultstr);
      if (resultstr.rfind("!TCL_ERROR ", 0) != std::string::npos) {
	resultstr = resultstr.substr(11);
	result = TCL_ERROR;
      }
      else {
	result = TCL_OK;
      }
      break;
    case App::TERM_STIM:
      result = g_App->stim_eval(command, resultstr);
      if (resultstr.empty()) result = TCL_OK;
      else if (resultstr.rfind("!TCL_ERROR ", 0) != std::string::npos) {
	resultstr = resultstr.substr(11);
	result = TCL_ERROR;
      }
      else {
	result = TCL_OK;
      }
      break;
    case App::TERM_GIT:
      result = g_App->git_eval(command, resultstr);
      if (resultstr.empty()) result = TCL_OK;
      else if (resultstr.rfind("!TCL_ERROR ", 0) != std::string::npos) {
	resultstr = resultstr.substr(11);
	result = TCL_ERROR;
      }
      else {
	result = TCL_OK;
      }
      break;
    case App::TERM_OPENIRIS:
      result = g_App->openiris_eval(command, resultstr);
      if (resultstr.empty()) result = TCL_OK;
      else if (resultstr.rfind("!TCL_ERROR ", 0) != std::string::npos) {
	resultstr = resultstr.substr(11);
	result = TCL_ERROR;
      }
      else {
	result = TCL_OK;
      }
      break;
    case App::TERM_MSG:
      result = g_App->msg_eval(command, resultstr);
      if (resultstr.empty()) result = TCL_OK;
      else if (resultstr.rfind("!TCL_ERROR ", 0) != std::string::npos) {
	resultstr = resultstr.substr(11);
	result = TCL_ERROR;
      }
      else {
	result = TCL_OK;
      }
      break;
    }
  
    if (strlen(resultstr.c_str())) {
      if (result != TCL_OK) output_term->append_ascii("\033[31m");
      output_term->append(resultstr.c_str());
      if (result != TCL_OK) output_term->append_ascii("\033[0m");
      output_term->append("\n");
    }
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

int esscmd(std::string cmd, std::string &rstr) {
  
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
  snprintf(cmd, sizeof(cmd), "evalNoReply {ess::load_system %s}",
	   system_widget->text());

  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host,
			 std::string(cmd), rstr);
  return 0;
}

int set_protocol(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "evalNoReply {ess::load_system %s %s}",
	   system_widget->text(),
	   protocol_widget->text());
  
  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host,
			 std::string(cmd), rstr);
  return 0;
}

int set_variant(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "evalNoReply {ess::load_system %s %s %s}",
	   system_widget->text(),
	   protocol_widget->text(),
	   variant_widget->text());

  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host,
			 std::string(cmd), rstr);
  return 0;
}

int reload_system(void) {
  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host, "evalNoReply ess::reload_system", rstr);
  return 0;
}

int reload_protocol(void) {
  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host, "evalNoReply ess::reload_protocol", rstr);
  return 0;
}

int reload_variant(void) {
  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host, "evalNoReply ess::reload_variant", rstr);
  return 0;
}

int save_settings(void) {
  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host, "ess::save_settings", rstr);
  return 0;
}

int reset_settings(void) {
  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host,
			 "ess::reset_settings", rstr);
  reload_variant();
  return 0;
}

void set_branch_cb(Fl_Widget *w, void *cbData) {
  Fl_Choice *b = (Fl_Choice *) w;
  char cmd[256];
  std::string rstr;

  snprintf(cmd, sizeof(cmd), "send git {git::switch_and_pull %s}", b->text());
  g_App->ess_eval(cmd, rstr);

  reload_variant();
}

void update_eye_settings(Fl_Widget *w, long wtype)
{
  std::string cmd;

  std::ostringstream oss;
  switch(wtype) {
  case 1:
    {
      Wheel_Spinner *spinner = static_cast<Wheel_Spinner *>(w);
      oss << "::openiris::set_param offset_h " << spinner->value();
      cmd = oss.str();
    }
    break;
  case 2:
    {
      Wheel_Spinner *spinner = static_cast<Wheel_Spinner *>(w);
      oss << "::openiris::set_param offset_v " << spinner->value();
      cmd = oss.str();
    }
    break;
  case 3:
    {
      Wheel_Spinner *spinner = static_cast<Wheel_Spinner *>(w);
      oss << "::openiris::set_param scale_h " << spinner->value();
      cmd = oss.str();
    }
    break;
  case 4:
    {
      Wheel_Spinner *spinner = static_cast<Wheel_Spinner *>(w);
      oss << "::openiris::set_param scale_v " << spinner->value();
      cmd = oss.str();
    }
    break;
  case 5:
    {
      Fl_Check_Button *check = static_cast<Fl_Check_Button *>(w);
      oss << "::openiris::set_param invert_h " << std::to_string(check->value());
      cmd = oss.str();
    }
    break;
  case 6:
    {
      Fl_Check_Button *check = static_cast<Fl_Check_Button *>(w);
      oss << "::openiris::set_param invert_v " << std::to_string(check->value());
      cmd = oss.str();
    }
    break;
    
  default: return; break;
  }
  
  std::string rstr;
  g_App->openiris_eval(cmd.c_str(), rstr);
}

// Version for string input
int refresh_eye_settings(Tcl_Interp *interp, const char *dictString)
{
  // Convert string to Tcl object
  Tcl_Obj *dictObj = Tcl_NewStringObj(dictString, -1);
  Tcl_IncrRefCount(dictObj);  // Prevent garbage collection
  
  // Verify it's a valid dictionary by trying to get its size
  Tcl_Size dictSize;
  if (Tcl_DictObjSize(interp, dictObj, &dictSize) != TCL_OK) {
    Tcl_DecrRefCount(dictObj);
    return TCL_ERROR;
  }
    
  Tcl_DictSearch search;
  Tcl_Obj *keyObj, *valueObj;
  int done;
  
  // Start the dictionary search
  if (Tcl_DictObjFirst(interp, dictObj, &search, &keyObj, &valueObj, &done) != TCL_OK) {
    Tcl_DecrRefCount(dictObj);
    return TCL_ERROR;
  }
  
  // Iterate through all key-value pairs
  while (!done) {
    const char *key = Tcl_GetString(keyObj);
    const char *value = Tcl_GetString(valueObj);
    
    // Get typed values as needed
    if (strcmp(key, "scale_h") == 0 || strcmp(key, "scale_v") == 0) {
      double doubleValue;
      if (Tcl_GetDoubleFromObj(interp, valueObj, &doubleValue) == TCL_OK) {
	if (!strcmp(key, "scale_h")) hGain_input->value(doubleValue);
	else vGain_input->value(doubleValue);
      }
    } else if (strcmp(key, "offset_h") == 0 || strcmp(key, "offset_v") == 0) {
      int intValue;
      if (Tcl_GetIntFromObj(interp, valueObj, &intValue) == TCL_OK) {
	if (!strcmp(key, "offset_h")) hBias_input->value(intValue);
	else vBias_input->value(intValue);
      }
    } else if (strcmp(key, "invert_h") == 0 || strcmp(key, "invert_v") == 0) {
      int intValue;
      if (Tcl_GetIntFromObj(interp, valueObj, &intValue) == TCL_OK) {
	if (!strcmp(key, "invert_h")) hInvert_checkbox->value(intValue);
	else vInvert_checkbox->value(intValue);
      }
    }
    
    Tcl_DictObjNext(&search, &keyObj, &valueObj, &done);
  }
  
  // Clean up
  Tcl_DictObjDone(&search);
  Tcl_DecrRefCount(dictObj);
  return TCL_OK;
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

void check_discovery_completion(void*) {
  if (g_App->meshDiscovery->isDiscoveryComplete()) {
    // Update the host widget with discovered peers
    host_widget->clear();
    host_widget->showroot(0);
    
    // Add localhost if available
    if (g_App->meshDiscovery->isLocalhostAvailable()) {
      host_widget->add("localhost");
    }
    
    // Add discovered peers
    auto displayTexts = g_App->meshDiscovery->getPeerDisplayTexts();
    for (const auto& displayText : displayTexts) {
      host_widget->add(displayText.c_str());
    }
    
    int itemCount = 0;
    for (Fl_Tree_Item *item = host_widget->first(); item; item = host_widget->next(item)) {
      itemCount++;
    }
    
    if (itemCount == 0) {
      host_widget->add("No systems found");
    }
    
    // Try to reconnect to previous host if it was set
    std::string reconnectHost = g_App->meshDiscovery->getReconnectHost();
    if (!reconnectHost.empty()) {
      select_host(reconnectHost.c_str());
      g_App->meshDiscovery->clearReconnectHost();
    }
        
    host_widget->redraw();
  } else {
    // Check again in 0.5 seconds
    Fl::repeat_timeout(0.5, check_discovery_completion);
  }
}

int refresh_hosts(int timeout = 2000)
{
  output_term->append("Starting mesh discovery...\n");
  output_term->redraw();
  Fl::check();
  
  // Discover peers
  int result = g_App->meshDiscovery->discoverPeers(timeout);  // Assumes you made it public
  
  // Clear and rebuild host widget
  host_widget->clear();
  host_widget->showroot(0);
  
  // Add localhost if available
  if (g_App->meshDiscovery->isLocalhostAvailable()) {
    host_widget->add("localhost");
  }
  
  // Add discovered peers
  auto displayTexts = g_App->meshDiscovery->getPeerDisplayTexts();
  for (const auto& displayText : displayTexts) {
    host_widget->add(displayText.c_str());
  }
  
  // Count items
  int itemCount = 0;
  for (Fl_Tree_Item *item = host_widget->first(); item; item = host_widget->next(item)) {
    itemCount++;
  }
  
  if (itemCount > 0) {
    output_term->append("Discovery complete: found ");
    output_term->append(std::to_string(itemCount).c_str());
    output_term->append(" system(s)\n");
  } else {
    output_term->append("No systems discovered\n");
  }
  
  output_term->redraw();
  return result;
}

void refresh_cb(Fl_Button *, void *)
{
  // stop any ongoing discovery
  g_App->meshDiscovery->stopDiscovery();
 
  /* store current host so we can restore if still available */
  char *current_host = NULL;
  Fl_Tree_Item *item = host_widget->first_selected_item();

  if (item) {
    current_host = strdup(item->label());
    g_App->disconnect_from_host(current_host);
  }

  /* start non-blocking refresh */
  host_widget->clear();
  host_widget->showroot(0);
  host_widget->add("Searching...");
  host_widget->redraw();
  
  // Store current_host for restoration after discovery
  if (current_host) {
    g_App->meshDiscovery->setReconnectHost(current_host);
    free(current_host);
  }
  
  g_App->meshDiscovery->startBackgroundDiscovery();
  
  // The completion check will handle reconnection
  Fl::add_timeout(0.5, check_discovery_completion);
  
  return;
}
void save_script_cb(Fl_Widget *w, void *cbData)
{
  TclEditor *editor = nullptr;

  // find the active editor
  if (editor_tabs) {
    Fl_Group* current_tab = static_cast<Fl_Group*>(editor_tabs->value());
    if (current_tab) {
      // Find the TclEditor in the current tab
      for (int i = 0; i < current_tab->children(); ++i) {
	editor = dynamic_cast<TclEditor*>(current_tab->child(i));
	break;
      }
    }
  }

  if (!editor) return;
  
  // use its label to save correct script type
  std::string type(editor->label());
  std::string text = g_App->editor_buffer_contents(type.c_str());
  std::string cmd = std::string("ess::save_script ") + type + " {" + text + "}";
  std::string rstr;
  g_App->msg_eval(cmd.c_str(), rstr);
  //std::cout << std::string("ess::save_script ") + type << std::endl;
  editor->mark_saved();
}

void push_script_cb(Fl_Widget *w, void *cbData)
{
  std::string cmd = std::string("git::commit_and_push");
  std::string rstr;
  g_App->git_eval(cmd.c_str(), rstr);
  //  std::cout << cmd << std::endl;
}

void pull_script_cb(Fl_Widget *w, void *cbData)
{
  std::string cmd = std::string("git::pull");
  std::string rstr;
  g_App->git_eval(cmd.c_str(), rstr);
  //  std::cout << cmd << std::endl;
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

// Functions that EyeTouchWin can call
void send_virtual_eye_data(int adc_x, int adc_y) {
  std::string cmd("set d [binary format s2 {");
  cmd += std::to_string(adc_y) + " " + std::to_string(adc_x) + "}];";
  cmd += "dservSetData ain/vals 0 4 $d; unset d";
  
  std::string rstr;
  if (!g_App->host.empty()) {
    g_App->ds_sock->esscmd(g_App->host, cmd, rstr);
  }
}

void send_virtual_touch_event(int x, int y, int event_type) {
  std::string cmd("set d [binary format s3 {");
  cmd += std::to_string(x) + " " + std::to_string(y) + " " + 
         std::to_string(event_type) + "}];";
  cmd += "dservSetData mtouch/event 0 4 $d; unset d";
  
  std::string rstr;
  if (!g_App->host.empty()) {
    g_App->ds_sock->esscmd(g_App->host, cmd, rstr);
  }
}

void virtual_eye_checkbox_cb(Fl_Check_Button* b, void*)
{
  eyetouch_widget->set_virtual_eye_enabled(b->value());
}

void virtual_touch_checkbox_cb(Fl_Check_Button* b, void*)
{
  eyetouch_widget->set_virtual_touch_enabled(b->value());
}

void virtual_eye_cb (VirtualEye *w, void *data)
{
  int result;
  std::string rstr;
  int nreps = 1;
  
  std::string cmd("set d [binary format s2 {");
  cmd += std::to_string(w->adc[1])+" "+std::to_string(w->adc[0])+"}];";
  cmd += "for {set i 0} {$i < ";
  cmd += std::to_string(nreps);
  cmd += "} {incr i} {dservSetData ain/vals 0 4 $d};";
  cmd += "unset d";

  //  std::cout << cmd << std::endl;

  if (!g_App->host.empty()) {
    result = g_App->ds_sock->esscmd(g_App->host, cmd, rstr);
  }
}

void virtual_joystick_cb (VirtualJoystick *w, void *data)
{
  int result;
  std::string rstr;

  std::string cmd;

  if (w->button_has_changed()) {
    int button = w->get_button_state();
    cmd+= "dservSet joystick/button " + std::to_string(button) + ";";
  }

  if (w->state_has_changed()) {
    int state = w->get_state();
    cmd+= "dservSet joystick/value " + std::to_string(state) + ";";
  }
  
  //  if (!cmd.empty()) { std::cout << cmd << std::endl; }

  if (!g_App->host.empty() && !cmd.empty()) {
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

  std::string selected_value = setting_info->settings()->at(c->value());
  
  // Use Tcl to properly format the list
  Tcl_Obj *cmdList = Tcl_NewListObj(0, NULL);
  Tcl_ListObjAppendElement(NULL, cmdList,
			   Tcl_NewStringObj(setting_info->arg(), -1));
  Tcl_ListObjAppendElement(NULL, cmdList,
			   Tcl_NewStringObj(selected_value.c_str(), -1));
  
  std::string cmd("ess::set_variant_args {");
  cmd += Tcl_GetString(cmdList);
  cmd += "}";
  
  Tcl_DecrRefCount(cmdList);  

  std::string rstr;
  g_App->ds_sock->esscmd(g_App->host, cmd, rstr);

  if (g_App->auto_reload) {
    reload_variant();
  }

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

    /* Split loader args to use below */
    Tcl_Size la_argc;
    Tcl_Obj **la_argv;
    if (Tcl_ListObjGetElements(g_App->interp(), loader_args, &la_argc, &la_argv) != TCL_OK) {
        return TCL_ERROR;
    }
    
    for (; !done ; row++, Tcl_DictObjNext(&search, &key, &value, &done)) {
        Fl_Choice *choice = new Fl_Choice(xoff+label_width,
                                          yoff+10+row*height,
                                          options_widget->w()-(label_width+20), height, 0);
        choice->copy_label(Tcl_GetString(key));
        choice->align(Fl_Align(FL_ALIGN_LEFT));
        choice->labeltype(FL_NORMAL_LABEL);

        VariantSettingUserData *userdata = new VariantSettingUserData(Tcl_GetString(key));
        
        // Parse the options list
        Tcl_Size optCount;
        Tcl_Obj **options;
        if (Tcl_ListObjGetElements(g_App->interp(), value, &optCount, &options) == TCL_OK) {
            int selectedIndex = -1;
            
            for (int i = 0; i < optCount; i++) {
                // Each option is a 2-element list: {display_name value}
                Tcl_Size elemCount;
                Tcl_Obj **elems;
                if (Tcl_ListObjGetElements(g_App->interp(), options[i], &elemCount, &elems) == TCL_OK && elemCount == 2) {
                    const char *displayName = Tcl_GetString(elems[0]);
                    
                    choice->add(displayName);
                    userdata->add_setting(Tcl_GetString(elems[1]));
                    
                    // Check if this matches the current value in loader_args
                    if (row < la_argc && selectedIndex == -1) {
                        // Use Tcl's string equal which handles dict/list normalization
                        Tcl_Obj *cmdv[4];
                        cmdv[0] = Tcl_NewStringObj("string", -1);
                        cmdv[1] = Tcl_NewStringObj("equal", -1);
                        cmdv[2] = la_argv[row];  // Current value from loader_args
                        cmdv[3] = elems[1];       // Option value from loader_arg_options
                        
                        Tcl_IncrRefCount(cmdv[0]);
                        Tcl_IncrRefCount(cmdv[1]);
                        
                        if (Tcl_EvalObjv(g_App->interp(), 4, cmdv, 0) == TCL_OK) {
                            int equal = 0;
                            Tcl_GetIntFromObj(g_App->interp(), Tcl_GetObjResult(g_App->interp()), &equal);
                            if (equal) {
                                selectedIndex = i;
                            }
                        }
                        
                        Tcl_DecrRefCount(cmdv[0]);
                        Tcl_DecrRefCount(cmdv[1]);
                    }
                }
            }
            
            // Set the current selection
            if (selectedIndex >= 0) {
                choice->value(selectedIndex);
            } else {
                choice->value(0);
            }
        }
        
        choice->callback(variant_setting_callback, userdata, true);
        choice->when(FL_WHEN_RELEASE_ALWAYS);
    }
    
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
     const char *vstr;
     const char *vkind;
     const char *vdtype;
     /*
      * argv[0] = value
      * argv[1] = variable_type (1=time,2=variable)
      * argv[2] = datatype
     */

     if (argc == 2) {
       vstr = "";
       vkind = argv[0];
       vdtype = argv[1];
     }
     else if (argc == 3) {
       vstr = argv[0];
       vkind = argv[1];
       vdtype = argv[2];
     }
     else continue;
     
     if (!strcmp(vdtype, "int")) {
       input = (Fl_Widget *) new Fl_Int_Input(xoff+label_width,
					      yoff+10+row*height,
					      options_widget->w()-(label_width+20), height, 0);
     }
     else if (!strcmp(vdtype, "float")) {
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
     if (argc != 3) input->labelcolor(fl_rgb_color(200, 50, 30));
     else if (!strcmp(vkind, "1")) input->labelcolor(fl_rgb_color(60, 50, 30));
     else input->labelcolor(fl_rgb_color(0, 0, 0));
     
     ((Fl_Input *) input)->value(vstr);
     input->callback(param_setting_callback);
     input->when(FL_WHEN_ENTER_KEY | FL_UNFOCUS);
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
  int retval = -2;
  Fl_Input *o;
  Tcl_Size argc;
  const char **argv;
  if (Tcl_SplitList(g_App->interp(), pstr, &argc, &argv) == TCL_OK) {
    if (argc % 2 != 0) {
      retval = -1;
      goto clean_and_return;
    }
    for (int i = 0; i < argc; i+=2) {
      o = (Fl_Input *) g_App->find_param(argv[i]);
      if (!o) {
	retval = 0;
	goto clean_and_return;
      }
      o->value(argv[i+1]);
      o->redraw();
      retval = 1;
    }
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

void update_system_layout(const char *system_dict)
{
  Fl_OpDesk *opdesk = opdesk_widget;
  Tcl_DictSearch search;
  Tcl_Obj *key, *value;
  int done;
  
  int xoff = opdesk->x()+20;
  int yoff = opdesk->y()+10;
  int height = 60;		// height of each item
  int width = 120;		// width of each item
  int row = 0;			// current row
  int col = 0;			// current col
  int ncols = 3;		// number of cols
  float space_factor = 1.25;

  g_App->clear_states();
  
  opdesk->clear();
  opdesk->begin();

  Tcl_Obj *dict = Tcl_NewStringObj(system_dict, -1);
  if (Tcl_DictObjFirst(g_App->interp(), dict, &search,
		       &key, &value, &done) != TCL_OK) {
    Tcl_DecrRefCount(dict);
    return;
  }
  
  for (int item = 0; !done ; item++, Tcl_DictObjNext(&search, &key, &value, &done)) {    
    Tcl_Size argc;
    const char **argv;

    /* connections from to */
    Tcl_SplitList(g_App->interp(), Tcl_GetString(value), &argc, &argv);

    row = item/ncols;
    col = item%ncols;
    
    char s[80];
    
    Fl_OpBox *opbox = new Fl_OpBox(xoff+space_factor*col*width,
				   yoff+space_factor*row*height,
				   width, height, 0);
    opbox->copy_label(Tcl_GetString(key));
    g_App->add_state(Tcl_GetString(key), opbox);

    opbox->begin();
    {
      /*Fl_OpButton *a =*/ new Fl_OpButton("In", FL_OP_INPUT_BUTTON);
      /*Fl_OpButton *b =*/ new Fl_OpButton("Out", FL_OP_OUTPUT_BUTTON);
    }
    opbox->end();

    Tcl_Free((char *) argv);
  }
  
  
  Tcl_DictObjDone(&search);  
  Tcl_DecrRefCount(dict);

  opdesk->end();
  opdesk->redraw();
}



void rmt_button_callback(Fl_Widget *o, void *data)
{
  Fl_Input *button = (Fl_Input *) o;
  std::string cmd = o->label();
  std::vector<Fl_Widget *> params = g_App->find_stim_params(cmd);

  if (params.size()) {
    for (Fl_Widget *p : params) {
      Fl_Input *input = (Fl_Input *) p;
      cmd += " ";
      cmd += input->value();
    }
  }
  std::string rstr;
  g_App->ds_sock->stimcmd(g_App->host, cmd, rstr);
}

int update_remote_commands(const char *rmt_cmds)
{
  Tcl_Interp *interp = g_App->interp();
  Tcl_DictSearch search;
  Tcl_Obj *key, *value;
  int done;

  g_App->clear_stim_params();
  
  rmt_commands_widget->clear();
  rmt_commands_widget->begin();

  int row = 0;
  int height = 30;
  int xoff = rmt_commands_widget->x()+4;
  int yoff = rmt_commands_widget->y();
  int label_width = 155;
    
  Tcl_Obj *dict = Tcl_NewStringObj(rmt_cmds, -1);
  if (Tcl_DictObjFirst(g_App->interp(), dict, &search,
		       &key, &value, &done) != TCL_OK) {
    Tcl_DecrRefCount(dict);
    return -1;
  }
  for (; !done ; Tcl_DictObjNext(&search, &key, &value, &done), row++) {
   Tcl_Size argc, argcc;
   const char **argv, **argvv;
   if (Tcl_SplitList(g_App->interp(), Tcl_GetString(value),
		     &argc, &argv) == TCL_OK) {
     Fl_Button *button = new Fl_Button(xoff,
				       yoff+10+row*height,
				       label_width, height, 0);
     button->copy_label(Tcl_GetString(key));     
     button->callback(rmt_button_callback);

     int option_width = 40;
     int option_padx = 3;
     for (int i = 0; i < argc; i++) {
       Fl_Input *input = new Fl_Input(xoff+label_width+option_padx+i*(option_width+option_padx),
				      yoff+10+row*height,
				      option_width, height, 0);
       input->copy_tooltip(argv[i]);
       g_App->add_stim_param(Tcl_GetString(key), input);
     }
     
     Tcl_Free((char *) argv);
   }
  }
  Tcl_DictObjDone(&search);
  
  Tcl_DecrRefCount(dict);

  rmt_commands_widget->end();
  rmt_commands_widget->redraw();

  return TCL_OK;
  
}


void process_dpoint_cb(void *cbdata) {
  const char *dpoint = (const char *) cbdata;
  if (!dpoint) return;

  json_error_t error;
  json_t *root;

  root = json_loads(dpoint, 0, &error);

  if (!root) {
       return;
  }

  // Extract and print values
  json_t *name = json_object_get(root, "name");
  json_t *dtype = json_object_get(root, "dtype");
  json_t *timestamp = json_object_get(root, "timestamp");
  json_t *data = json_object_get(root, "data");

  // Get safe string values
  const char* name_str = json_string_value(name);
  const char* data_str = json_string_value(data);
    
  static int obs_id = 0, obs_total;
  static int block_percent_correct, block_percent_complete;
  
  if (!strcmp(name_str, "eventlog/events")) {
    if (event_widget) {
      // Pass the entire JSON string, not just the data field
      event_widget->processEventlogData(std::string(dpoint));
    }
  }
  else if (!strcmp(json_string_value(name), "ess/obs_active")) {
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
    }
  }
  
  else if (!strcmp(json_string_value(name), "ess/touch_press")) {
    int x, y;
    if (sscanf(json_string_value(data), "%d %d", &x, &y) == 2) {
        eyetouch_widget->touch_pos(x, y);
        eyetouch_widget->show_touch(true);
    }
  }

  else if (!strcmp(json_string_value(name), "ess/touch_release")) {
    eyetouch_widget->show_touch(false);
  }
        
  else if (!strcmp(json_string_value(name), "ess/touch_drag")) {
    int x, y;
    if (sscanf(json_string_value(data), "%d %d", &x, &y) == 2) {
        eyetouch_widget->touch_pos(x, y);
    }
  }

  else if (!strcmp(json_string_value(name), "ess/touch_state")) {
  }
  
  else if (!strcmp(json_string_value(name), "ess/touch_event_type")) {
  }
  
  else if (!strcmp(json_string_value(name), "ess/transition_state")) {
    g_App->select_transition_state(json_string_value(data));
  }

  else if (!strcmp(json_string_value(name), "ess/action_state")) {
    g_App->select_action_state(json_string_value(data));
  }
  
  else if (!strcmp(json_string_value(name), "ess/reset")) {
    clear_counter_widgets();
  }

  else if (!strcmp(json_string_value(name), "ess/in_obs")) {
    if (!strcmp(json_string_value(data), "1")) g_App->obs_on();
    else g_App->obs_off();
  }

  /* not yet connected to any actions...*/
  else if (!strcmp(json_string_value(name), "ess/running")) { }
  else if (!strncmp(json_string_value(name), "ess/user_", 9)) { }
  else if (!strcmp(json_string_value(name), "ess/block_id")) { }
  else if (!strcmp(json_string_value(name), "ess/touch")) { }
  else if (!strncmp(json_string_value(name), "ess/block_n", 11)) { }
  
  else if (!strcmp(json_string_value(name), "ess/state")) {
   if (!strcmp(json_string_value(data), "Stopped")) {
   	system_status_widget->textcolor(FL_RED);
	event_widget->onSystemStateChanged(false);	
   } else if (!strcmp(json_string_value(data),"Running")) {
   	system_status_widget->textcolor(fl_rgb_color(40, 200, 20));
	event_widget->onSystemStateChanged(true);	
   } else system_status_widget->textcolor(FL_BLACK);
    system_status_widget->value(json_string_value(data));
    system_status_widget->redraw_label();
  }

  else if (!strcmp(json_string_value(name), "ess/status")) {
    if (!strcmp(json_string_value(data), "loading")) {
      fl_cursor(FL_CURSOR_WAIT);
      g_App->set_controls_enabled(false);
      Fl::flush();
    }
    else if (!strcmp(json_string_value(data), "running")) {
      fl_cursor(FL_CURSOR_DEFAULT);
      g_App->set_controls_enabled(false);
      Fl::flush();
    }
    else if (!strcmp(json_string_value(data), "stopped")) {
      fl_cursor(FL_CURSOR_DEFAULT);
      g_App->set_controls_enabled(true);
      Fl::flush();
    }
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

    // has happened, not sure why...
    if (!data) return;
    
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


  /* script files for this system */
  else if (!strcmp(json_string_value(name), "ess/system_script")) {
    g_App->set_editor_buffer(system_editor, "system", json_string_value(data));
  }
  else if (!strcmp(json_string_value(name), "ess/protocol_script")) {
    g_App->set_editor_buffer(protocol_editor, "protocol",
			     json_string_value(data));
  }
  else if (!strcmp(json_string_value(name), "ess/loaders_script")) {
    g_App->set_editor_buffer(loaders_editor,
			     "loaders", json_string_value(data));
  }
  else if (!strcmp(json_string_value(name), "ess/variants_script")) {
    g_App->set_editor_buffer(variants_editor,
			     "variants", json_string_value(data));
  }
  else if (!strcmp(json_string_value(name), "ess/stim_script")) {
    g_App->set_editor_buffer(stim_editor, "stim", json_string_value(data));
  }


else if (!strcmp(json_string_value(name), "ess/variant_info_json")) {
    // Parse the JSON just like Qt6 does
    json_error_t error;
    json_t *root = json_loads(json_string_value(data), 0, &error);
    if (!root) {
        return;
    }
    
    json_t *options = json_object_get(root, "options");
    json_t *loader_arg_names = json_object_get(root, "loader_arg_names");
    json_t *loader_args = json_object_get(root, "loader_args");
    
    if (!options || !loader_arg_names || !loader_args) {
        json_decref(root);
        return;
    }
    
    // Clear existing options
    options_widget->clear();
    options_widget->begin();
    
    int row = 0;
    int height = 30;
    int xoff = options_widget->x();
    int yoff = options_widget->y();
    int label_width = 170;
    
    // Iterate through each argument name
    size_t arg_index;
    json_t *arg_name;
    json_array_foreach(loader_arg_names, arg_index, arg_name) {
        const char *arg_name_str = json_string_value(arg_name);
        
        // Get options for this argument
        json_t *arg_options = json_object_get(options, arg_name_str);
        if (!arg_options || !json_is_array(arg_options)) {
            continue;
        }
        
        Fl_Choice *choice = new Fl_Choice(xoff+label_width,
                                          yoff+10+row*height,
                                          options_widget->w()-(label_width+20), height, 0);
        choice->copy_label(arg_name_str);
        choice->align(Fl_Align(FL_ALIGN_LEFT));
        choice->labeltype(FL_NORMAL_LABEL);
        
        VariantSettingUserData *userdata = new VariantSettingUserData(arg_name_str);
        
        int selected_index = 0;
        size_t opt_index;
        json_t *option;
        
        // Add each option to the choice widget
        json_array_foreach(arg_options, opt_index, option) {
            json_t *label = json_object_get(option, "label");
            json_t *value = json_object_get(option, "value");
            json_t *selected = json_object_get(option, "selected");
            
            if (label && value) {
                const char *label_str = json_string_value(label);
                const char *value_str = json_string_value(value);
                
                choice->add(label_str);
                userdata->add_setting(value_str);
                
                // Check if this is the selected option
                if (selected && json_is_true(selected)) {
                    selected_index = opt_index;
                }
            }
        }
        
        // Set the current selection
        choice->value(selected_index);
        
        choice->callback(variant_setting_callback, userdata, true);
        choice->when(FL_WHEN_RELEASE_ALWAYS);
        
        row++;
    }
    
    options_widget->end();
    options_widget->redraw();
    
    json_decref(root);
}  
  
  else if (!strcmp(json_string_value(name), "ess/variant_info")) {
#if 0
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
#endif
  }

  else if (!strcmp(json_string_value(name), "ess/param_settings")) {
    add_params(json_string_value(data));
  }
  
  else if (!strcmp(json_string_value(name), "ess/state_table")) {
    update_system_layout(json_string_value(data));
  }
    
  else if (!strcmp(json_string_value(name), "ess/rmt_cmds")) {
    update_remote_commands(json_string_value(data));
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

  else if (!strcmp(json_string_value(name), "ess/git/branch")) {
    branch_widget->value(branch_widget->find_index(json_string_value(data)));    
  }
  
  else if (!strcmp(json_string_value(name), "ess/git/branches")) {
    Tcl_Size argc;
    char *string;
    const char **argv;
    if (Tcl_SplitList(g_App->interp(),
		      json_string_value(data), &argc, &argv) == TCL_OK) {
      branch_widget->clear();
      for (int i = 0; i < argc; i++) branch_widget->add(argv[i]);
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

  else if (!strcmp(json_string_value(name), "ess/stiminfo")) {
    // use stimdg instead
  }

  else if (!strcmp(json_string_value(name), "ess/trialinfo")) {
    // use trialdg instead
  }

  // don't need this script, just listen for graphics/stiminfo
  else if (!strcmp(json_string_value(name), "ess/viz_config")) {
  }
  
  else if (!strcmp(json_string_value(name), "trialdg")) {
    const char *dgdata = json_string_value(data);
    DYN_GROUP *dg = decode_dg(dgdata, strlen(dgdata));
    Tcl_VarEval(g_App->interp(),
		"if [dg_exists trialdg] { dg_delete trialdg; }", NULL);
    g_App->putGroup(dg);
    do_sortby();
  }

  else if (!strcmp(json_string_value(name), "graphics/stimulus")) {
    const char *gbuf_commands = json_string_value(data);
    if (plot_widget) {
      plot_widget->processGraphicsData(std::string(gbuf_commands));
    }
  }
  
  else if (!strcmp(json_string_value(name), "print")) {
    const char *msg = json_string_value(data);
    output_term->append(msg);
    output_term->append("\n");
  }
  
  else if (!strcmp(json_string_value(name), "openiris/settings")) {
    refresh_eye_settings(g_App->interp(), json_string_value(data));
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

 Tcl_SetAssocData(g_App->interp(), "cgwin", NULL,
		  static_cast<ClientData>(plot_widget));
 Tcl_Eval(g_App->interp(), "cgAddGroup cgwin");
 
 return;
}

// On windows, make this a windows, not console, app
#ifdef _MSC_VER
# pragma comment(linker, "/subsystem:windows /ENTRY:mainCRTStartup")
#endif

int main(int argc, char *argv[]) {

  App app(argc, argv);

  int w;
  menu_bar->copy(menuitems, &w);
  
  add_tcl_code();

  initialize_subjects();

  // Finish configuring our system selector
  host_widget->clear();
  host_widget->selectmode(FL_TREE_SELECT_SINGLE);
  host_widget->item_reselect_mode(FL_TREE_SELECTABLE_ALWAYS);  
  host_widget->showroot(0);
  host_widget->add("Searching...");
  host_widget->redraw();
 
  // Start non-blocking discovery
  g_App->meshDiscovery->startBackgroundDiscovery();
   
  // Show window immediately
  if (g_App->initfull) g_App->win->fullscreen();
  g_App->win->show(argc, argv);
    
  // Check for discovery completion periodically
  Fl::add_timeout(0.5, check_discovery_completion);
    
    
  if (g_App->inithost) {
    add_host(g_App->inithost);
    select_host(g_App->inithost);
  }
  else {
    g_App->set_controls_enabled(false);
    virtual_checkbox_group->deactivate();
  }

  Fl::run();
  
  app.dsnet_thread.detach();
  
  return 1;
}
