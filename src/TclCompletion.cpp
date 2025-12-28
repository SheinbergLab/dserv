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

// Helper to check if a pattern is valid for info commands/procs/etc
// Returns false if the pattern contains spaces (would be parsed as multiple args)
static bool isValidPattern(const std::string& pattern) {
    if (pattern.find(' ') != std::string::npos) return false;
    if (pattern.find('\t') != std::string::npos) return false;
    return true;
}

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
// Filename Completion Function
// ============================================

std::vector<std::string> getFilenameCompletions(Tcl_Interp* interp,
                                                 const std::string& partial,
                                                 bool dirsOnly) {
    std::vector<std::string> results;
    
    if (partial.empty()) {
        return results;
    }
    
    // Build glob pattern
    std::string globPattern = partial + "*";
    
    // Build glob command with proper options
    std::string cmd = "glob -nocomplain ";
    if (dirsOnly) {
        cmd += "-types d ";
    }
    cmd += "-- {" + globPattern + "}";
    
    // Execute glob
    if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
        const char* listStr = Tcl_GetStringResult(interp);
        TclListSize objc;
        Tcl_Obj** objv;
        Tcl_Obj* listObj = Tcl_NewStringObj(listStr, -1);
        Tcl_IncrRefCount(listObj);
        
        if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
            for (TclListSize i = 0; i < objc; i++) {
                std::string path = Tcl_GetString(objv[i]);
                
                // Check if this is a directory and add trailing /
                // Use file isdirectory to check
                std::string checkCmd = "file isdirectory {" + path + "}";

                if (Tcl_Eval(interp, checkCmd.c_str()) == TCL_OK) {
                    const char* isDirStr = Tcl_GetStringResult(interp);
                    if (isDirStr && isDirStr[0] == '1') {
                        // It's a directory - add trailing /
                        if (!path.empty() && path[path.length() - 1] != '/') {
                            path += "/";
                        }
                    }
                }
                
                results.push_back(path);
            }
        }
        Tcl_DecrRefCount(listObj);
    }
    
    return results;
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
            
            // Validate index pattern
            std::string indexPattern = indexPartial + "*";
            if (!isValidPattern(indexPattern)) {
                return results;
            }
            
            // Query array indices
            cmd = "array names " + arrayName + " " + indexPattern;
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

    // Check for custom command-specific completion first
    if (ctx.wordIndex > 0 && !ctx.command.empty()) {
        // Try custom completion via ::completion::get_matches
        // Pass full input so proc-based completions can see previous args
        std::string checkCmd = "::completion::get_matches {" + 
                              ctx.command + "} " + 
                              std::to_string(ctx.wordIndex) + 
                              " {" + ctx.partial + "} {" + 
                              actualPartial + "}";
        
        if (Tcl_Eval(interp, checkCmd.c_str()) == TCL_OK) {
            const char* result = Tcl_GetStringResult(interp);
            
            // If non-empty result, we have custom completions
            if (result && result[0] != '\0') {
                TclListSize objc;
                Tcl_Obj** objv;
                Tcl_Obj* listObj = Tcl_NewStringObj(result, -1);
                Tcl_IncrRefCount(listObj);
                
                if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK && objc > 0) {
                    // Build prefix (everything before the word being completed)
                    size_t lastSpace = actualPartial.rfind(' ');
                    std::string commandPrefix = "";
                    if (lastSpace != std::string::npos) {
                        commandPrefix = actualPartial.substr(0, lastSpace + 1);
                    }
                    
                    // Add custom completions with command prefix
                    for (TclListSize i = 0; i < objc; i++) {
                        std::string match = Tcl_GetString(objv[i]);
                        std::string fullCompletion = prefix + commandPrefix + match;
                        if (seen.find(fullCompletion) == seen.end()) {
                            seen.insert(fullCompletion);
                            results.push_back(fullCompletion);
                        }
                    }
                    
                    Tcl_DecrRefCount(listObj);
                    return results;  // Return custom completions
                }
            }
        }
        // If no custom completions or error, fall through to default completion
    }
    
    // Context-aware completion for specific commands
    if (ctx.wordIndex > 0) {  // We're completing an argument, not the command itself
        
        // Use the context's partial (the word being completed) for the pattern
        std::string contextPattern = ctx.partial + "*";
        
        // Skip context-aware completion if the pattern is invalid
        if (!isValidPattern(contextPattern)) {
            return results;
        }
        
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
        
        // 5. Filename completion for file-taking commands
        bool needsFilenameCompletion = false;
        bool dirsOnly = false;
        
        // Commands that take filenames
        if (ctx.command == "source" || ctx.command == "open" || 
            ctx.command == "exec" || ctx.command == "load" ||
            ctx.command == "glob") {
            needsFilenameCompletion = true;
        }
        
        // cd takes directories only
        if (ctx.command == "cd") {
            needsFilenameCompletion = true;
            dirsOnly = true;
        }
        
        // file subcommands that take paths
        if (ctx.command == "file" && ctx.wordIndex >= 2) {
            needsFilenameCompletion = true;
        }
        
        // Also check if the partial looks like a path pattern
        if (!needsFilenameCompletion) {
            if (ctx.partial.length() > 0) {
                // Absolute path
                if (ctx.partial[0] == '/') {
                    needsFilenameCompletion = true;
                }
                // Relative paths
                else if (ctx.partial.length() >= 2) {
                    if ((ctx.partial[0] == '.' && ctx.partial[1] == '/') ||
                        (ctx.partial[0] == '~' && ctx.partial[1] == '/')) {
                        needsFilenameCompletion = true;
                    }
                    else if (ctx.partial.length() >= 3 &&
                             ctx.partial[0] == '.' && ctx.partial[1] == '.' && ctx.partial[2] == '/') {
                        needsFilenameCompletion = true;
                    }
                }
            }
        }

        if (needsFilenameCompletion) {
            auto filenames = getFilenameCompletions(interp, ctx.partial, dirsOnly);
            for (const auto& filename : filenames) {
                addWithCommandPrefix(filename);
            }
            if (!results.empty()) {
                return results;
            }
            // If no filename matches, fall through to general completion
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
            std::string varPattern = varPartial + "*";
            if (isValidPattern(varPattern)) {
                cmd = "info globals " + varPattern;
                if (Tcl_Eval(interp, cmd.c_str()) == TCL_OK) {
                    parseListResult(interp, "$");  // Add $ prefix back
                }
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
        if (isValidPattern(fullPattern)) {
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
                    std::string nsName = Tcl_GetString(nsObjv[i]);
                    // Strip leading ::
                    if (nsName.substr(0, 2) == "::") {
                        nsName = nsName.substr(2);
                    }
                    
                    // Add the namespace itself as a completion option
                    std::string nsCompletion = prefix + nsName + "::";
                    if (seen.find(nsCompletion) == seen.end()) {
                        seen.insert(nsCompletion);
                        results.push_back(nsCompletion);
                    }
                }
            }
            Tcl_DecrRefCount(nsListObj);
        }
    }
    
    return results;
}

// Tcl command callback: complete <partial>
// Returns Tcl list of completion candidates (full replacement text)
int TclCompleteCmd(ClientData clientData, Tcl_Interp* interp, 
                   int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "partial");
        return TCL_ERROR;
    }
    
    std::string partial = Tcl_GetString(objv[1]);
    std::vector<std::string> completions = getCompletions(interp, partial);
    
    // Return as Tcl list
    Tcl_Obj* resultList = Tcl_NewListObj(0, NULL);
    for (const auto& completion : completions) {
        Tcl_ListObjAppendElement(interp, resultList, 
                                 Tcl_NewStringObj(completion.c_str(), -1));
    }
    Tcl_SetObjResult(interp, resultList);
    
    return TCL_OK;
}

// ============================================
// Token-only completion (for editors)
// ============================================

std::vector<std::string> getCompletionTokens(Tcl_Interp* interp, const std::string& partial) {
    std::vector<std::string> tokens;
    
    // Get full completions first
    std::vector<std::string> fullCompletions = getCompletions(interp, partial);
    
    if (fullCompletions.empty()) {
        return tokens;
    }
    
    // Determine what kind of completion this is
    bool isEmbedded = (partial.rfind('[') != std::string::npos);
    size_t parenPos = partial.rfind('(');
    bool isArraySubscript = false;
    if (parenPos != std::string::npos) {
        size_t closePos = partial.find(')', parenPos);
        if (closePos == std::string::npos) {
            // We're completing array subscript
            isArraySubscript = true;
        }
    }
    
    // Extract tokens from full completions
    for (const auto& full : fullCompletions) {
        std::string token;
        
        if (isArraySubscript) {
            // For array subscripts: "tcl_platform(os)" -> "os)"
            // The full completion already includes arrayName(index)
            // We want just index)
            size_t fullParenPos = full.rfind('(');
            if (fullParenPos != std::string::npos) {
                token = full.substr(fullParenPos + 1);  // Everything after (
            } else {
                token = full;  // Shouldn't happen, but be safe
            }
        } else if (isEmbedded) {
            // For embedded commands: "[dl_fromto" -> "dl_fromto"
            // Strip the leading [
            size_t fullEmbedPos = full.rfind('[');
            if (fullEmbedPos != std::string::npos) {
                token = full.substr(fullEmbedPos + 1);  // Everything after [
            } else {
                token = full;
            }
        } else {
            // Normal case: "set tcl_platform" -> "tcl_platform"
            // Extract last space-separated word
            size_t lastSpace = full.rfind(' ');
            if (lastSpace != std::string::npos) {
                token = full.substr(lastSpace + 1);
            } else {
                token = full;
            }
        }
        
        tokens.push_back(token);
    }
    
    return tokens;
}

// New Tcl command: complete_token <partial>
// Returns just the token to insert (for editor use)
int TclCompleteTokenCmd(ClientData clientData, Tcl_Interp* interp, 
                        int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "partial");
        return TCL_ERROR;
    }
    
    std::string partial = Tcl_GetString(objv[1]);
    std::vector<std::string> tokens = getCompletionTokens(interp, partial);
    
    // Return as Tcl list
    Tcl_Obj* resultList = Tcl_NewListObj(0, NULL);
    for (const auto& token : tokens) {
        Tcl_ListObjAppendElement(interp, resultList, 
                                 Tcl_NewStringObj(token.c_str(), -1));
    }
    Tcl_SetObjResult(interp, resultList);
    
    return TCL_OK;
}
  
// ============================================
// Registration Helper
// ============================================

void RegisterCompletionCommands(Tcl_Interp* interp) {
    // Register the 'complete' command
    Tcl_CreateObjCommand(interp, "complete", TclCompleteCmd, NULL, NULL);

    Tcl_CreateObjCommand(interp, "complete_token", TclCompleteTokenCmd, NULL, NULL);
 
    // Initialize custom completion namespace
    const char* completion_namespace = R"TCL(
# Custom argument completion system
namespace eval ::completion {
    variable rules
    array set rules {}
    
    # Register completion rule for a command
    # Usage:
    #   completion::register ess::load_system {
    #       {literal {tcp udp serial}}
    #   }
    # Or:
    #   completion::register ess::load_system 1 {datapoint ess/systems}
    proc register {cmd_name args} {
        variable rules
        
        if {[llength $args] == 1} {
            set specs [lindex $args 0]
            set arg_pos 1
            foreach spec $specs {
                set rules($cmd_name,$arg_pos) $spec
                incr arg_pos
            }
        } elseif {[llength $args] == 2} {
            lassign $args arg_pos spec
            set rules($cmd_name,$arg_pos) $spec
        } else {
            error "Usage: completion::register cmd {spec1 spec2...} OR completion::register cmd pos spec"
        }
    }
    
    # Get completion matches for a command argument
    # full_input is the complete command line being typed
    proc get_matches {cmd_name arg_pos partial {full_input ""}} {
        variable rules
        
        if {![info exists rules($cmd_name,$arg_pos)]} {
            return {}
        }
        
        set spec $rules($cmd_name,$arg_pos)
        set type [lindex $spec 0]
        
        switch $type {
            datapoint {
                set dp_name [lindex $spec 1]
                if {[catch {dservGet $dp_name} value]} {
                    return {}
                }
                set matches {}
                foreach item $value {
                    if {[string match ${partial}* $item]} {
                        lappend matches $item
                    }
                }
                return $matches
            }            
            literal {
                set values [lindex $spec 1]
                set matches {}
                foreach item $values {
                    if {[string match ${partial}* $item]} {
                        lappend matches $item
                    }
                }
                return $matches
            }
            
            proc {
                set proc_name [lindex $spec 1]
                # DEBUG
                # puts stderr "DEBUG get_matches: full_input='$full_input' partial='$partial'"
                
                # Parse full_input to get previous arguments
                set prev_args {}
                if {$full_input ne ""} {
                    # Simple word split (good enough for most cases)
                    set words [regexp -all -inline {\S+} $full_input]
                    # DEBUG
                    # puts stderr "DEBUG words='$words'"
                    
                    # First word is command, rest up to current position are prev args
                    # If we're at position N, we want args 1..(N-1)
                    if {[llength $words] > 1} {
                        set prev_args [lrange $words 1 end]
                    }
                }
                # DEBUG
                # puts stderr "DEBUG prev_args='$prev_args'"
                
                # Call proc with: prev_args partial
                # Proc signature: proc name {prev_args partial}
                if {[catch {$proc_name $prev_args $partial} matches]} {
                    return {}
                }
                return $matches
            }
            
            glob {
                set pattern [lindex $spec 1]
                set full_pattern ${partial}*
                if {[catch {glob -nocomplain $full_pattern} matches]} {
                    return {}
                }
                return $matches
            }
            
            range {
                lassign [lrange $spec 1 end] min max step
                return [list "${min}..${max}"]
            }
            
            default {
                return {}
            }
        }
    }
    
    # List all registered commands (for debugging)
    proc list_rules {} {
        variable rules
        set result {}
        foreach key [lsort [array names rules]] {
            lassign [split $key ,] cmd pos
            lappend result "$cmd arg$pos: $rules($key)"
        }
        return $result
    }
    
    # Clear all rules
    proc clear {} {
        variable rules
        array unset rules *
    }
}
)TCL";
    
    // Evaluate the completion namespace (ignore errors - may already exist)
    Tcl_Eval(interp, completion_namespace);
}

} // namespace TclCompletion
