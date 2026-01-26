#ifndef DGEXPORT_H
#define DGEXPORT_H

#include <cstdio>
#include <string>

#include <df.h>
#include <dynio.h>

class DgExport {
public:
    struct Options {
        char delimiter;
        bool includeHeader;
        int floatPrecision;
        bool prettyJson;
        
        Options() : delimiter('\t'), includeHeader(true), floatPrecision(6), prettyJson(true) {}
    };
    
    // CSV export
    static std::string toCSV(DYN_GROUP* dg, const char* filename, const Options& opts = Options());
    static std::string toCSVString(DYN_GROUP* dg, const Options& opts = Options());
    static std::string listToCSV(DYN_LIST* dl, const char* filename, const Options& opts = Options());
    
    // JSON export
    static std::string toJSON(DYN_GROUP* dg, const char* filename, const Options& opts = Options());
    static std::string toJSONString(DYN_GROUP* dg, const Options& opts = Options());
    
private:
    static void formatValue(char* buf, size_t bufsize, DYN_LIST* dl, int row, int precision);
};

#endif
