# Namespace-Aware Completion

## Overview

The tab completion system automatically discovers and completes procedures from Tcl namespaces like `::ess`, including nested namespaces.

## How It Works

### Automatic Discovery

When the completion cache is populated (on connect or `:refresh`), the system:

1. **Queries top-level namespaces:**
   ```tcl
   namespace children ::
   # Returns: ::ess ::tcl ::mylib ...
   ```

2. **Gets procs from each namespace:**
   ```tcl
   info procs ::ess::*
   # Returns: ::ess::start ::ess::stop ::ess::configure ...
   ```

3. **Recursively checks nested namespaces:**
   ```tcl
   namespace children ::mylib
   # Returns: ::mylib::util ::mylib::data ...
   
   info procs ::mylib::util::*
   # Returns: ::mylib::util::helper ::mylib::util::format ...
   ```

4. **Stores all procs with full qualification**
   - Cache includes: `my_global_proc`, `::ess::start`, `::mylib::util::helper`

### Depth Limit

Currently queries:
- Global procs (no namespace)
- Top-level namespaces (`::namespace`)
- One level of nesting (`::namespace::child`)

**Rationale:** Most Tcl codebases don't go deeper than 2 levels, and querying too deeply could be slow.

## Usage Examples

### Basic Namespace Completion

**Show all procs in a namespace:**
```
[dserv] > ::ess::<TAB>
Matches: ::ess::start, ::ess::stop, ::ess::configure, ::ess::reset
```

**Complete a partial namespace-qualified name:**
```
[dserv] > ::ess::st<TAB>
Matches: ::ess::start, ::ess::stop
[dserv] > ::ess::sta<TAB>
[dserv] > ::ess::start           # Completed!
```

### Mixed Completion

**When not using `::` prefix, all procs are candidates:**
```
[dserv] > st<TAB>
Matches: start, stop, string, ::ess::start, ::ess::stop
```

**Once you type `::`, only namespace-qualified procs match:**
```
[dserv] > ::<TAB>
Matches: ::ess::start, ::ess::stop, ::mylib::init, ::tcl::clock::format, ...
```

### Nested Namespace Example

```
[dserv] > ::mylib::<TAB>
Matches: ::mylib::init, ::mylib::util::helper, ::mylib::data::load
[dserv] > ::mylib::util::<TAB>
Matches: ::mylib::util::helper, ::mylib::util::format, ::mylib::util::validate
[dserv] > ::mylib::util::h<TAB>
[dserv] > ::mylib::util::helper  # Completed!
```

## Common Patterns

### Pattern 1: Exploring a Namespace

```
# See what's available in ::ess
[dserv] > ::ess::<TAB>
Matches: ::ess::start, ::ess::stop, ::ess::configure, ::ess::reset, ::ess::status

# Try a specific one
[dserv] > ::ess::status
→ idle
```

### Pattern 2: Working with Namespace Commands

```
# Complete namespace procs naturally
[dserv] > ::ess::start
→ ESS started

[dserv] > ::ess::configure -voltage 5.0
→ Configured

# Same completion works in command routing
/eye_control ::ess::<TAB>
Matches: ::ess::calibrate, ::ess::track, ...
```

### Pattern 3: After Sourcing Namespace Code

```
# Source a file that defines ::myapp namespace
[dserv] > source /path/to/myapp.tcl

# Refresh to include new procs
[dserv] > :refresh
Refreshing completion caches...

# Now they're available
[dserv] > ::myapp::<TAB>
Matches: ::myapp::init, ::myapp::run, ::myapp::cleanup
```

## Technical Details

### Cache Structure

Each interpreter's completion cache includes:
```go
type CompletionCache struct {
    commands []string  // Built-in Tcl commands
    procs    []string  // Global + namespace-qualified procs
    globals  []string  // Global variables
}
```

**Example proc list:**
```
procs: [
    "my_helper",           // Global proc
    "calculate",           // Global proc
    "::ess::start",        // Namespace proc
    "::ess::stop",         // Namespace proc
    "::lib::util::log",    // Nested namespace proc
]
```

### Query Sequence

```
1. info procs                      → global procs
2. namespace children ::           → [::ess ::lib ::tcl]
3. info procs ::ess::*             → [::ess::start ::ess::stop ...]
4. namespace children ::ess        → [::ess::internal]
5. info procs ::ess::internal::*   → [::ess::internal::helper ...]
6. (repeat for each namespace)
```

### Completion Logic

When user types and presses Tab:

1. **Extract partial word**
2. **Check for `::`:**
   - Present → Only search procs, filter by namespace prefix
   - Absent → Search all (commands, procs, globals)
3. **Filter matches**
4. **Apply completion**

### Performance

| Namespaces | Query Time | Cache Size | Completion Speed |
|------------|------------|------------|------------------|
| 0-2        | 50-100ms   | ~10KB      | < 1ms            |
| 3-5        | 100-200ms  | ~20KB      | < 1ms            |
| 6-10       | 200-500ms  | ~50KB      | < 1ms            |

**Note:** Query happens once on connect/refresh (async). Completion itself is instant from cache.

## Troubleshooting

### Problem: Namespace procs don't appear

**Check 1:** Verify namespace exists:
```
[dserv] > namespace children ::
→ ::ess ::tcl ...
```

**Check 2:** Verify procs in namespace:
```
[dserv] > info procs ::ess::*
→ ::ess::start ::ess::stop ...
```

**Check 3:** Refresh cache:
```
[dserv] > :refresh
Refreshing completion caches...
```

### Problem: Deeply nested procs missing

**Cause:** Only queries 2 levels deep (global + 1 level of nesting).

**Workaround:** Manually complete the namespace path:
```
[dserv] > ::deeply::nested::namespace::my_proc
```

Or extend the depth in `RefreshCompletionCache()` if needed.

### Problem: Slow cache refresh

**Cause:** Many namespaces with many procs.

**Solutions:**
1. Accept the delay (only happens on connect/refresh)
2. Reduce query depth in code
3. Use more selective namespace queries

## Customization

### Adjusting Query Depth

To query deeper namespaces, modify `RefreshCompletionCache()` in `tcp_client.go`:

```go
// Current: queries 2 levels (global + 1 nested)
// To add more levels, add another loop:

for _, grandchild := range grandchildren {
    grandchildProcsCmd := fmt.Sprintf("info procs %s::*", grandchild)
    // ... query logic
}
```

**Trade-off:** More depth = slower refresh, more memory, more completions.

### Excluding Namespaces

To skip certain namespaces (e.g., `::tcl` internals):

```go
for _, ns := range namespaces {
    // Skip unwanted namespaces
    if ns == "::tcl" || ns == "::auto" {
        continue
    }
    // ... rest of query logic
}
```

## Examples from Real Interpreters

### ESS (Eye Subsystem)

```
[ess] > ::<TAB>
Matches: ::ess::start, ::ess::stop, ::ess::configure, ::ess::status,
         ::ess::calibrate, ::ess::track, ::ess::save_data

[ess] > ::ess::c<TAB>
Matches: ::ess::configure, ::ess::calibrate

[ess] > ::ess::ca<TAB>
[ess] > ::ess::calibrate
```

### Complex Namespace Hierarchy

```
[dserv] > ::myapp::<TAB>
Matches: ::myapp::init, ::myapp::gui::show, ::myapp::data::load,
         ::myapp::util::log, ::myapp::util::format

[dserv] > ::myapp::gui::<TAB>
Matches: ::myapp::gui::show, ::myapp::gui::hide, ::myapp::gui::update

[dserv] > ::myapp::util::<TAB>
Matches: ::myapp::util::log, ::myapp::util::format, ::myapp::util::validate
```

## Best Practices

1. **Use `:refresh` after loading new namespace code**
   ```
   [dserv] > source mylib.tcl
   [dserv] > :refresh
   ```

2. **Explore with Tab**
   - Type `::` and press Tab to see all namespaces
   - Type `::namespace::` and press Tab to explore that namespace

3. **Debug mode shows namespace stats**
   ```bash
   ./tcl-console -debug
   ```
   Look for: `Cache updated - procs:42 (global:15 ns:27)`

4. **Full qualification is optional**
   - For global procs: just type name
   - For namespace procs: can use full `::ns::proc` or just `proc` if unambiguous

## Future Enhancements

Potential improvements:
- [ ] Configurable depth limit
- [ ] Namespace-aware variable completion (`::ess::config_var`)
- [ ] Dynamic depth (query until no more children)
- [ ] Namespace caching (refresh namespaces separately from procs)
- [ ] Completion of namespace commands themselves (`namespace eval ...`)

## See Also

- **TAB_COMPLETION.md** - Complete tab completion guide
- **QUICK_START.md** - Getting started
- **README.md** - Feature overview
