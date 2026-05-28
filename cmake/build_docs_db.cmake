# build_docs_db.cmake — build db/docs.db from db/docs.sql at build time.
#
# Invoked via `cmake -P` from a custom command so it works under any
# generator (Makefiles, Ninja, Xcode) without relying on shell features.
#
# Required -D args:
#   SQLITE3  path to the sqlite3 executable
#   SQL      path to db/docs.sql (source of truth)
#   DB       output path for the built docs.db
#
# Behavior:
#   - Loading the data tables MUST succeed (fatal on failure).
#   - The full-text index (entries_fts) is best-effort: if the chosen
#     sqlite3 lacks the fts5 module, we warn but still succeed, because
#     the docs subprocess rebuilds the index at runtime with dlsh's
#     fts5-capable sqlite. This keeps the build green on machines whose
#     sqlite3 was compiled without fts5 (seen on some macOS CI runners).

if(NOT SQLITE3 OR NOT SQL OR NOT DB)
    message(FATAL_ERROR "build_docs_db.cmake requires -DSQLITE3= -DSQL= -DDB=")
endif()

file(REMOVE "${DB}")
get_filename_component(_dbdir "${DB}" DIRECTORY)
file(MAKE_DIRECTORY "${_dbdir}")

# Load the .sql. docs.sql wraps the table data in a transaction (commits
# before the FTS footer), so even if the fts5 statements fail, the data
# tables are already committed.
execute_process(
    COMMAND "${SQLITE3}" "${DB}" ".read ${SQL}"
    RESULT_VARIABLE _rc
    ERROR_VARIABLE  _err
    OUTPUT_VARIABLE _out
)

# Verify the core data actually loaded, regardless of the sqlite3 exit code
# (which may be nonzero solely due to the best-effort fts5 statements).
execute_process(
    COMMAND "${SQLITE3}" "${DB}" "SELECT COUNT(*) FROM entries;"
    RESULT_VARIABLE _count_rc
    OUTPUT_VARIABLE _count
    ERROR_QUIET
)
string(STRIP "${_count}" _count)

if(NOT _count_rc EQUAL 0 OR NOT _count MATCHES "^[0-9]+$" OR _count EQUAL 0)
    message(FATAL_ERROR
        "Failed to build docs.db: entries table empty or missing.\n"
        "sqlite3 rc=${_rc}\nstderr: ${_err}")
endif()

# Did the FTS index build? (entries_fts is a virtual table)
execute_process(
    COMMAND "${SQLITE3}" "${DB}"
        "SELECT COUNT(*) FROM sqlite_master WHERE name='entries_fts';"
    OUTPUT_VARIABLE _fts
    ERROR_QUIET
)
string(STRIP "${_fts}" _fts)

if(_fts STREQUAL "0" OR _fts STREQUAL "")
    message(WARNING
        "docs.db built with ${_count} entries, but the full-text index was "
        "NOT created (sqlite3 at '${SQLITE3}' lacks the fts5 module). "
        "Search will be unavailable until the docs subprocess rebuilds the "
        "index at runtime. Install an fts5-capable sqlite3 to embed it now.")
else()
    message(STATUS "docs.db built: ${_count} entries, full-text index present")
endif()
