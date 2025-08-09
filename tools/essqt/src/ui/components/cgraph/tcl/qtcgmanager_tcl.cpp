#include "qtcgmanager.hpp"
#include "qtcgraph.hpp"
#include <tcl.h>

// Manager Tcl commands for the main interpreter
static int cg_list_cmd(ClientData data, Tcl_Interp *interp,
                      int objc, Tcl_Obj *const objv[])
{
    auto& manager = QtCGManager::getInstance();
    QStringList names = manager.getAllGraphNames();
    
    Tcl_Obj* listObj = Tcl_NewListObj(0, NULL);
    for (const QString& name : names) {
        Tcl_ListObjAppendElement(interp, listObj, 
            Tcl_NewStringObj(name.toUtf8().constData(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}

static int cg_send_cmd(ClientData data, Tcl_Interp *interp,
                      int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "graphName script");
        return TCL_ERROR;
    }
    
    auto& manager = QtCGManager::getInstance();
    QString graphName = Tcl_GetString(objv[1]);
    QString command = Tcl_GetString(objv[2]);
    
    QtCGraph* graph = manager.getGraph(graphName);
    if (!graph) {
        Tcl_SetResult(interp, "Graph not found", TCL_STATIC);
        return TCL_ERROR;
    }

    // Execute command and get result
    int result = graph->eval(command);
    QString graphResult = graph->result();
    
    // Set the result string (success or error message)
    if (!graphResult.isEmpty()) {
        Tcl_SetObjResult(interp, 
            Tcl_NewStringObj(graphResult.toUtf8().constData(), -1));
    }
    
    // Return the actual TCL result code
    return result;
}

static int cg_broadcast_cmd(ClientData data, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script");
        return TCL_ERROR;
    }
    
    auto& manager = QtCGManager::getInstance();
    QString command = Tcl_GetString(objv[1]);
    
    int result = manager.broadcast(command);
    
    if (result == TCL_OK) {
        return TCL_OK;
    } else {
        Tcl_SetResult(interp, "One or more commands failed", TCL_STATIC);
        return TCL_ERROR;
    }
}

static int cg_group_cmd(ClientData data, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "groupTag script");
        return TCL_ERROR;
    }
    
    auto& manager = QtCGManager::getInstance();
    QString groupTag = Tcl_GetString(objv[1]);
    QString command = Tcl_GetString(objv[2]);
    
    int result = manager.sendToGroup(groupTag, command);
    
    if (result == TCL_OK) {
        return TCL_OK;
    } else {
        Tcl_SetResult(interp, "Group command failed", TCL_STATIC);
        return TCL_ERROR;
    }
}

static int cg_addgroup_cmd(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "graphName groupTag");
        return TCL_ERROR;
    }
    
    auto& manager = QtCGManager::getInstance();
    QString graphName = Tcl_GetString(objv[1]);
    QString groupTag = Tcl_GetString(objv[2]);
    
    manager.addToGroup(graphName, groupTag);
    return TCL_OK;
}

static int cg_removegroup_cmd(ClientData data, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "graphName groupTag");
        return TCL_ERROR;
    }
    
    auto& manager = QtCGManager::getInstance();
    QString graphName = Tcl_GetString(objv[1]);
    QString groupTag = Tcl_GetString(objv[2]);
    
    manager.removeFromGroup(graphName, groupTag);
    return TCL_OK;
}

static int cg_groupmembers_cmd(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "groupTag");
        return TCL_ERROR;
    }
    
    auto& manager = QtCGManager::getInstance();
    QString groupTag = Tcl_GetString(objv[1]);
    QStringList members = manager.getGroupMembers(groupTag);
    
    Tcl_Obj* listObj = Tcl_NewListObj(0, NULL);
    for (const QString& name : members) {
        Tcl_ListObjAppendElement(interp, listObj, 
            Tcl_NewStringObj(name.toUtf8().constData(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}

static int cg_share_set_cmd(ClientData data, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "key value");
        return TCL_ERROR;
    }
    
    auto& manager = QtCGManager::getInstance();
    QString key = Tcl_GetString(objv[1]);
    
    // Store as string for simplicity
    QString value = Tcl_GetString(objv[2]);
    manager.setSharedData(key, value);
    
    return TCL_OK;
}

static int cg_share_get_cmd(ClientData data, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "key");
        return TCL_ERROR;
    }
    
    auto& manager = QtCGManager::getInstance();
    QString key = Tcl_GetString(objv[1]);
    QVariant value = manager.getSharedData(key);
    
    if (value.isValid()) {
        QString strValue = value.toString();
        Tcl_SetObjResult(interp, 
            Tcl_NewStringObj(strValue.toUtf8().constData(), -1));
    }
    
    return TCL_OK;
}

// Register manager commands with a Tcl interpreter
extern "C" int Qtcgmanager_Init(Tcl_Interp *interp)
{
    if (Tcl_InitStubs(interp, "9.0", 0) == nullptr) {
        return TCL_ERROR;
    }

    if (Tcl_PkgProvide(interp, "qtcgmanager", "1.0") != TCL_OK) {
        return TCL_ERROR;
    }

    // Create namespace
    Tcl_Eval(interp, "namespace eval ::cg {}");
    
    // Core commands
    Tcl_CreateObjCommand(interp, "::cg::list", 
                        (Tcl_ObjCmdProc *) cg_list_cmd, 
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "::cg::send",
                        (Tcl_ObjCmdProc *) cg_send_cmd, 
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "::cg::broadcast",
                        (Tcl_ObjCmdProc *) cg_broadcast_cmd, 
                        (ClientData) NULL, NULL);
    
    // Group commands
    Tcl_CreateObjCommand(interp, "::cg::group",
                        (Tcl_ObjCmdProc *) cg_group_cmd, 
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "::cg::addgroup",
                        (Tcl_ObjCmdProc *) cg_addgroup_cmd, 
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "::cg::removegroup",
                        (Tcl_ObjCmdProc *) cg_removegroup_cmd, 
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "::cg::groupmembers",
                        (Tcl_ObjCmdProc *) cg_groupmembers_cmd, 
                        (ClientData) NULL, NULL);
    
    // Shared data commands
    Tcl_CreateObjCommand(interp, "::cg::share::set",
                        (Tcl_ObjCmdProc *) cg_share_set_cmd, 
                        (ClientData) NULL, NULL);
    Tcl_CreateObjCommand(interp, "::cg::share::get",
                        (Tcl_ObjCmdProc *) cg_share_get_cmd, 
                        (ClientData) NULL, NULL);
    
    // Create convenience aliases
    Tcl_Eval(interp, R"tcl(
        # Help command
        proc ::cg::help {} {
            return "CGraph Manager Commands:
  cg::list                  - List all graph names
  cg::send name script      - Send script to named graph
  cg::broadcast script      - Send script to all graphs
  cg::group tag script      - Send script to group members
  cg::addgroup name tag     - Add graph to group
  cg::removegroup name tag  - Remove graph from group
  cg::groupmembers tag      - List graphs in group
  cg::share::set key val    - Set shared data
  cg::share::get key        - Get shared data"
        }
    )tcl");
    
    return TCL_OK;
}