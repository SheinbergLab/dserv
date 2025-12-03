# docsapi-1.0.tm
# =============================================================================
# Documentation API Module
# =============================================================================
# JSON API endpoints for the documentation frontend.
# Designed to be called via WebSocket eval from web clients.
#
# Usage (in subprocess):
#   package require docsdb
#   package require docsapi
#   docsdb::init "/path/to/docs.db"
#
# Then from WebSocket:
#   api_entries_list {type tutorial category basics}
#   api_entry_get some-slug
#   api_search {query "dl_fromto"}
#
# =============================================================================

package require yajltcl

namespace eval ::docsapi {
    variable version "1.0"
}

# =============================================================================
# JSON Encoding Helpers
# =============================================================================

proc ::docsapi::json {data} {
    # Encode Tcl data structure as JSON using yajltcl
    set gen [yajl create #auto]
    encode_value $gen $data 1
    set result [$gen get]
    $gen delete
    return $result
}

proc ::docsapi::encode_value {gen value {top 0}} {
    # Detect type and encode appropriately
    # top=1 means this is the top-level call (expect dict or list of dicts)
    
    if {$value eq ""} {
        $gen null
    } elseif {$value eq "true"} {
        $gen bool true
    } elseif {$value eq "false"} {
        $gen bool false
    } elseif {[string is integer -strict $value]} {
        $gen integer $value
    } elseif {[string is double -strict $value]} {
        $gen double $value
    } elseif {$top && [is_list_of_dicts $value]} {
        # Top-level array of objects
        encode_array $gen $value
    } elseif {[is_dict $value]} {
        encode_dict $gen $value
    } else {
        # Default: treat as string
        $gen string $value
    }
}

proc ::docsapi::encode_dict {gen d} {
    $gen map_open
    dict for {key val} $d {
        $gen string $key
        encode_value $gen $val 0
    }
    $gen map_close
}

proc ::docsapi::encode_array {gen items} {
    $gen array_open
    foreach item $items {
        encode_value $gen $item 0
    }
    $gen array_close
}

proc ::docsapi::encode_list {gen items} {
    # Explicitly encode as JSON array of strings
    $gen array_open
    foreach item $items {
        $gen string $item
    }
    $gen array_close
}

proc ::docsapi::is_dict {value} {
    # Only return true for dicts we explicitly created
    # These always have 4+ elements (at least 2 key-value pairs)
    # and keys are simple lowercase identifiers
    if {[catch {dict size $value}]} {
        return 0
    }
    set len [llength $value]
    if {$len < 4} {
        # Less than 2 key-value pairs - not a dict we created
        return 0
    }
    if {$len % 2 != 0} {
        return 0
    }
    # First key should be a simple lowercase word (like "id", "slug", "name")
    set first_key [lindex $value 0]
    if {![regexp {^[a-z][a-zA-Z]*$} $first_key]} {
        return 0
    }
    return 1
}

proc ::docsapi::is_list_of_dicts {value} {
    if {[llength $value] < 1} {
        return 0
    }
    foreach item $value {
        if {![is_dict $item]} {
            return 0
        }
    }
    return 1
}

# Helper for optional dict access
proc ::docsapi::dict_get_default {d key default} {
    if {[dict exists $d $key]} {
        return [dict get $d $key]
    }
    return $default
}

# =============================================================================
# Entry Listing API
# =============================================================================

proc api_entries_list {{filter {}}} {
    # List entries with optional filters
    # filter can include: type, category, namespace, published
    
    set where_clauses [list "1=1"]
    
    if {[dict exists $filter type]} {
        set type [dict get $filter type]
        lappend where_clauses "e.entry_type = '$type'"
    }
    
    if {[dict exists $filter category]} {
        set cat [dict get $filter category]
        lappend where_clauses "c.slug = '$cat'"
    }
    
    if {[dict exists $filter namespace]} {
        set ns [dict get $filter namespace]
        lappend where_clauses "e.namespace = '$ns'"
    }
    
    if {[dict exists $filter published]} {
        set pub [dict get $filter published]
        lappend where_clauses "e.published = $pub"
    } else {
        # Default to published only
        lappend where_clauses "e.published = 1"
    }
    
    set where [join $where_clauses " AND "]
    
    set query "
        SELECT 
            e.id, e.slug, e.entry_type, e.title, e.summary,
            e.namespace, e.syntax, e.difficulty, e.stability,
            e.deprecated, e.sort_order,
            c.slug as category_slug, c.name as category_name
        FROM entries e
        LEFT JOIN categories c ON e.category_id = c.id
        WHERE $where
        ORDER BY c.sort_order, e.sort_order, e.title
    "
    
    set results [list]
    ::docsdb::db eval $query row {
        set item [dict create \
            id $row(id) \
            slug $row(slug) \
            type $row(entry_type) \
            title $row(title) \
            summary $row(summary) \
            namespace $row(namespace) \
            syntax $row(syntax) \
            difficulty $row(difficulty) \
            stability $row(stability) \
            deprecated [expr {$row(deprecated) ? true : false}] \
        ]
        
        if {$row(category_slug) ne ""} {
            dict set item category [dict create \
                slug $row(category_slug) \
                name $row(category_name) \
            ]
        }
        
        lappend results $item
    }
    
    return [::docsapi::json $results]
}

# Convenience aliases
proc api_commands_list {{filter {}}} {
    dict set filter type command
    return [api_entries_list $filter]
}

proc api_tutorials_list {{filter {}}} {
    dict set filter type tutorial
    return [api_entries_list $filter]
}

# =============================================================================
# Single Entry API
# =============================================================================

proc api_entry_get {slug} {
    # Get full entry details by slug
    
    set found 0
    ::docsdb::db eval {
        SELECT 
            e.*, c.slug as category_slug, c.name as category_name
        FROM entries e
        LEFT JOIN categories c ON e.category_id = c.id
        WHERE e.slug = $slug
    } row {
        set found 1
        set result [dict create \
            id $row(id) \
            slug $row(slug) \
            type $row(entry_type) \
            title $row(title) \
            summary $row(summary) \
            content $row(content) \
            namespace $row(namespace) \
            syntax $row(syntax) \
            returnType $row(return_type) \
            seeAlso $row(see_also) \
            difficulty $row(difficulty) \
            estimatedTime $row(estimated_time) \
            stability $row(stability) \
            deprecated [expr {$row(deprecated) ? true : false}] \
            deprecatedMsg $row(deprecated_msg) \
        ]
        
        if {$row(category_slug) ne ""} {
            dict set result category [dict create \
                slug $row(category_slug) \
                name $row(category_name) \
            ]
        }
        
        set entry_id $row(id)
    }
    
    if {!$found} {
        return [::docsapi::json [dict create error "Entry not found" slug $slug]]
    }
    
    # Get parameters (for commands)
    set params [list]
    ::docsdb::db eval {
        SELECT name, param_type, description, is_optional, default_value
        FROM parameters
        WHERE entry_id = $entry_id
        ORDER BY sort_order
    } {
        lappend params [dict create \
            name $name \
            type $param_type \
            description $description \
            optional [expr {$is_optional ? true : false}] \
            default $default_value \
        ]
    }
    if {[llength $params] > 0} {
        dict set result parameters $params
    }
    
    # Get examples
    set examples [list]
    ::docsdb::db eval {
        SELECT id, title, description, code, expected_output, output_type, example_type
        FROM examples
        WHERE entry_id = $entry_id
        ORDER BY sort_order
    } {
        lappend examples [dict create \
            id $id \
            title $title \
            description $description \
            code $code \
            expectedOutput $expected_output \
            outputType $output_type \
            exampleType $example_type \
        ]
    }
    if {[llength $examples] > 0} {
        dict set result examples $examples
    }
    
    # Get hints (for tutorials)
    set hints [list]
    ::docsdb::db eval {
        SELECT hint_text FROM hints
        WHERE entry_id = $entry_id
        ORDER BY sort_order
    } {
        lappend hints $hint_text
    }
    if {[llength $hints] > 0} {
        dict set result hints $hints
    }
    
    # Get tags
    set tags [list]
    ::docsdb::db eval {
        SELECT t.name FROM tags t
        JOIN entry_tags et ON t.id = et.tag_id
        WHERE et.entry_id = $entry_id
    } {
        lappend tags $name
    }
    if {[llength $tags] > 0} {
        dict set result tags $tags
    }
    
    # Get related entries
    set related [list]
    ::docsdb::db eval {
        SELECT e.slug, e.title, e.entry_type, el.link_type
        FROM entry_links el
        JOIN entries e ON el.to_entry_id = e.id
        WHERE el.from_entry_id = $entry_id
    } {
        lappend related [dict create \
            slug $slug \
            title $title \
            type $entry_type \
            linkType $link_type \
        ]
    }
    if {[llength $related] > 0} {
        dict set result related $related
    }
    
    return [::docsapi::json $result]
}

# Convenience aliases
proc api_command_get {name_or_slug} {
    # Try slug first, then command name
    set slug $name_or_slug
    
    # If it doesn't look like a slug, convert command name to slug
    if {![string match "cmd-*" $slug]} {
        set slug "cmd-[string map {:: - _ -} $name_or_slug]"
    }
    
    return [api_entry_get $slug]
}

proc api_tutorial_get {slug} {
    return [api_entry_get $slug]
}

# =============================================================================
# Categories API
# =============================================================================

proc api_categories_list {} {
    set results [list]
    
    ::docsdb::db eval {
        SELECT id, slug, name, description, icon, sort_order
        FROM categories
        ORDER BY sort_order, name
    } {
        # Count entries in this category
        set entry_count [::docsdb::db eval {
            SELECT COUNT(*) FROM entries 
            WHERE category_id = $id AND published = 1
        }]
        
        lappend results [dict create \
            id $id \
            slug $slug \
            name $name \
            description $description \
            icon $icon \
            entryCount $entry_count \
        ]
    }
    
    return [::docsapi::json $results]
}

# =============================================================================
# Search API
# =============================================================================

proc api_search {params} {
    set query [::docsapi::dict_get_default $params query ""]
    set type [::docsapi::dict_get_default $params type ""]
    set limit [::docsapi::dict_get_default $params limit 20]
    
    if {$query eq ""} {
        return [::docsapi::json [list]]
    }
    
    # Build FTS query - use quotes for phrase, append * for prefix match
    # Avoid leading * which FTS5 doesn't support
    set fts_query "\"${query}\" OR ${query}*"
    
    set type_filter ""
    if {$type ne ""} {
        set type_filter "AND e.entry_type = '$type'"
    }
    
    set results [list]
    
    catch {
        ::docsdb::db eval "
            SELECT 
                e.slug, e.entry_type, e.title, e.summary, e.namespace,
                snippet(entries_fts, 1, '<mark>', '</mark>', '...', 30) as snippet
            FROM entries_fts
            JOIN entries e ON entries_fts.rowid = e.id
            WHERE entries_fts MATCH '$fts_query'
            AND e.published = 1
            $type_filter
            ORDER BY rank
            LIMIT $limit
        " row {
            lappend results [dict create \
                slug $row(slug) \
                type $row(entry_type) \
                title $row(title) \
                summary $row(summary) \
                namespace $row(namespace) \
                snippet $row(snippet) \
            ]
        }
    }
    
    return [::docsapi::json $results]
}

# =============================================================================
# Quick Help (for autocomplete tooltips)
# =============================================================================

proc api_quick_help {command_name} {
    set slug "cmd-[string map {:: - _ -} $command_name]"
    
    set result ""
    ::docsdb::db eval {
        SELECT syntax, summary, return_type
        FROM entries
        WHERE slug = $slug
    } {
        set result [dict create \
            syntax $syntax \
            description $summary \
            returns $return_type \
        ]
    }
    
    if {$result eq ""} {
        return [::docsapi::json [dict create error "Not found"]]
    }
    
    return [::docsapi::json $result]
}

# =============================================================================
# Namespace listing (for filter UI)
# =============================================================================

proc api_namespaces_list {} {
    set results [list]
    
    ::docsdb::db eval {
        SELECT namespace, COUNT(*) as cnt
        FROM entries
        WHERE entry_type = 'command' 
        AND published = 1
        AND namespace != ''
        GROUP BY namespace
        ORDER BY namespace
    } {
        lappend results [dict create \
            namespace $namespace \
            count $cnt \
        ]
    }
    
    return [::docsapi::json $results]
}

# =============================================================================
# Content Management (for authoring UI)
# =============================================================================

proc api_entry_create {data} {
    # Create a new entry
    set slug [dict get $data slug]
    set type [dict get $data type]
    set title [dict get $data title]
    
    set summary [::docsapi::dict_get_default $data summary ""]
    set content [::docsapi::dict_get_default $data content ""]
    set namespace [::docsapi::dict_get_default $data namespace ""]
    set syntax [::docsapi::dict_get_default $data syntax ""]
    set difficulty [::docsapi::dict_get_default $data difficulty "beginner"]
    set category_slug [::docsapi::dict_get_default $data category ""]
    
    # Get category ID
    set category_id ""
    if {$category_slug ne ""} {
        ::docsdb::db eval {SELECT id FROM categories WHERE slug = $category_slug} {
            set category_id $id
        }
    }
    
    ::docsdb::db eval {
        INSERT INTO entries (slug, entry_type, category_id, title, summary, content, namespace, syntax, difficulty)
        VALUES ($slug, $type, $category_id, $title, $summary, $content, $namespace, $syntax, $difficulty)
    }
    
    set id [::docsdb::db last_insert_rowid]
    
    # Add hints if provided
    if {[dict exists $data hints]} {
        set sort_order 0
        foreach hint [dict get $data hints] {
            ::docsdb::db eval {
                INSERT INTO hints (entry_id, hint_text, sort_order)
                VALUES ($id, $hint, $sort_order)
            }
            incr sort_order
        }
    }
    
    return [::docsapi::json [dict create id $id slug $slug]]
}

proc api_entry_update {slug data} {
    # Update an existing entry
    set updates [list]
    
    foreach {field column} {
        title title
        summary summary
        content content
        namespace namespace
        syntax syntax
        difficulty difficulty
        stability stability
        deprecated deprecated
        published published
    } {
        if {[dict exists $data $field]} {
            set val [dict get $data $field]
            lappend updates "$column = '$val'"
        }
    }
    
    if {[llength $updates] == 0} {
        return [::docsapi::json [dict create error "No fields to update"]]
    }
    
    set update_clause [join $updates ", "]
    
    ::docsdb::db eval "
        UPDATE entries SET $update_clause
        WHERE slug = '$slug'
    "
    
    return [::docsapi::json [dict create success true slug $slug]]
}

proc api_entry_delete {slug} {
    ::docsdb::db eval {DELETE FROM entries WHERE slug = $slug}
    return [::docsapi::json [dict create success true]]
}

# =============================================================================
# Stats
# =============================================================================

proc api_stats {} {
    set result [dict create]
    
    ::docsdb::db eval {SELECT COUNT(*) as cnt FROM entries WHERE entry_type = 'command' AND published = 1} {
        dict set result commands $cnt
    }
    ::docsdb::db eval {SELECT COUNT(*) as cnt FROM entries WHERE entry_type = 'tutorial' AND published = 1} {
        dict set result tutorials $cnt
    }
    ::docsdb::db eval {SELECT COUNT(*) as cnt FROM entries WHERE entry_type = 'guide' AND published = 1} {
        dict set result guides $cnt
    }
    ::docsdb::db eval {SELECT COUNT(*) as cnt FROM categories} {
        dict set result categories $cnt
    }
    ::docsdb::db eval {SELECT COUNT(*) as cnt FROM examples} {
        dict set result examples $cnt
    }
    
    return [::docsapi::json $result]
}

# =============================================================================
# Export
# =============================================================================

namespace eval ::docsapi {
    namespace export json
}
