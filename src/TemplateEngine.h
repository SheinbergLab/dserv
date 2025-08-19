#pragma once

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <tcl.h>

class TemplateEngine {
public:
    TemplateEngine();
    ~TemplateEngine();
    
    // Basic variable substitution
    void setVar(const std::string& key, const std::string& value);
    void setVar(const std::string& key, int value);
    void setVar(const std::string& key, bool value);
    
    // Advanced features
    void setConditional(const std::string& key, bool condition);
    void setLoop(const std::string& key, const std::vector<std::map<std::string, std::string>>& items);
    
    // Tcl integration
    void setTclInterpreter(Tcl_Interp* interp) { tclInterp = interp; }
    
    // Template rendering
    std::string render(const std::string& templatePath);
    std::string renderString(const std::string& templateContent);
    
    // Utility functions
    static std::string escapeHtml(const std::string& input);
    
private:
    std::map<std::string, std::string> variables;
    std::map<std::string, bool> conditionals;
    std::map<std::string, std::vector<std::map<std::string, std::string>>> loops;
    Tcl_Interp* tclInterp;
    
    // Template processing functions
    std::string processVariables(const std::string& content);
    std::string processConditionals(const std::string& content);
    std::string processLoops(const std::string& content);
    std::string processTclBlocks(const std::string& content);
    
    // Helper functions
    std::string readFile(const std::string& filePath);
    size_t findMatchingBrace(const std::string& content, size_t start, const std::string& openTag, const std::string& closeTag);
};