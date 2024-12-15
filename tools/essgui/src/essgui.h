// generated by Fast Light User Interface Designer (fluid) version 1.0400

#ifndef essgui_h
#define essgui_h

#include <FL/Fl_Tree.H>

int eval(char *command, void *cbdata);
void tree_cb(Fl_Tree*, void*);
int esscmd(char *cmd, std::string &rstr);
int esscmd(const char *cmd);
int set_host(void);
int add_host(const char *);
int set_subject(void);
int set_system(void);
int set_protocol(void);
int set_variant(void);

class VirtualEye;
void virtual_eye_cb(VirtualEye *w, void *data);

#endif