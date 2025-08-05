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