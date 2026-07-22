#
# settingsdb-1.0.tm -- tiny sqlite-backed store for STABLE per-setup settings
# that must survive dserv restarts (eye calibration, juicer, sound levels, ...).
#
# Keyed by (subsystem, profile); the value is an OPAQUE string -- the caller picks
# the representation (a Tcl dict, JSON, whatever it already uses). One small local
# db, opened per subprocess. Deliberately dumb: no schema per subsystem, so any
# subprocess can persist its settings without a migration.
#
#   settingsdb::init $dspath/db/calibration.db
#   settingsdb::save eye  $settings            ;# profile defaults to "default"
#   set s [settingsdb::load eye]               ;# "" if nothing stored yet
#

package provide settingsdb 1.0
package require sqlite3

namespace eval ::settingsdb {
    variable conn ""

    # Open/create the store at `path` (mkdir's the dir). Returns the sqlite
    # handle; idempotent-ish (re-init just reopens).
    proc init {path {handle ::settingsdb::db}} {
        variable conn
        set dir [file dirname $path]
        if {![file exists $dir]} { file mkdir $dir }
        sqlite3 $handle $path
        $handle eval {
            CREATE TABLE IF NOT EXISTS settings (
                subsystem  TEXT NOT NULL,
                profile    TEXT NOT NULL DEFAULT 'default',
                value      TEXT NOT NULL,
                updated_at TEXT NOT NULL DEFAULT (datetime('now')),
                PRIMARY KEY (subsystem, profile)
            );
        }
        set conn $handle
        return $handle
    }

    # Store `value` under (subsystem, profile). Upsert.
    proc save {subsystem value {profile default}} {
        variable conn
        $conn eval {
            INSERT OR REPLACE INTO settings (subsystem, profile, value, updated_at)
            VALUES ($subsystem, $profile, $value, datetime('now'))
        }
    }

    # The stored value for (subsystem, profile), or "" if none.
    proc load {subsystem {profile default}} {
        variable conn
        return [$conn onecolumn {
            SELECT value FROM settings WHERE subsystem=$subsystem AND profile=$profile
        }]
    }

    # Drop a stored entry (reset that subsystem/profile to caller defaults).
    proc forget {subsystem {profile default}} {
        variable conn
        $conn eval {DELETE FROM settings WHERE subsystem=$subsystem AND profile=$profile}
    }

    namespace export init save load forget
}
