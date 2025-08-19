#include "TemplateEngine.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <iostream>

TemplateEngine::TemplateEngine() : tclInterp(nullptr) {
}

TemplateEngine::~TemplateEngine() {
}

void TemplateEngine::setVar(const std::string& key, const std::string& value) {
    variables[key] = value;
}

void TemplateEngine::setVar(const std::string& key, int value) {
    variables[key] = std::to_string(value);
}

void TemplateEngine::setVar(const std::string& key, bool value) {
    variables[key] = value ? "true" : "false";
}

void TemplateEngine::setConditional(const std::string& key, bool condition) {
    conditionals[key] = condition;
}

void TemplateEngine::setLoop(const std::string& key, const std::vector<std::map<std::string, std::string>>& items) {
    loops[key] = items;
}

std::string TemplateEngine::render(const std::string& templatePath) {
    std::string content = readFile(templatePath);
    if (content.empty()) {
        std::cerr << "Failed to read template file: " << templatePath << std::endl;
        return "";
    }
    return renderString(content);
}

std::string TemplateEngine::renderString(const std::string& templateContent) {
    std::string result = templateContent;
    
    // Process in order: Tcl blocks first (can generate variables), then loops, conditionals, variables
    result = processTclBlocks(result);
    result = processLoops(result);
    result = processConditionals(result);
    result = processVariables(result);
    
    return result;
}

std::string TemplateEngine::processVariables(const std::string& content) {
    std::string result = content;
    
    // Simple variable substitution: {{varname}}
    for (const auto& [key, value] : variables) {
        std::string pattern = "{{" + key + "}}";
        size_t pos = 0;
        while ((pos = result.find(pattern, pos)) != std::string::npos) {
            result.replace(pos, pattern.length(), value);
            pos += value.length();
        }
    }
    
    return result;
}

std::string TemplateEngine::processConditionals(const std::string& content) {
    std::string result = content;
    
    // Process {{#if condition}} ... {{/if}} blocks
    for (const auto& [key, condition] : conditionals) {
        std::string startTag = "{{#if " + key + "}}";
        std::string endTag = "{{/if}}";
        
        size_t pos = 0;
        while ((pos = result.find(startTag, pos)) != std::string::npos) {
            size_t endPos = result.find(endTag, pos + startTag.length());
            if (endPos != std::string::npos) {
                if (condition) {
                    // Keep content, remove tags
                    result.erase(endPos, endTag.length());
                    result.erase(pos, startTag.length());
                    pos += (endPos - pos - startTag.length());
                } else {
                    // Remove entire block
                    result.erase(pos, endPos + endTag.length() - pos);
                }
            } else {
                pos += startTag.length();
            }
        }
    }
    
    return result;
}

std::string TemplateEngine::processLoops(const std::string& content) {
    std::string result = content;
    
    // Process {{#each items}} ... {{/each}} blocks
    for (const auto& [key, items] : loops) {
        std::string startTag = "{{#each " + key + "}}";
        std::string endTag = "{{/each}}";
        
        size_t pos = 0;
        while ((pos = result.find(startTag, pos)) != std::string::npos) {
            size_t endPos = result.find(endTag, pos + startTag.length());
            if (endPos != std::string::npos) {
                std::string loopTemplate = result.substr(pos + startTag.length(), 
                                                        endPos - pos - startTag.length());
                std::string loopResult;
                
                // Render template for each item
                for (const auto& item : items) {
                    std::string itemResult = loopTemplate;
                    // Replace variables in this iteration
                    for (const auto& [itemKey, itemValue] : item) {
                        std::string itemPattern = "{{" + itemKey + "}}";
                        size_t itemPos = 0;
                        while ((itemPos = itemResult.find(itemPattern, itemPos)) != std::string::npos) {
                            itemResult.replace(itemPos, itemPattern.length(), itemValue);
                            itemPos += itemValue.length();
                        }
                    }
                    loopResult += itemResult;
                }
                
                // Replace the entire loop block with the rendered result
                result.replace(pos, endPos + endTag.length() - pos, loopResult);
                pos += loopResult.length();
            } else {
                pos += startTag.length();
            }
        }
    }
    
    return result;
}

std::string TemplateEngine::processTclBlocks(const std::string& content) {
    if (!tclInterp) {
        return content;
    }
    
    std::string result = content;
    std::string startTag = "{{tcl:";
    std::string endTag = "}}";
    
    size_t pos = 0;
    while ((pos = result.find(startTag, pos)) != std::string::npos) {
        size_t endPos = result.find(endTag, pos + startTag.length());
        if (endPos != std::string::npos) {
            std::string tclCode = result.substr(pos + startTag.length(), 
                                              endPos - pos - startTag.length());
            
            // Execute Tcl code
            int evalResult = Tcl_Eval(tclInterp, tclCode.c_str());
            std::string tclResult;
            
            if (evalResult == TCL_OK) {
                const char* resultStr = Tcl_GetStringResult(tclInterp);
                tclResult = resultStr ? resultStr : "";
            } else {
                tclResult = "[TCL ERROR]";
                std::cerr << "Tcl evaluation error: " << Tcl_GetStringResult(tclInterp) << std::endl;
            }
            
            // Replace the Tcl block with the result
            result.replace(pos, endPos + endTag.length() - pos, tclResult);
            pos += tclResult.length();
        } else {
            pos += startTag.length();
        }
    }
    
    return result;
}

std::string TemplateEngine::readFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        return "";
    }
    
    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    file.close();
    return content;
}

std::string TemplateEngine::escapeHtml(const std::string& input) {
    std::string result;
    result.reserve(input.length() * 1.1); // Reserve some extra space
    
    for (char c : input) {
        switch (c) {
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '&': result += "&amp;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&#39;"; break;
            default: result += c; break;
        }
    }
    return result;
}