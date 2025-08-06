// src/tcl/TclUtils.cpp
#include "TclUtils.h"
#include <QMutex>
#include <QMutexLocker>

// Static instance for singleton pattern
TclUtils* TclUtils::s_instance = nullptr;

TclUtils::TclUtils(QObject *parent)
    : QObject(parent)
    , m_tclInterp(nullptr)
{
    initializeInterpreter();
}

TclUtils::~TclUtils()
{
    if (m_tclInterp) {
        Tcl_DeleteInterp(m_tclInterp);
    }
}

TclUtils* TclUtils::instance()
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    
    if (!s_instance) {
        s_instance = new TclUtils();
    }
    return s_instance;
}

bool TclUtils::initializeInterpreter()
{
    // Create Tcl interpreter for parsing
    m_tclInterp = Tcl_CreateInterp();
    if (!m_tclInterp) {
        setError("Failed to create Tcl interpreter");
        return false;
    }
    
    if (Tcl_Init(m_tclInterp) != TCL_OK) {
        QString error = QString("Failed to initialize Tcl interpreter: %1")
                       .arg(Tcl_GetStringResult(m_tclInterp));
        setError(error);
        Tcl_DeleteInterp(m_tclInterp);
        m_tclInterp = nullptr;
        return false;
    }
    
    return true;
}

void TclUtils::setError(const QString &error)
{
    m_lastError = error;
    qWarning() << "TclUtils error:" << error;
}

QMap<QString, QStringList> TclUtils::parseDictToStringLists(const QString &dictStr)
{
    QMap<QString, QStringList> result;
    clearError();

    if (!m_tclInterp) {
        setError("No Tcl interpreter available");
        return result;
    }

    if (dictStr.trimmed().isEmpty()) {
        return result; // Empty dict is valid
    }

    // Create Tcl object from string
    Tcl_Obj *dictObj = Tcl_NewStringObj(dictStr.toUtf8().constData(), -1);
    Tcl_IncrRefCount(dictObj);

    // Verify it's a valid dictionary
    Tcl_Size dictSize;
    if (Tcl_DictObjSize(m_tclInterp, dictObj, &dictSize) != TCL_OK) {
        setError("Invalid Tcl dictionary format");
        Tcl_DecrRefCount(dictObj);
        return result;
    }

    // Iterate through dictionary
    Tcl_DictSearch search;
    Tcl_Obj *keyObj, *valueObj;
    int done;

    if (Tcl_DictObjFirst(m_tclInterp, dictObj, &search, &keyObj, &valueObj, &done) != TCL_OK) {
        setError("Failed to iterate dictionary");
        Tcl_DecrRefCount(dictObj);
        return result;
    }

    while (!done) {
        QString key = extractString(keyObj);
        QStringList valueList = parseList(extractString(valueObj));
        
        result[key] = valueList;
        
        Tcl_DictObjNext(&search, &keyObj, &valueObj, &done);
    }

    Tcl_DictObjDone(&search);
    Tcl_DecrRefCount(dictObj);

    return result;
}

QVariantMap TclUtils::parseDictToVariantMap(const QString &dictStr)
{
    QVariantMap result;
    clearError();

    if (!m_tclInterp) {
        setError("No Tcl interpreter available");
        return result;
    }

    if (dictStr.trimmed().isEmpty()) {
        return result;
    }

    Tcl_Obj *dictObj = Tcl_NewStringObj(dictStr.toUtf8().constData(), -1);
    Tcl_IncrRefCount(dictObj);

    Tcl_Size dictSize;
    if (Tcl_DictObjSize(m_tclInterp, dictObj, &dictSize) != TCL_OK) {
        setError("Invalid Tcl dictionary format");
        Tcl_DecrRefCount(dictObj);
        return result;
    }

    Tcl_DictSearch search;
    Tcl_Obj *keyObj, *valueObj;
    int done;

    if (Tcl_DictObjFirst(m_tclInterp, dictObj, &search, &keyObj, &valueObj, &done) != TCL_OK) {
        setError("Failed to iterate dictionary");
        Tcl_DecrRefCount(dictObj);
        return result;
    }

    while (!done) {
        QString key = extractString(keyObj);
        QVariant value = parseObjectToVariant(valueObj);
        
        result[key] = value;
        
        Tcl_DictObjNext(&search, &keyObj, &valueObj, &done);
    }

    Tcl_DictObjDone(&search);
    Tcl_DecrRefCount(dictObj);

    return result;
}

QStringList TclUtils::parseList(const QString &listStr)
{
    QStringList result;
    clearError();

    if (!m_tclInterp) {
        setError("No Tcl interpreter available");
        return result;
    }

    if (listStr.trimmed().isEmpty()) {
        return result;
    }

    Tcl_Obj *listObj = Tcl_NewStringObj(listStr.toUtf8().constData(), -1);
    Tcl_IncrRefCount(listObj);

    Tcl_Size listLength;
    Tcl_Obj **listElements;

    if (Tcl_ListObjGetElements(m_tclInterp, listObj, &listLength, &listElements) != TCL_OK) {
        setError("Invalid Tcl list format");
        Tcl_DecrRefCount(listObj);
        return result;
    }

    for (int i = 0; i < listLength; i++) {
        QString element = extractString(listElements[i]);
        result.append(element);
    }

    Tcl_DecrRefCount(listObj);
    return result;
}

QList<QStringList> TclUtils::parseNestedList(const QString &listStr)
{
    QList<QStringList> result;
    clearError();

    if (!m_tclInterp) {
        setError("No Tcl interpreter available");
        return result;
    }

    if (listStr.trimmed().isEmpty()) {
        return result;
    }

    // First parse as top-level list
    QStringList topLevel = parseList(listStr);
    
    // Then parse each element as a sublist
    for (const QString &element : topLevel) {
        QStringList subList = parseList(element);
        result.append(subList);
    }

    return result;
}

QString TclUtils::extractString(Tcl_Obj *objPtr)
{
    if (!objPtr) return QString();
    return QString::fromUtf8(Tcl_GetString(objPtr));
}

int TclUtils::extractInt(Tcl_Obj *objPtr, bool *ok)
{
    if (!objPtr || !m_tclInterp) {
        if (ok) *ok = false;
        return 0;
    }

    int value;
    int result = Tcl_GetIntFromObj(m_tclInterp, objPtr, &value);
    if (ok) *ok = (result == TCL_OK);
    return (result == TCL_OK) ? value : 0;
}

double TclUtils::extractDouble(Tcl_Obj *objPtr, bool *ok)
{
    if (!objPtr || !m_tclInterp) {
        if (ok) *ok = false;
        return 0.0;
    }

    double value;
    int result = Tcl_GetDoubleFromObj(m_tclInterp, objPtr, &value);
    if (ok) *ok = (result == TCL_OK);
    return (result == TCL_OK) ? value : 0.0;
}

bool TclUtils::extractBool(Tcl_Obj *objPtr, bool *ok)
{
    if (!objPtr || !m_tclInterp) {
        if (ok) *ok = false;
        return false;
    }

    int value;
    int result = Tcl_GetBooleanFromObj(m_tclInterp, objPtr, &value);
    if (ok) *ok = (result == TCL_OK);
    return (result == TCL_OK) ? (value != 0) : false;
}

QVariant TclUtils::parseObjectToVariant(Tcl_Obj *objPtr)
{
    if (!objPtr) return QVariant();

    // Try different type conversions
    bool ok;
    
    // Try integer first
    int intVal = extractInt(objPtr, &ok);
    if (ok) return intVal;
    
    // Try double
    double doubleVal = extractDouble(objPtr, &ok);
    if (ok) return doubleVal;
    
    // Try boolean
    bool boolVal = extractBool(objPtr, &ok);
    if (ok) return boolVal;
    
    // Try as list
    QString objStr = extractString(objPtr);
    if (isValidList(objStr)) {
        QStringList listVal = parseList(objStr);
        if (listVal.size() > 1 || !listVal.isEmpty()) {
            return listVal;
        }
    }
    
    // Default to string
    return objStr;
}

bool TclUtils::isValidList(const QString &str)
{
    if (!m_tclInterp || str.isEmpty()) return false;
    
    Tcl_Obj *listObj = Tcl_NewStringObj(str.toUtf8().constData(), -1);
    Tcl_IncrRefCount(listObj);
    
    Tcl_Size listLength;
    Tcl_Obj **listElements;
    bool valid = (Tcl_ListObjGetElements(m_tclInterp, listObj, &listLength, &listElements) == TCL_OK);
    
    Tcl_DecrRefCount(listObj);
    return valid;
}

bool TclUtils::isValidDict(const QString &str)
{
    if (!m_tclInterp || str.isEmpty()) return false;
    
    Tcl_Obj *dictObj = Tcl_NewStringObj(str.toUtf8().constData(), -1);
    Tcl_IncrRefCount(dictObj);
    
    Tcl_Size dictSize;
    bool valid = (Tcl_DictObjSize(m_tclInterp, dictObj, &dictSize) == TCL_OK);
    
    Tcl_DecrRefCount(dictObj);
    return valid;
}

QString TclUtils::escapeString(const QString &str)
{
    // Basic Tcl string escaping
    QString escaped = str;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    escaped.replace("\n", "\\n");
    escaped.replace("\t", "\\t");
    return escaped;
}

QString TclUtils::createList(const QStringList &list)
{
    if (list.isEmpty()) return "{}";
    
    QStringList escaped;
    for (const QString &item : list) {
        QString escapedItem = item;
        if (item.contains(' ') || item.contains('\t') || item.contains('\n') || 
            item.contains('{') || item.contains('}') || item.contains('\\')) {
            escapedItem = "{" + escapeString(item) + "}";
        }
        escaped.append(escapedItem);
    }
    
    return escaped.join(" ");
}

QString TclUtils::createDict(const QVariantMap &map)
{
    if (map.isEmpty()) return "{}";
    
    QStringList pairs;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        QString key = it.key();
        QVariant value = it.value();
        
        QString valueStr;
        if (value.typeId() == QMetaType::QStringList) {
            valueStr = createList(value.toStringList());
        } else {
            valueStr = value.toString();
        }
        
        pairs.append(QString("%1 {%2}").arg(key).arg(valueStr));
    }
    
    return pairs.join(" ");
}

// Quick type checking helpers
bool TclUtils::isNumeric(const QString &str)
{
    if (str.isEmpty()) return false;
    
    bool ok;
    str.toDouble(&ok);
    return ok;
}

bool TclUtils::isInteger(const QString &str)
{
    if (str.isEmpty()) return false;
    
    bool ok;
    str.toInt(&ok);
    return ok;
}

bool TclUtils::isDouble(const QString &str)
{
    if (str.isEmpty()) return false;
    
    bool ok;
    double val = str.toDouble(&ok);
    // Check if it's a double (has decimal part) vs integer
    return ok && (val != static_cast<int>(val) || str.contains('.'));
}

bool TclUtils::isBoolean(const QString &str)
{
    QString lower = str.toLower();
    return (lower == "true" || lower == "false" || 
            lower == "yes" || lower == "no" ||
            lower == "1" || lower == "0" ||
            lower == "on" || lower == "off");
}

// Safe extraction with defaults
int TclUtils::extractIntWithDefault(const QString &str, int defaultValue)
{
    if (!m_tclInterp) return defaultValue;
    
    Tcl_Obj *obj = Tcl_NewStringObj(str.toUtf8().constData(), -1);
    Tcl_IncrRefCount(obj);
    
    bool ok;
    int result = extractInt(obj, &ok);
    
    Tcl_DecrRefCount(obj);
    return ok ? result : defaultValue;
}

double TclUtils::extractDoubleWithDefault(const QString &str, double defaultValue)
{
    if (!m_tclInterp) return defaultValue;
    
    Tcl_Obj *obj = Tcl_NewStringObj(str.toUtf8().constData(), -1);
    Tcl_IncrRefCount(obj);
    
    bool ok;
    double result = extractDouble(obj, &ok);
    
    Tcl_DecrRefCount(obj);
    return ok ? result : defaultValue;
}

bool TclUtils::extractBoolWithDefault(const QString &str, bool defaultValue)
{
    if (!m_tclInterp) return defaultValue;
    
    Tcl_Obj *obj = Tcl_NewStringObj(str.toUtf8().constData(), -1);
    Tcl_IncrRefCount(obj);
    
    bool ok;
    bool result = extractBool(obj, &ok);
    
    Tcl_DecrRefCount(obj);
    return ok ? result : defaultValue;
}

QString TclUtils::extractStringFromList(const QString &listStr, int index, const QString &defaultValue)
{
    QStringList list = parseList(listStr);
    if (index >= 0 && index < list.size()) {
        return list[index];
    }
    return defaultValue;
}

// Common parsing patterns
QVariantMap TclUtils::parseKeyValuePairs(const QString &str, const QString &separator)
{
    QVariantMap result;
    
    // First try as Tcl dict
    if (isValidDict(str)) {
        return parseDictToVariantMap(str);
    }
    
    // Otherwise parse as simple key-value pairs
    QStringList parts = str.split(separator, Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size() - 1; i += 2) {
        QString key = parts[i];
        QString value = parts[i + 1];
        
        // Try to determine the type
        if (isInteger(value)) {
            result[key] = value.toInt();
        } else if (isDouble(value)) {
            result[key] = value.toDouble();
        } else if (isBoolean(value)) {
            result[key] = extractBoolWithDefault(value, false);
        } else {
            result[key] = value;
        }
    }
    
    return result;
}

QPair<QString, QString> TclUtils::splitKeyValue(const QString &str, const QString &separator)
{
    int pos = str.indexOf(separator);
    if (pos > 0) {
        return qMakePair(str.left(pos).trimmed(), 
                        str.mid(pos + separator.length()).trimmed());
    }
    return qMakePair(str, QString());
}

QStringList TclUtils::parseSpaceSeparatedQuoted(const QString &str)
{
    // First try Tcl list parsing
    if (isValidList(str)) {
        return parseList(str);
    }
    
    // Manual parsing for quoted strings
    QStringList result;
    QString current;
    bool inQuotes = false;
    bool escaped = false;
    
    for (int i = 0; i < str.length(); ++i) {
        QChar ch = str[i];
        
        if (escaped) {
            current += ch;
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            inQuotes = !inQuotes;
        } else if (ch == ' ' && !inQuotes) {
            if (!current.isEmpty()) {
                result.append(current);
                current.clear();
            }
        } else {
            current += ch;
        }
    }
    
    if (!current.isEmpty()) {
        result.append(current);
    }
    
    return result;
}

// Nested structure helpers
QVariant TclUtils::parseNestedStructure(const QString &str)
{
    if (!m_tclInterp) return QVariant();
    
    // Try as dict first
    if (isValidDict(str)) {
        QVariantMap map = parseDictToVariantMap(str);
        
        // Recursively parse nested structures
        for (auto it = map.begin(); it != map.end(); ++it) {
            QString valueStr = it.value().toString();
            if (isValidDict(valueStr) || isValidList(valueStr)) {
                it.value() = parseNestedStructure(valueStr);
            }
        }
        return map;
    }
    
    // Try as list
    if (isValidList(str)) {
        QStringList list = parseList(str);
        QVariantList varList;
        
        for (const QString &item : list) {
            if (isValidDict(item) || isValidList(item)) {
                varList.append(parseNestedStructure(item));
            } else {
                varList.append(item);
            }
        }
        return varList;
    }
    
    // Return as string
    return str;
}

QString TclUtils::getNestedValue(const QString &dictStr, const QStringList &keyPath)
{
    if (keyPath.isEmpty()) return QString();
    
    QVariantMap currentMap = parseDictToVariantMap(dictStr);
    
    for (int i = 0; i < keyPath.size() - 1; ++i) {
        if (!currentMap.contains(keyPath[i])) {
            return QString();
        }
        
        QVariant value = currentMap[keyPath[i]];
        if (value.typeId() == QMetaType::QVariantMap) {
            currentMap = value.toMap();
        } else {
            // Try parsing string as dict
            QString valueStr = value.toString();
            if (isValidDict(valueStr)) {
                currentMap = parseDictToVariantMap(valueStr);
            } else {
                return QString();
            }
        }
    }
    
    return currentMap.value(keyPath.last()).toString();
}

// Validation and debugging
QString TclUtils::validateAndDescribe(const QString &str)
{
    QStringList descriptions;
    
    if (isValidDict(str)) {
        descriptions << "Valid Tcl dictionary";
        QVariantMap map = parseDictToVariantMap(str);
        descriptions << QString("Keys: %1").arg(QStringList(map.keys()).join(", "));
    }
    
    if (isValidList(str)) {
        descriptions << "Valid Tcl list";
        QStringList list = parseList(str);
        descriptions << QString("Items: %1").arg(list.size());
    }
    
    if (isInteger(str)) {
        descriptions << "Integer value";
    } else if (isDouble(str)) {
        descriptions << "Double value";
    } else if (isBoolean(str)) {
        descriptions << "Boolean value";
    }
    
    if (descriptions.isEmpty()) {
        descriptions << "Plain string";
    }
    
    return descriptions.join("; ");
}

QString TclUtils::prettyPrint(const QString &tclStr, int indent)
{
    // Simple pretty printer for Tcl structures
    QString result;
    int level = 0;
    bool inQuotes = false;
    
    for (int i = 0; i < tclStr.length(); ++i) {
        QChar ch = tclStr[i];
        
        if (ch == '"' && (i == 0 || tclStr[i-1] != '\\')) {
            inQuotes = !inQuotes;
            result += ch;
        } else if (!inQuotes && ch == '{') {
            result += "{\n";
            level++;
            result += QString(level * indent, ' ');
        } else if (!inQuotes && ch == '}') {
            if (result.endsWith(' ')) {
                result.chop(indent);
            }
            if (result.endsWith('\n')) {
                result.chop(1);
            }
            level--;
            result += "\n" + QString(level * indent, ' ') + "}";
        } else if (!inQuotes && ch == ' ' && i + 1 < tclStr.length() && 
                   (tclStr[i+1] == '{' || (i > 0 && tclStr[i-1] == '}'))) {
            result += "\n" + QString(level * indent, ' ');
        } else {
            result += ch;
        }
    }
    
    return result;
}

// Conversion helpers  
QString TclUtils::variantMapToDict(const QVariantMap &map)
{
    // Alias for createDict for clarity
    return createDict(map);
}

QString TclUtils::stringListToList(const QStringList &list)
{
    // Alias for createList for clarity
    return createList(list);
}

QString TclUtils::escapeForTcl(const QString &str)
{
    // More comprehensive Tcl escaping
    QString escaped = str;
    
    // Escape special characters
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    escaped.replace("\n", "\\n");
    escaped.replace("\r", "\\r");
    escaped.replace("\t", "\\t");
    escaped.replace("$", "\\$");
    escaped.replace("[", "\\[");
    escaped.replace("]", "\\]");
    
    // If contains spaces or special chars, wrap in braces
    if (escaped.contains(' ') || escaped.contains('\t') || 
        escaped.contains('\n') || escaped.contains('{') || 
        escaped.contains('}')) {
        // Check if we need quotes or braces
        if (escaped.contains('"') && !escaped.contains('{')) {
            return "{" + escaped + "}";
        } else {
            return "\"" + escaped + "\"";
        }
    }
    
    return escaped;
}


bool TclUtils::isCompleteTclCommand(const QString &command)
{
    if (!m_tclInterp) return false;
    
    // Use Tcl_CommandComplete to check if command is complete
    return Tcl_CommandComplete(command.toUtf8().constData()) != 0;
}

int TclUtils::countCommandWords(const QString &command)
{
    if (!m_tclInterp) return 0;
    
    Tcl_Parse parse;
    const char *cmd = command.toUtf8().constData();
    int length = command.length();
    
    if (Tcl_ParseCommand(m_tclInterp, cmd, length, 0, &parse) != TCL_OK) {
        return 0;
    }
    
    int numWords = parse.numWords;
    Tcl_FreeParse(&parse);
    
    return numWords;
}

bool TclUtils::isBracedArgument(const QString &command, int argIndex)
{
    if (!m_tclInterp || argIndex < 0) return false;
    
    Tcl_Parse parse;
    const char *cmd = command.toUtf8().constData();
    int length = command.length();
    
    if (Tcl_ParseCommand(m_tclInterp, cmd, length, 0, &parse) != TCL_OK) {
        return false;
    }
    
    bool isBraced = false;
    
    // We need to find the token for the word at argIndex
    // Tcl_Parse contains an array of tokens, where command words are TCL_TOKEN_WORD type
    int wordIndex = 0;
    
    for (int i = 0; i < parse.numTokens; i++) {
        Tcl_Token *token = &parse.tokenPtr[i];
        
        if (token->type == TCL_TOKEN_WORD) {
            if (wordIndex == argIndex) {
                // Check if this word starts with a brace
                if (token->start && token->size > 0 && token->start[0] == '{') {
                    isBraced = true;
                }
                break;
            }
            wordIndex++;
        }
    }
    
    Tcl_FreeParse(&parse);
    return isBraced;
}

TclUtils::CommandInfo TclUtils::analyzeCommand(const QString &commandLine)
{
    CommandInfo info;
    info.argCount = 0;
    
    if (!m_tclInterp || commandLine.isEmpty()) return info;
    
    Tcl_Parse parse;
    const char *cmd = commandLine.toUtf8().constData();
    int length = commandLine.length();
    
    if (Tcl_ParseCommand(m_tclInterp, cmd, length, 0, &parse) != TCL_OK) {
        Tcl_FreeParse(&parse);
        return info;
    }
    
    // Parse the command structure
    if (parse.numWords > 0) {
        // First word is the command (might be a variable like $sys)
        Tcl_Token *cmdToken = &parse.tokenPtr[1]; // Skip TCL_TOKEN_COMMAND, get first word
        info.command = QString::fromUtf8(cmdToken->start, cmdToken->size).trimmed();
        info.argCount = parse.numWords - 1;
        
        // Analyze each argument after the command
        int tokenIdx = 1; // Start at first word token
        
        for (int wordNum = 0; wordNum < parse.numWords; wordNum++) {
            if (wordNum > 0) { // Skip command word, look at arguments
                Tcl_Token *wordToken = &parse.tokenPtr[tokenIdx];
                
                // Check if this word is a braced argument
                bool isBraced = false;
                if (wordToken->type == TCL_TOKEN_WORD && wordToken->numComponents > 0) {
                    // Look at the first component of this word
                    Tcl_Token *component = &parse.tokenPtr[tokenIdx + 1];
                    if (component->type == TCL_TOKEN_TEXT && 
                        wordToken->size > 0 && 
                        wordToken->start[0] == '{') {
                        isBraced = true;
                    }
                }
                info.bracedArgs.append(isBraced);
            }
            
            // Skip to next word token
            if (tokenIdx < parse.numTokens) {
                Tcl_Token *tok = &parse.tokenPtr[tokenIdx];
                tokenIdx += tok->numComponents + 1;
            }
        }
    }
    
    Tcl_FreeParse(&parse);
    return info;
}
// ESS-specific helper functions
namespace EssTclHelpers {

QMap<QString, QStringList> parseStateTransitions(const QString &stateTableStr)
{
    TclUtils *tcl = TclUtils::instance();
    QMap<QString, QStringList> transitions = tcl->parseDictToStringLists(stateTableStr);
#ifdef DEBUG    
    if (!tcl->lastError().isEmpty()) {
        qDebug() << "Failed to parse state transitions:" << tcl->lastError();
    } else {
        qDebug() << "Parsed" << transitions.size() << "state transitions";
    }
#endif    
    return transitions;
}

QStringList extractStateNames(const QString &stateTableStr)
{
    QMap<QString, QStringList> transitions = parseStateTransitions(stateTableStr);
    QStringList states = transitions.keys();
    
    // Also add any states mentioned in transitions that aren't keys
    for (auto it = transitions.constBegin(); it != transitions.constEnd(); ++it) {
        const QStringList &targetStates = it.value();
        for (const QString &targetState : targetStates) {
            if (!states.contains(targetState) && !targetState.isEmpty()) {
                states.append(targetState);
            }
        }
    }
    
    states.removeDuplicates();
    states.sort();
    
    return states;
}

QVariantMap parseParameters(const QString &paramsStr)
{
    TclUtils *tcl = TclUtils::instance();
    return tcl->parseDictToVariantMap(paramsStr);
}

} // namespace EssTclHelpers