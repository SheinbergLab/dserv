// TclCompletion.cpp
// Standalone Tcl tab completion library

#include "TclCompletion.h"
#include <set>

// Tcl 8.6 vs 9.0 compatibility
#if TCL_MAJOR_VERSION >= 9
    typedef Tcl_Size TclListSize;
#else
    typedef int TclListSize;
#endif

namespace TclCompletion {

// Helper to extract command context from partial input
struct CompletionContext {
    std::string command;      // First word (the command)
    std::string partial;      // What we're completing
    int wordIndex;            // Which word are we completing (0=command, 1=first arg, etc.)
};

static CompletionContext parseContext(const std::string& input) {
    CompletionContext ctx;
    ctx.wordIndex = 0;
    
    // Simple word splitting - good enough for most cases
    std::vector<std::string> words;
    std::string current;
    bool inBraces = false;
    bool inQuotes = false;
    
    for (char ch : input) {
        if (ch == '{' && !inQuotes) {
            inBraces = true;
            current += ch;
        } else if (ch == '}' && !inQuotes) {
            inBraces = false;
            current += ch;
        } else if (ch == '"' && !inBraces) {
            inQuotes = !inQuotes;
            current += ch;
        } else if ((ch == ' ' || ch == '\t') && !inBraces && !inQuotes) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        } else {
            current += ch;
        }
    }
    
    // The last (incomplete) word is what we're completing
    ctx.partial = current;
    ctx.wordIndex = words.size();
    
    // First word is the command (if we have any complete words)
    if (!words.empty()) {
        ctx.command = words[0];
    }
    
    return ctx;
}

// ============================================
// Core Completion Function
// ============================================

std::vector<std::string> getCompletions(Tcl_Interp* interp, const std::string& partial) {
    std::vector<std::string> results;
    std::set<std::string> seen;  // for deduplication
    
    // Check if we're completing inside an embedded command: [...
    // Examples: {format "filename_[ex"} or "test [info c" or [string ma
    std::string actualPartial = partial;
    std::string prefix = "";
    size_t embedPos = partial.rfind('[');
    
    if (embedPos != std::string::npos) {
        // Check if there's a closing ] after the [
        size_t closePos = partial.find(']', embedPos);
        if (closePos == std::string::npos) {
            // No closing ] - we're completing inside [...]
            prefix = partial.substr(0, embedPos + 1);  // Everything up to and including [
            actualPartial = partial.substr(embedPos + 1);  // The command after [
            
            // Trim whitespace from actualPartial
            size_t firstNonSpace = actualPartial.find_first_not_of(" \t");
            if (firstNonSpace != std::string::npos) {
                actualPartial = actualPartial.substr(firstNonSpace);
            }
        }
    }
    
    // Helper to parse Tcl list results
    auto parseListResult = [&](Tcl_Interp* interp, const std::string& itemPrefix = "") {
        const char* listStr = Tcl_GetStringResult(interp);
        TclListSize objc;
        Tcl_Obj** objv;
        Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
        Tcl_IncrRefCount(listObj);
        
        if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
            for (TclListSize i = 0; i < objc; i++) {
                std::string item = itemPrefix + Tcl_GetString(objv[i]);
                // Apply the prefix (everything before the word being completed)
                std::string fullItem = prefix + item;
                if (seen.find(fullItem) == seen.end()) {
                    seen.insert(fullItem);
                    results.push_back(fullItem);
                }
            }
        }
        Tcl_DecrRefCount(listObj);
    };
    
    std::string pattern = actualPartial + "*";
    std::string cmd;
    
    // Check for array element completion: varName(partial
    // Example: tcl_platform(ma -> tcl_platform(machine)
    size_t parenPos = actualPartial.rfind('(');
    if (parenPos != std::string::npos) {
        // Check if there's a closing paren
        size_t closePos = actualPartial.find(')', parenPos);
        if (closePos == std::string::npos) {
            // No closing paren - completing array element
            std::string arrayName = actualPartial.substr(0, parenPos);
            std::string indexPartial = actualPartial.substr(parenPos + 1);
            
            // Strip any leading/trailing whitespace from array name
            size_t nameStart = arrayName.find_last_of(" \t");
            if (nameStart != std::string::npos) {
                arrayName = arrayName.substr(nameStart + 1);
            }
            
            // Query array indices
            cmd = "array names " + arrayName + " " + indexPartial + "*";
            if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
                const char* listStr = Tcl_GetStringResult(interp);
                TclListSize objc;
                Tcl_Obj** objv;
                Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
                Tcl_IncrRefCount(listObj);
                
                if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
                    // Build prefix (everything before the array element)
                    std::string beforeArray = actualPartial.substr(0, parenPos - arrayName.length());
                    
                    for (TclListSize i = 0; i < objc; i++) {
                        std::string indexName = Tcl_GetString(objv[i]);
                        // Build full completion: prefix + arrayName + ( + index + )
                        std::string fullCompletion = prefix + beforeArray + arrayName + "(" + indexName + ")";
                        if (seen.find(fullCompletion) == seen.end()) {
                            seen.insert(fullCompletion);
                            results.push_back(fullCompletion);
                        }
                    }
                }
                Tcl_DecrRefCount(listObj);
            }
            
            return results;  // Return array element completions
        }
    }
    
    // Parse context to see if we should do context-aware completion
    CompletionContext ctx = parseContext(actualPartial);
    
    // Context-aware completion for specific commands
    if (ctx.wordIndex > 0) {  // We're completing an argument, not the command itself
        
        // Use the context's partial (the word being completed) for the pattern
        std::string contextPattern = ctx.partial + "*";
        
        // Build prefix (everything before the word being completed)
        // For "set tcl_pl", prefix should be "set "
        size_t lastSpace = actualPartial.rfind(' ');
        std::string commandPrefix = "";
        if (lastSpace != std::string::npos) {
            commandPrefix = actualPartial.substr(0, lastSpace + 1);
        }
        
        // Helper to add results with command prefix
        auto addWithCommandPrefix = [&](const std::string& match) {
            std::string fullCompletion = prefix + commandPrefix + match;
            if (seen.find(fullCompletion) == seen.end()) {
                seen.insert(fullCompletion);
                results.push_back(fullCompletion);
            }
        };
        
        // 1. Variable names after set/unset/global/variable/upvar
        if (ctx.command == "set" || ctx.command == "unset" || 
            ctx.command == "global" || ctx.command == "variable" || 
            ctx.command == "upvar") {
            
            // Use info vars for variable completion (without $)
            cmd = "info vars " + contextPattern;
            if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
                const char* listStr = Tcl_GetStringResult(interp);
                TclListSize objc;
                Tcl_Obj** objv;
                Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
                Tcl_IncrRefCount(listObj);
                
                if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
                    for (TclListSize i = 0; i < objc; i++) {
                        addWithCommandPrefix(Tcl_GetString(objv[i]));
                    }
                }
                Tcl_DecrRefCount(listObj);
            }
            
            // Also try info globals for broader scope
            cmd = "info globals " + contextPattern;
            if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
                const char* listStr = Tcl_GetStringResult(interp);
                TclListSize objc;
                Tcl_Obj** objv;
                Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
                Tcl_IncrRefCount(listObj);
                
                if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
                    for (TclListSize i = 0; i < objc; i++) {
                        addWithCommandPrefix(Tcl_GetString(objv[i]));
                    }
                }
                Tcl_DecrRefCount(listObj);
            }
            
            return results;  // Only return variable names
        }
        
        // 2. Namespace names after "namespace eval"
        if (ctx.command == "namespace" && ctx.wordIndex == 2) {
            cmd = "namespace children :: " + contextPattern;
            if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
                const char* listStr = Tcl_GetStringResult(interp);
                TclListSize objc;
                Tcl_Obj** objv;
                Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
                Tcl_IncrRefCount(listObj);
                
                if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
                    for (TclListSize i = 0; i < objc; i++) {
                        std::string ns = Tcl_GetString(objv[i]);
                        // Strip leading ::
                        if (ns.substr(0, 2) == "::") {
                            ns = ns.substr(2);
                        }
                        addWithCommandPrefix(ns);
                    }
                }
                Tcl_DecrRefCount(listObj);
            }
            return results;
        }
        
        // 3. Array names after "array" command
        if (ctx.command == "array" && ctx.wordIndex >= 2) {
            cmd = "info vars " + contextPattern;
            if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
                const char* listStr = Tcl_GetStringResult(interp);
                TclListSize objc;
                Tcl_Obj** objv;
                Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
                Tcl_IncrRefCount(listObj);
                
                if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
                    for (TclListSize i = 0; i < objc; i++) {
                        std::string varName = Tcl_GetString(objv[i]);
                        // Check if it's an array
                        std::string checkCmd = "array exists " + varName;
                        if (Tcl_Eval(interp, checkCmd.c_str()) == TCL_OK) {
                            const char* result = Tcl_GetStringResult(interp);
                            if (result && result[0] == '1') {
                                addWithCommandPrefix(varName);
                            }
                        }
                    }
                }
                Tcl_DecrRefCount(listObj);
            }
            return results;
        }
        
        // 4. Proc names after "proc" (for editing/overwriting)
        if (ctx.command == "proc" && ctx.wordIndex == 1) {
            cmd = "info procs " + contextPattern;
            if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
                const char* listStr = Tcl_GetStringResult(interp);
                TclListSize objc;
                Tcl_Obj** objv;
                Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
                Tcl_IncrRefCount(listObj);
                
                if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
                    for (TclListSize i = 0; i < objc; i++) {
                        addWithCommandPrefix(Tcl_GetString(objv[i]));
                    }
                }
                Tcl_DecrRefCount(listObj);
            }
            return results;
        }
    }
    
    // If no context-specific completion, continue with general completion
    
    // Check if this is a namespace-qualified completion
    size_t colonPos = actualPartial.rfind("::");
    bool isNamespaceQualified = (colonPos != std::string::npos);
    
    // Only do global searches if NOT namespace-qualified
    if (!isNamespaceQualified) {
        // 1. Query commands: "info commands partial*"
        cmd = "info commands " + pattern;
        if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
            parseListResult(interp);
        }
        
        // 2. Query procs: "info procs partial*"
        cmd = "info procs " + pattern;
        if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
            parseListResult(interp);
        }
        
        // 3. If starts with $, query global variables
        if (!actualPartial.empty() && actualPartial[0] == '$') {
            std::string varPartial = actualPartial.substr(1);  // Remove $
            cmd = "info globals " + varPartial + "*";
            if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
                parseListResult(interp, "$");  // Add $ prefix back
            }
        }
    }
    
    // 4. Handle namespace-qualified completions
    // If partial contains ::, try to complete within that namespace
    if (colonPos != std::string::npos) {
        std::string ns = actualPartial.substr(0, colonPos);
        std::string namePartial = actualPartial.substr(colonPos + 2);
        
        // Query from global namespace with full pattern to avoid imported commands
        // Use ::ns::partial* to only get commands actually in that namespace
        std::string fullPattern = "::" + ns + "::" + namePartial + "*";
        cmd = "info commands " + fullPattern;
        if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
            const char* listStr = Tcl_GetStringResult(interp);
            TclListSize objc;
            Tcl_Obj** objv;
            Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
            Tcl_IncrRefCount(listObj);
            
            if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
                for (TclListSize i = 0; i < objc; i++) {
                    std::string item = Tcl_GetString(objv[i]);
                    // Strip leading :: if present for consistent output
                    if (item.substr(0, 2) == "::") {
                        item = item.substr(2);
                    }
                    // Apply the prefix
                    std::string fullItem = prefix + item;
                    if (seen.find(fullItem) == seen.end()) {
                        seen.insert(fullItem);
                        results.push_back(fullItem);
                    }
                }
            }
            Tcl_DecrRefCount(listObj);
        }
    }
    
    // 5. Smart child namespace search
    // If partial matches a namespace name, also search commands in that namespace
    // Example: "str" matches "string" namespace → also return "string::map" etc.
    if (colonPos == std::string::npos && !actualPartial.empty()) {
        // Check if any namespace starts with our partial
        cmd = "namespace children :: " + pattern;
        if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
            const char* listStr = Tcl_GetStringResult(interp);
            TclListSize nsObjc;
            Tcl_Obj** nsObjv;
            Tcl_Obj* nsListObj = Tcl_NewStringObj(listStr, -1);
            Tcl_IncrRefCount(nsListObj);
            
            if (Tcl_ListObjGetElements(interp, nsListObj, &nsObjc, &nsObjv) == TCL_OK) {
                for (TclListSize i = 0; i < nsObjc; i++) {
                    std::string fullNs = Tcl_GetString(nsObjv[i]);
                    
                    // Strip leading :: from namespace name
                    std::string ns = fullNs;
                    if (ns.substr(0, 2) == "::") {
                        ns = ns.substr(2);
                    }
                    
                    // Get all commands in this matching namespace
                    std::string nsCmd = "namespace eval " + fullNs + " {info commands}";
                    if (Tcl_Eval(interp, nsCmd.c_str()) == TCL_OK) {
                        const char* cmdListStr = Tcl_GetStringResult(interp);
                        TclListSize cmdObjc;
                        Tcl_Obj** cmdObjv;
                        Tcl_Obj* cmdListObj = Tcl_NewStringObj(cmdListStr, -1);
                        Tcl_IncrRefCount(cmdListObj);
                        
                        if (Tcl_ListObjGetElements(interp, cmdListObj, &cmdObjc, &cmdObjv) == TCL_OK) {
                            // Limit to reasonable number of commands per namespace
                            TclListSize maxCmds = 20;
                            for (TclListSize j = 0; j < cmdObjc && j < maxCmds; j++) {
                                std::string cmdName = ns + "::" + Tcl_GetString(cmdObjv[j]);
                                // Apply the prefix
                                std::string fullItem = prefix + cmdName;
                                if (seen.find(fullItem) == seen.end()) {
                                    seen.insert(fullItem);
                                    results.push_back(fullItem);
                                }
                            }
                        }
                        Tcl_DecrRefCount(cmdListObj);
                    }
                }
            }
            Tcl_DecrRefCount(nsListObj);
        }
    }
    
    return results;
}

// ============================================
// Cross-Interpreter Completion
// ============================================

std::vector<std::string> getRemoteCompletions(Tcl_Interp* mainInterp,
                                               const std::string& targetInterp,
                                               const std::string& partial) {
    std::vector<std::string> results;
    
    // Build command: send $targetInterp {complete $partial}
    // Note: Need to properly escape braces in partial
    std::string cmd = "send " + targetInterp + " {complete {" + partial + "}}";
    
    if (Tcl_Eval(mainInterp, cmd.c_str()) == TCL_OK) {
        const char* listStr = Tcl_GetStringResult(mainInterp);
        TclListSize objc;
        Tcl_Obj** objv;
        Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
        Tcl_IncrRefCount(listObj);
        
        if (Tcl_ListObjGetElements(mainInterp, listObj, &objc, &objv) == TCL_OK) {
            for (TclListSize i = 0; i < objc; i++) {
                results.push_back(Tcl_GetString(objv[i]));
            }
        }
        Tcl_DecrRefCount(listObj);
    }
    
    return results;
}

// ============================================
// Tcl Command: complete <partial>
// ============================================

int TclCompleteCmd(ClientData clientData, Tcl_Interp* interp, 
                   int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "partial");
        return TCL_ERROR;
    }
    
    std::string partial = Tcl_GetString(objv[1]);
    std::vector<std::string> matches = getCompletions(interp, partial);
    
    // Return as Tcl list
    Tcl_Obj* resultList = Tcl_NewListObj(0, NULL);
    for (const auto& match : matches) {
        Tcl_ListObjAppendElement(interp, resultList, 
                                 Tcl_NewStringObj(match.c_str(), -1));
    }
    Tcl_SetObjResult(interp, resultList);
    
    return TCL_OK;
}

// ============================================
// Registration Helper
// ============================================

void RegisterCompletionCommand(Tcl_Interp* interp) {
    Tcl_CreateObjCommand(interp, "complete", TclCompleteCmd, NULL, NULL);
}

} // namespace TclCompletion
