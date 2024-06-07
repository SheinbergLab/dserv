#ifndef __JIMINTERP_H
#define __JIMINTERP_H

#include <jim.h>
#include <cassert>

class JimInterp {
private:
  Jim_Interp *_interp;

public:
  JimInterp(void)
  {
    /* Create Jim instance */
    _interp = Jim_CreateInterp();
    assert(_interp != NULL);
    
    /* We register base commands, so that we actually implement Tcl. */
    Jim_RegisterCoreCommands(_interp);
    
    /* And initialise any static extensions */
    Jim_InitStaticExtensions(_interp);
  }

  ~JimInterp(void)
  {
    /* Free the interpreter */
    Jim_FreeInterp(_interp);
  }

  Jim_Interp *interp(void) { return _interp; }
  
  int eval(const char *command, std::string &resultstr)
  {
    int len;
    int result = Jim_Eval(_interp, command);
    auto rstr = Jim_GetString(Jim_GetResult(_interp), &len);
    resultstr = std::string(rstr, len);
    return result;
  }

  std::string eval(const char *command)
  {
    int len;
    int result = Jim_Eval(_interp, command);
    auto rstr = Jim_GetString(Jim_GetResult(_interp), &len);
    return std::string(rstr, len);
  }
};


#endif /* __JIMINTERP_H */
