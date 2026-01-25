/**
 * DgExport.cxx - Export DYN_GROUP data to various formats
 */

#include "DgExport.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

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
                        // Escape quotes and wrap in quotes
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
    return "";  // Success
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
    
    // Header
    if (opts.includeHeader) {
        for (int c = 0; c < numCols; c++) {
            if (c > 0) result += opts.delimiter;
            result += DYN_LIST_NAME(DYN_GROUP_LIST(dg, c));
        }
        result += '\n';
    }
    
    // Data
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
    
    // Header
    if (opts.includeHeader) {
        fprintf(fp, "%s\n", DYN_LIST_NAME(dl));
    }
    
    // Data
    char buf[1024];
    for (int r = 0; r < DYN_LIST_N(dl); r++) {
        formatValue(buf, sizeof(buf), dl, r, opts.floatPrecision);
        fprintf(fp, "%s\n", buf);
    }
    
    fclose(fp);
    return "";
}

std::string DgExport::toJSON(DYN_GROUP* dg, const char* filename, const Options& opts) {
    if (!dg) return "No data to export";
    if (!filename || !filename[0]) return "No filename specified";
    
    FILE* fp = fopen(filename, "w");
    if (!fp) return std::string("Could not open file: ") + filename;
    
    bool pretty = opts.prettyJson;
    
    fprintf(fp, "{");
    writeIndent(fp, 1, pretty);
    
    // Group name
    fprintf(fp, "\"name\": \"%s\",", DYN_GROUP_NAME(dg));
    writeIndent(fp, 1, pretty);
    fprintf(fp, "\"lists\": {");
    
    int numCols = DYN_GROUP_N(dg);
    for (int c = 0; c < numCols; c++) {
        DYN_LIST* dl = DYN_GROUP_LIST(dg, c);
        
        writeIndent(fp, 2, pretty);
        fprintf(fp, "\"%s\": {", DYN_LIST_NAME(dl));
        
        writeIndent(fp, 3, pretty);
        const char* typeStr;
        switch (DYN_LIST_DATATYPE(dl)) {
            case DF_LONG:   typeStr = "int";    break;
            case DF_SHORT:  typeStr = "short";  break;
            case DF_FLOAT:  typeStr = "float";  break;
            case DF_CHAR:   typeStr = "char";   break;
            case DF_STRING: typeStr = "string"; break;
            case DF_LIST:   typeStr = "list";   break;
            default:        typeStr = "unknown"; break;
        }
        fprintf(fp, "\"type\": \"%s\",", typeStr);
        
        writeIndent(fp, 3, pretty);
        fprintf(fp, "\"count\": %d,", DYN_LIST_N(dl));
        
        writeIndent(fp, 3, pretty);
        fprintf(fp, "\"values\": [");
        
        int n = DYN_LIST_N(dl);
        for (int r = 0; r < n; r++) {
            if (r > 0) fprintf(fp, ",");
            if (pretty && n > 10) writeIndent(fp, 4, pretty);
            writeJsonValue(fp, dl, r, 4, pretty);
        }
        
        if (pretty && n > 10) writeIndent(fp, 3, pretty);
        fprintf(fp, "]");
        
        writeIndent(fp, 2, pretty);
        fprintf(fp, "}%s", (c < numCols - 1) ? "," : "");
    }
    
    writeIndent(fp, 1, pretty);
    fprintf(fp, "}");
    writeIndent(fp, 0, pretty);
    fprintf(fp, "}\n");
    
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
            snprintf(buf, bufsize, "[nested list: %d elements]", DYN_LIST_N(nested));
            break;
        }
    }
}

void DgExport::writeJsonValue(FILE* fp, DYN_LIST* dl, int row, int indent, bool pretty) {
    if (!dl || row >= DYN_LIST_N(dl)) {
        fprintf(fp, "null");
        return;
    }
    
    switch (DYN_LIST_DATATYPE(dl)) {
        case DF_LONG: {
            int* vals = (int*)DYN_LIST_VALS(dl);
            fprintf(fp, "%d", vals[row]);
            break;
        }
        case DF_SHORT: {
            short* vals = (short*)DYN_LIST_VALS(dl);
            fprintf(fp, "%d", vals[row]);
            break;
        }
        case DF_FLOAT: {
            float* vals = (float*)DYN_LIST_VALS(dl);
            float v = vals[row];
            if (std::isnan(v)) {
                fprintf(fp, "null");  // JSON doesn't have NaN
            } else if (std::isinf(v)) {
                fprintf(fp, "null");  // JSON doesn't have Inf
            } else {
                fprintf(fp, "%g", v);
            }
            break;
        }
        case DF_CHAR: {
            char* vals = (char*)DYN_LIST_VALS(dl);
            fprintf(fp, "%d", (int)vals[row]);
            break;
        }
        case DF_STRING: {
            char** vals = (char**)DYN_LIST_VALS(dl);
            const char* s = vals[row] ? vals[row] : "";
            // Escape JSON string
            fputc('"', fp);
            for (const char* p = s; *p; p++) {
                switch (*p) {
                    case '"':  fprintf(fp, "\\\""); break;
                    case '\\': fprintf(fp, "\\\\"); break;
                    case '\n': fprintf(fp, "\\n");  break;
                    case '\r': fprintf(fp, "\\r");  break;
                    case '\t': fprintf(fp, "\\t");  break;
                    default:   fputc(*p, fp);       break;
                }
            }
            fputc('"', fp);
            break;
        }
        case DF_LIST: {
            DYN_LIST** vals = (DYN_LIST**)DYN_LIST_VALS(dl);
            DYN_LIST* nested = vals[row];
            
            fprintf(fp, "[");
            int n = DYN_LIST_N(nested);
            for (int i = 0; i < n; i++) {
                if (i > 0) fprintf(fp, ", ");
                writeJsonValue(fp, nested, i, indent + 1, false);  // Don't pretty-print nested
            }
            fprintf(fp, "]");
            break;
        }
    }
}

void DgExport::writeIndent(FILE* fp, int indent, bool pretty) {
    if (!pretty) return;
    fputc('\n', fp);
    for (int i = 0; i < indent; i++) {
        fprintf(fp, "  ");
    }
}
