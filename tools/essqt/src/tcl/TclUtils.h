// src/tcl/TclUtils.h
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QVariant>
#include <QDebug>
#include <tcl.h>

/**
 * @brief General-purpose Tcl parsing and utility functions
 * 
 * This class provides common Tcl parsing functionality that can be shared
 * across the entire application. It handles Tcl data structure parsing,
 * script evaluation, and other Tcl-related operations.
 */
class TclUtils : public QObject
{
    Q_OBJECT

public:
    explicit TclUtils(QObject *parent = nullptr);
    ~TclUtils();

    /**
     * @brief Get a shared instance of TclUtils (singleton pattern)
     * @return Shared TclUtils instance
     */
    static TclUtils* instance();

    // Core Tcl parsing functions
    
    /**
     * @brief Parse a Tcl dictionary string into a QMap
     * @param dictStr The Tcl dictionary string
     * @return QMap with parsed key-value pairs, values as QStringList
     */
    QMap<QString, QStringList> parseDictToStringLists(const QString &dictStr);

    /**
     * @brief Parse a Tcl dictionary string into a QVariantMap
     * @param dictStr The Tcl dictionary string
     * @return QVariantMap with parsed key-value pairs
     */
    QVariantMap parseDictToVariantMap(const QString &dictStr);

    /**
     * @brief Parse a Tcl list string into a QStringList
     * @param listStr The Tcl list string
     * @return QStringList with parsed elements
     */
    QStringList parseList(const QString &listStr);

    /**
     * @brief Parse a Tcl list of lists into nested QStringLists
     * @param listStr The Tcl list string containing sublists
     * @return QList<QStringList> with parsed nested elements
     */
    QList<QStringList> parseNestedList(const QString &listStr);

    // Type-safe value extraction
    
    /**
     * @brief Extract string value from Tcl object
     * @param objPtr Tcl object pointer
     * @return QString representation
     */
    QString extractString(Tcl_Obj *objPtr);

    /**
     * @brief Extract integer value from Tcl object
     * @param objPtr Tcl object pointer
     * @param ok Optional pointer to success flag
     * @return Integer value, 0 if conversion fails
     */
    int extractInt(Tcl_Obj *objPtr, bool *ok = nullptr);

    /**
     * @brief Extract double value from Tcl object
     * @param objPtr Tcl object pointer
     * @param ok Optional pointer to success flag
     * @return Double value, 0.0 if conversion fails
     */
    double extractDouble(Tcl_Obj *objPtr, bool *ok = nullptr);

    /**
     * @brief Extract boolean value from Tcl object
     * @param objPtr Tcl object pointer
     * @param ok Optional pointer to success flag
     * @return Boolean value, false if conversion fails
     */
    bool extractBool(Tcl_Obj *objPtr, bool *ok = nullptr);

    // Validation and utility functions
    
    /**
     * @brief Check if a string is a valid Tcl list
     * @param str String to validate
     * @return true if valid Tcl list, false otherwise
     */
    bool isValidList(const QString &str);

    /**
     * @brief Check if a string is a valid Tcl dictionary
     * @param str String to validate
     * @return true if valid Tcl dictionary, false otherwise
     */
    bool isValidDict(const QString &str);

    /**
     * @brief Escape a string for safe use in Tcl
     * @param str String to escape
     * @return Escaped string safe for Tcl
     */
    QString escapeString(const QString &str);

    /**
     * @brief Create a properly formatted Tcl list from QStringList
     * @param list QStringList to convert
     * @return Tcl-formatted list string
     */
    QString createList(const QStringList &list);

    /**
     * @brief Create a properly formatted Tcl dictionary from QVariantMap
     * @param map QVariantMap to convert
     * @return Tcl-formatted dictionary string
     */
    QString createDict(const QVariantMap &map);

    // Error handling
    
    /**
     * @brief Check if the Tcl interpreter is ready
     * @return true if interpreter is available, false otherwise
     */
    bool isReady() const { return m_tclInterp != nullptr; }

    /**
     * @brief Get last error message from Tcl operations
     * @return Error message string, empty if no error
     */
    QString lastError() const { return m_lastError; }

    /**
     * @brief Clear the last error message
     */
    void clearError() { m_lastError.clear(); }

    // Advanced functionality (for future use)
    
    /**
     * @brief Get direct access to Tcl interpreter (use with caution)
     * @return Tcl interpreter pointer, nullptr if not available
     */
    Tcl_Interp* interpreter() const { return m_tclInterp; }

private:
    static TclUtils *s_instance;
    
    Tcl_Interp *m_tclInterp;
    QString m_lastError;

    void setError(const QString &error);
    bool initializeInterpreter();
    
    // Helper for recursive parsing
    QVariant parseObjectToVariant(Tcl_Obj *objPtr);
};

// Convenience functions for common ESS-specific parsing tasks
namespace EssTclHelpers {
    /**
     * @brief Parse ESS state transitions from state table datapoint
     * @param stateTableStr The state table string from ess/state_table
     * @return Map of state name to list of possible transitions
     */
    QMap<QString, QStringList> parseStateTransitions(const QString &stateTableStr);

    /**
     * @brief Extract all state names from ESS state table
     * @param stateTableStr The state table string
     * @return List of all state names found
     */
    QStringList extractStateNames(const QString &stateTableStr);

    /**
     * @brief Parse ESS parameter dictionary
     * @param paramsStr Parameter string from ess/params
     * @return Map of parameter names to values
     */
    QVariantMap parseParameters(const QString &paramsStr);
}