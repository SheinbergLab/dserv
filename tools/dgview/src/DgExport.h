#ifndef DGEXPORT_H
#define DGEXPORT_H

/**
 * DgExport - Export DYN_GROUP data to various formats
 * 
 * Supports:
 * - CSV (tab or comma separated)
 * - JSON (full structure)
 */

#include <cstdio>
#include <string>

#include <df.h>
#include <dynio.h>

class DgExport {
public:
    // Export options
    struct Options {
        char delimiter;
        bool includeHeader;
        int floatPrecision;
        bool prettyJson;
        
        Options() : delimiter('\t'), includeHeader(true), floatPrecision(6), prettyJson(true) {}
    };
    
    // Export to CSV file
    // Returns empty string on success, error message on failure
    static std::string toCSV(DYN_GROUP* dg, const char* filename, const Options& opts = Options());
    
    // Export to JSON file
    static std::string toJSON(DYN_GROUP* dg, const char* filename, const Options& opts = Options());
    
    // Export single DYN_LIST to CSV
    static std::string listToCSV(DYN_LIST* dl, const char* filename, const Options& opts = Options());
    
    // Get CSV as string (for clipboard)
    static std::string toCSVString(DYN_GROUP* dg, const Options& opts = Options());
    
private:
    static void formatValue(char* buf, size_t bufsize, DYN_LIST* dl, int row, int precision);
    static void writeJsonValue(FILE* fp, DYN_LIST* dl, int row, int indent, bool pretty);
    static void writeIndent(FILE* fp, int indent, bool pretty);
};

#endif // DGEXPORT_H
