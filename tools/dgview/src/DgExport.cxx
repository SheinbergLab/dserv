/**
 * DgExport.cxx - Export DYN_GROUP data to various formats
 */

#include "DgExport.h"
#include "nlohmann/json.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>

using json = nlohmann::json;

// Forward declaration for recursive nested list handling
static json listToJsonArray(DYN_LIST* dl);

static json listToJsonArray(DYN_LIST* dl) {
    json arr = json::array();
    
    if (!dl) return arr;
    
    int n = DYN_LIST_N(dl);
    
    switch (DYN_LIST_DATATYPE(dl)) {
        case DF_LONG: {
            int* vals = (int*)DYN_LIST_VALS(dl);
            for (int i = 0; i < n; i++) {
                arr.push_back(vals[i]);
            }
            break;
        }
        case DF_SHORT: {
            short* vals = (short*)DYN_LIST_VALS(dl);
            for (int i = 0; i < n; i++) {
                arr.push_back(vals[i]);
            }
            break;
        }
        case DF_FLOAT: {
            float* vals = (float*)DYN_LIST_VALS(dl);
            for (int i = 0; i < n; i++) {
                float v = vals[i];
                if (std::isnan(v) || std::isinf(v)) {
                    arr.push_back(nullptr);  // JSON doesn't support NaN/Inf
                } else {
                    arr.push_back(v);
                }
            }
            break;
        }
        case DF_CHAR: {
            char* vals = (char*)DYN_LIST_VALS(dl);
            for (int i = 0; i < n; i++) {
                arr.push_back((int)vals[i]);
            }
            break;
        }
        case DF_STRING: {
            char** vals = (char**)DYN_LIST_VALS(dl);
            for (int i = 0; i < n; i++) {
                arr.push_back(vals[i] ? vals[i] : "");
            }
            break;
        }
        case DF_LIST: {
            DYN_LIST** vals = (DYN_LIST**)DYN_LIST_VALS(dl);
            for (int i = 0; i < n; i++) {
                arr.push_back(listToJsonArray(vals[i]));
            }
            break;
        }
    }
    
    return arr;
}

std::string DgExport::toJSON(DYN_GROUP* dg, const char* filename, const Options& opts) {
    if (!dg) return "No data to export";
    if (!filename || !filename[0]) return "No filename specified";
    
    json j;
    j["name"] = DYN_GROUP_NAME(dg);
    
    json lists = json::object();
    for (int i = 0; i < DYN_GROUP_N(dg); i++) {
        DYN_LIST* dl = DYN_GROUP_LIST(dg, i);
        lists[DYN_LIST_NAME(dl)] = listToJsonArray(dl);
    }
    j["lists"] = lists;
    
    std::ofstream out(filename);
    if (!out) return std::string("Could not open file: ") + filename;
    
    out << j.dump(opts.prettyJson ? 2 : -1);
    
    if (!out.good()) return "Error writing to file";
    
    return "";  // Success
}

std::string DgExport::toJSONString(DYN_GROUP* dg, const Options& opts) {
    if (!dg) return "";
    
    json j;
    j["name"] = DYN_GROUP_NAME(dg);
    
    json lists = json::object();
    for (int i = 0; i < DYN_GROUP_N(dg); i++) {
        DYN_LIST* dl = DYN_GROUP_LIST(dg, i);
        lists[DYN_LIST_NAME(dl)] = listToJsonArray(dl);
    }
    j["lists"] = lists;
    
    return j.dump(opts.prettyJson ? 2 : -1);
}

// ---- CSV methods unchanged ----

std::string DgExport::toCSV(DYN_GROUP* dg, const char* filename, const Options& opts) {
    if (!dg) return "No data to export";
    if (!filename || !filename[0]) return "No filename specified";
    
    FILE* fp = fopen(filename, "w");
    if (!fp) return std::string("Could not open file: ") + filename;
    
    int numCols = DYN_GROUP_N(dg);
    int maxRows = 0;
    for (int i = 0; i < numCols; i++) {
        int n = DYN_LIST_N(DYN_GROUP_LIST(dg, i));
        if (n > maxRows) maxRows = n;
    }
    
    // Header
    if (opts.includeHeader) {
        for (int c = 0; c < numCols; c++) {
            if (c > 0) fputc(opts.delimiter, fp);
            fprintf(fp, "%s", DYN_LIST_NAME(DYN_GROUP_LIST(dg, c)));
        }
        fputc('\n', fp);
    }
    
    // Data
    char buf[1024];
    for (int r = 0; r < maxRows; r++) {
        for (int c = 0; c < numCols; c++) {
            if (c > 0) fputc(opts.delimiter, fp);
            
            DYN_LIST* dl = DYN_GROUP_LIST(dg, c);
            if (r < DYN_LIST_N(dl)) {
                formatValue(buf, sizeof(buf), dl, r, opts.floatPrecision);
                
                // Quote strings containing delimiter or newline
                if (DYN_LIST_DATATYPE(dl) == DF_STRING) {
                    if (strchr(buf, opts.delimiter) || strchr(buf, '\n') || strchr(buf, '"')) {
                        fputc('"', fp);
                        for (const char* p = buf; *p; p++) {
                            if (*p == '"') fputc('"', fp);
                            fputc(*p, fp);
                        }
                        fputc('"', fp);
                    } else {
                        fputs(buf, fp);
                    }
                } else {
                    fputs(buf, fp);
                }
            }
        }
        fputc('\n', fp);
    }
    
    fclose(fp);
    return "";
}

std::string DgExport::toCSVString(DYN_GROUP* dg, const Options& opts) {
    if (!dg) return "";
    
    std::string result;
    
    int numCols = DYN_GROUP_N(dg);
    int maxRows = 0;
    for (int i = 0; i < numCols; i++) {
        int n = DYN_LIST_N(DYN_GROUP_LIST(dg, i));
        if (n > maxRows) maxRows = n;
    }
    
    if (opts.includeHeader) {
        for (int c = 0; c < numCols; c++) {
            if (c > 0) result += opts.delimiter;
            result += DYN_LIST_NAME(DYN_GROUP_LIST(dg, c));
        }
        result += '\n';
    }
    
    char buf[1024];
    for (int r = 0; r < maxRows; r++) {
        for (int c = 0; c < numCols; c++) {
            if (c > 0) result += opts.delimiter;
            
            DYN_LIST* dl = DYN_GROUP_LIST(dg, c);
            if (r < DYN_LIST_N(dl)) {
                formatValue(buf, sizeof(buf), dl, r, opts.floatPrecision);
                result += buf;
            }
        }
        result += '\n';
    }
    
    return result;
}

std::string DgExport::listToCSV(DYN_LIST* dl, const char* filename, const Options& opts) {
    if (!dl) return "No data to export";
    if (!filename || !filename[0]) return "No filename specified";
    
    FILE* fp = fopen(filename, "w");
    if (!fp) return std::string("Could not open file: ") + filename;
    
    if (opts.includeHeader) {
        fprintf(fp, "%s\n", DYN_LIST_NAME(dl));
    }
    
    char buf[1024];
    for (int r = 0; r < DYN_LIST_N(dl); r++) {
        formatValue(buf, sizeof(buf), dl, r, opts.floatPrecision);
        fprintf(fp, "%s\n", buf);
    }
    
    fclose(fp);
    return "";
}

void DgExport::formatValue(char* buf, size_t bufsize, DYN_LIST* dl, int row, int precision) {
    buf[0] = '\0';
    if (!dl || row >= DYN_LIST_N(dl)) return;
    
    switch (DYN_LIST_DATATYPE(dl)) {
        case DF_LONG: {
            int* vals = (int*)DYN_LIST_VALS(dl);
            snprintf(buf, bufsize, "%d", vals[row]);
            break;
        }
        case DF_SHORT: {
            short* vals = (short*)DYN_LIST_VALS(dl);
            snprintf(buf, bufsize, "%d", vals[row]);
            break;
        }
        case DF_FLOAT: {
            float* vals = (float*)DYN_LIST_VALS(dl);
            float v = vals[row];
            if (std::isnan(v)) {
                snprintf(buf, bufsize, "NaN");
            } else if (std::isinf(v)) {
                snprintf(buf, bufsize, v > 0 ? "Inf" : "-Inf");
            } else {
                snprintf(buf, bufsize, "%.*g", precision, v);
            }
            break;
        }
        case DF_CHAR: {
            char* vals = (char*)DYN_LIST_VALS(dl);
            snprintf(buf, bufsize, "%d", (int)vals[row]);
            break;
        }
        case DF_STRING: {
            char** vals = (char**)DYN_LIST_VALS(dl);
            snprintf(buf, bufsize, "%s", vals[row] ? vals[row] : "");
            break;
        }
        case DF_LIST: {
            DYN_LIST** vals = (DYN_LIST**)DYN_LIST_VALS(dl);
            DYN_LIST* nested = vals[row];
            snprintf(buf, bufsize, "[nested: %d]", nested ? DYN_LIST_N(nested) : 0);
            break;
        }
    }
}
