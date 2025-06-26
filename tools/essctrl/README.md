# essctrl - Multi-Service Command Line Client

`essctrl` is a versatile command-line client that provides both interactive and non-interactive access to multiple network services. It supports various service types including databases, messaging systems, git repositories, and more.

The default service is the ESS service (port 2560), which supports commands with newline characters and provides robust message-based communication.

## Features

- **Interactive Mode**: Full-featured command-line interface with history and tab completion
- **Command-Line Execution**: Execute single commands and exit (perfect for scripting)
- **Stdin Processing**: Read and execute multiple commands from stdin (pipes, files, etc.)
- **Multi-Service Support**: Connect to different services on various ports
- **Newline Support**: Default messaging service handles multi-line commands and responses
- **History Management**: Persistent command history across sessions
- **Tab Completion**: Smart completion for common commands
- **Error Handling**: Automatic detection and handling of server errors with visual feedback
- **Return Codes**: Proper exit codes for scripting and automation

## Error Handling

`essctrl` automatically detects and handles server errors that are prefixed with `!TCL_ERROR `:

- **Interactive Mode**: Error messages are displayed in red (if terminal supports colors) with the error prefix removed
- **Non-Interactive Mode**: Returns appropriate exit codes and strips error prefixes from output
- **Batch Processing**: Tracks errors across multiple commands and returns proper exit codes

### Exit Codes

- `0` - Success (all commands executed without errors)
- `1` - Error occurred (connection failed, server error, invalid arguments, etc.)

## Supported Services

| Service   | Port | Description |
|-----------|------|-------------|
| ess       | 2560 | **Default** - ESS service (supports newlines) |
| legacy    | 2570 | Legacy ESS service (deprecated) |
| db        | 2571 | Database service |
| dserv     | 4620 | Data server |
| stim      | 4612 | Stimulation service |
| pg        | 2572 | PostgreSQL interface |
| git       | 2573 | Git repository service |
| openiris  | 2574 | OpenIris service |

## Installation

### Prerequisites

- C compiler (gcc, clang, or MSVC)
- linenoise library
- sockapi library (custom socket API)

### Building

```bash
gcc -o essctrl essctrl.c linenoise.c sockapi.c
```

Or with your existing build system/Makefile.

## Usage

### Input Methods

`essctrl` supports three input methods, checked in this priority order:

1. **Command-line execution** (`-c` flag) - Execute single command and exit
2. **Stdin processing** (when stdin is redirected) - Process multiple commands from pipes/files
3. **Interactive mode** (default) - Full interactive session with terminal

### Command Line Syntax

```
essctrl [server] [options]
```

### Options

- `server` - Server address (default: localhost)
- `-c command` - Execute command and exit
- `-s service` - Target service (ess, legacy, db, dserv, stim, pg, git, openiris)
- `-h` - Show help message

### Stdin Mode (Batch Processing)

Process multiple commands from stdin - perfect for automation and batch operations:

```bash
# Pipe commands directly
echo "status" | ./essctrl

# Multiple commands
echo -e "status\ninfo\nhelp" | ./essctrl

# From a file
cat commands.txt | ./essctrl

# Target specific service
echo "SELECT * FROM users" | ./essctrl -s db

# Here document
./essctrl <<EOF
status
info
help
EOF

# Generate commands programmatically
seq 1 5 | sed 's/^/ping host/' | ./essctrl

# Process results with other tools
echo "status" | ./essctrl | grep "uptime"

# Check for errors in batch processing
echo -e "good_cmd\nbad_cmd\ngood_cmd" | ./essctrl -s dserv
if [ $? -eq 0 ]; then
    echo "All commands succeeded"
else
    echo "At least one command failed"
fi
```

### Interactive Mode

Start essctrl without the `-c` option to enter interactive mode:

```bash
# Connect to localhost with default ESS service
./essctrl

# Connect to remote server
./essctrl server.example.com
```

In interactive mode, you can:
- Execute commands directly
- Switch between services using `/service` commands
- Use tab completion
- Access command history with up/down arrows
- See error messages highlighted in red (if terminal supports colors)
- Type `exit` to quit

#### Interactive Service Commands

- `/ess` - Switch to ESS service (default)
- `/legacy` - Switch to legacy ESS service (deprecated)
- `/db` - Switch to database service
- `/dserv` - Switch to data server
- `/stim` - Switch to stimulation service
- `/pg` - Switch to PostgreSQL service
- `/git` - Switch to git service
- `/openiris` - Switch to OpenIris service
- `/historylen N` - Set history length to N entries

You can also execute one-off commands on other services:
- `/db SELECT * FROM users` - Execute SQL command on database
- `/legacy status` - Check legacy ESS service status

### Non-Interactive Mode (Command Execution)

Execute single commands and exit - perfect for scripting and automation:

```bash
# Execute command on default service (ESS)
./essctrl -c "hello world"

# Execute multi-line command (newlines supported)
./essctrl -c "line 1
line 2
line 3"

# Execute command on specific service
./essctrl -s db -c "SELECT * FROM users"

# Execute on remote server
./essctrl server.example.com -s legacy -c "status"

# Check PostgreSQL tables
./essctrl -s pg -c "\\dt"

# Error handling in scripts
./essctrl -s dserv -c "some_command"
if [ $? -eq 0 ]; then
    echo "Command succeeded"
else
    echo "Command failed"
    exit 1
fi
```

### Examples

#### Basic Usage

```bash
# Interactive mode with localhost
./essctrl

# Interactive mode with specific server
./essctrl production-server.company.com

# Get help
./essctrl -h
```

#### ESS Service (Default)

```bash
# Send a simple message
./essctrl -c "Hello world"

# Send multi-line message
./essctrl -c "Line 1
Line 2
Line 3"

# Check ESS status
./essctrl -c "STATUS"

# Send structured data
./essctrl -c "JSON:
{
  \"message\": \"hello\",
  \"timestamp\": \"$(date)\"
}"
```

#### Legacy ESS Service (Deprecated)

```bash
# Check legacy ESS service status
./essctrl -s legacy -c "status"

# Get legacy ESS information
./essctrl -s legacy -c "info"
```

#### Database Operations

```bash
# Check database status
./essctrl -s db -c "STATUS"

# Run SQL query
./essctrl -s db -c "SELECT COUNT(*) FROM users"

# Multi-line SQL (using default ESS service for complex queries)
./essctrl -c "SQL:
SELECT u.name, u.email, COUNT(o.id) as order_count
FROM users u
LEFT JOIN orders o ON u.id = o.user_id
GROUP BY u.id, u.name, u.email
ORDER BY order_count DESC"

# PostgreSQL specific
./essctrl -s pg -c "\\l"  # List databases
./essctrl -s pg -c "SELECT version()"
```

#### Error Handling Examples

```bash
# Single command with error checking
./essctrl -s dserv -c "invalid_command"
echo "Exit code: $?"  # Will be 1 if error occurred

# Batch processing with error detection
./essctrl -s dserv <<EOF
good_command
bad_command
another_good_command
EOF
if [ $? -ne 0 ]; then
    echo "One or more commands failed"
fi

# Interactive mode - errors appear in red
./essctrl -s dserv
dserv> invalid_command
# Error message appears in red without "!TCL_ERROR " prefix
```

#### Scripting Examples

```bash
#!/bin/bash

# Health check script using stdin
./essctrl <<EOF | while read line; do
    echo "Service response: $line"
done
STATUS
INFO
PING
EOF
```

```bash
#!/bin/bash

# Batch database operations with error handling
./essctrl -s db <<EOF
SELECT COUNT(*) FROM users;
SELECT COUNT(*) FROM orders;
SELECT COUNT(*) FROM products;
EOF

if [ $? -eq 0 ]; then
    echo "All database queries completed successfully"
else
    echo "One or more database queries failed"
    exit 1
fi
```

```bash
#!/bin/bash

# Monitor multiple services
services=("legacy" "db" "ess")
overall_status=0

for service in "${services[@]}"; do
    echo "=== $service Status ==="
    echo "STATUS" | ./essctrl -s "$service"
    if [ $? -ne 0 ]; then
        echo "ERROR: $service is not responding properly"
        overall_status=1
    fi
    echo
done

exit $overall_status
```

```bash
#!/bin/bash

# Process command file with comprehensive error handling
if [ -f "daily_commands.txt" ]; then
    ./essctrl < daily_commands.txt
    exit_code=$?
    if [ $exit_code -eq 0 ]; then
        echo "Daily commands completed successfully"
    else
        echo "Error processing daily commands (exit code: $exit_code)"
        # Log the error or send notification
        echo "Daily command errors detected at $(date)" >> error.log
        exit 1
    fi
else
    echo "Error: daily_commands.txt not found"
    exit 1
fi
```

#### Legacy Script Examples

```bash
#!/bin/bash

# Health check script using single commands
echo "Checking service health..."

# Check messaging service (default)
./essctrl -c "STATUS" 2>/dev/null
if [ $? -eq 0 ]; then
    echo "Messaging: OK"
else
    echo "Messaging: FAILED"
fi

# Check ESS service
./essctrl -s ess -c "status" 2>/dev/null
if [ $? -eq 0 ]; then
    echo "ESS: OK"
else
    echo "ESS: FAILED"
fi

# Check database
./essctrl -s db -c "SELECT 1" 2>/dev/null
if [ $? -eq 0 ]; then
    echo "Database: OK"
else
    echo "Database: FAILED"
fi
```

```bash
#!/bin/bash

# Backup notification script with detailed info
./essctrl -c "BACKUP_START:
Server: $(hostname)
Started: $(date)
Type: Full backup
Target: /backup/$(date +%Y%m%d)"

# ... run backup ...

./essctrl -c "BACKUP_COMPLETE:
Server: $(hostname) 
Completed: $(date)
Duration: $((SECONDS/60)) minutes
Status: Success"
```

#### Git Operations

```bash
# Check git status
./essctrl -s git -c "status"

# Get repository information
./essctrl -s git -c "log --oneline -10"
```

#### Multi-line Command Examples

The default messaging service supports commands with embedded newlines:

```bash
# Send formatted text
./essctrl -c "REPORT:
Server: $(hostname)
Date: $(date)
Status: Online
Users: 42"

# Send configuration data
./essctrl -c "CONFIG:
server.port=8080
server.host=localhost
debug.enabled=true"
```

## Configuration

### Input Method Priority

`essctrl` automatically detects the input method in this order:

1. **Command-line argument** (`-c "command"`) - Highest priority, executes single command
2. **Redirected stdin** (pipes, files, here-docs) - Processes multiple commands line by line  
3. **Interactive terminal** - Default mode with full features

### History

Command history is automatically saved to `history.txt` in the current directory. The history persists across sessions and can be navigated using arrow keys in interactive mode.

### Connection Settings

Default connection settings:
- Server: localhost
- Default service: Messaging (port 2560)
- History file: history.txt

The messaging service (default) provides enhanced functionality:
- Supports commands and responses with embedded newlines
- Better handling of structured data and multi-line content
- Robust message-based communication protocol

## Error Handling Details

### Server Error Detection

`essctrl` automatically detects server errors by looking for responses that start with `!TCL_ERROR `. When such errors are detected:

1. **Interactive Mode**:
   - Error prefix (`!TCL_ERROR `) is automatically stripped
   - Error message is displayed in red if terminal supports colors
   - Session continues normally

2. **Non-Interactive Mode** (`-c` flag):
   - Error prefix is stripped from output
   - Command returns exit code 1
   - Program terminates

3. **Batch Mode** (stdin):
   - Error prefix is stripped from each error response
   - Processing continues for remaining commands
   - Final exit code is 1 if any command had an error

### Color Support

Error messages are displayed in red when:
- Running in interactive mode
- Terminal supports ANSI color codes
- `TERM` environment variable is set
- Output is directed to a terminal (not redirected)

### Error Handling in Scripts

```bash
# Simple error checking
./essctrl -s dserv -c "command"
if [ $? -ne 0 ]; then
    echo "Command failed"
    exit 1
fi

# Advanced error handling with output capture
output=$(./essctrl -s dserv -c "command" 2>&1)
exit_code=$?
if [ $exit_code -ne 0 ]; then
    echo "Error occurred: $output"
    # Log error, send alert, etc.
fi
```

## Return Codes

- `0` - Success (all commands executed without server errors)
- `1` - Error occurred (connection failed, server error, invalid arguments, etc.)

Note: Server errors (responses starting with `!TCL_ERROR `) are treated as command failures and result in exit code 1.

## Advanced Usage

### Stdin Processing Features

- **Line-by-line execution**: Each line is treated as a separate command
- **Empty line handling**: Blank lines are automatically skipped
- **Immediate output**: Results are printed as each command completes
- **Error continuation**: Processing continues even if individual commands fail
- **Service targeting**: Use `-s service` to direct all stdin commands to specific service
- **Error tracking**: Final exit code reflects whether any errors occurred

### Combining Input Methods

```bash
# File processing with specific service
cat sql_queries.txt | ./essctrl -s db

# Dynamic command generation
generate_reports.sh | ./essctrl -s msg

# Pipeline processing with error handling
./essctrl -s ess -c "get_nodes" | grep "active" | \
    sed 's/^/ping /' | ./essctrl
echo "Pipeline exit code: $?"

# Error-aware pipeline
if ./essctrl -s dserv -c "validate_config"; then
    echo "Config valid, proceeding..."
    cat deployment_commands.txt | ./essctrl -s dserv
else
    echo "Config validation failed, aborting"
    exit 1
fi
```

### Stdin vs Command-line Examples

```bash
# Single command (using -c)
./essctrl -c "STATUS"

# Multiple commands (using stdin)
echo -e "STATUS\nINFO\nVERSION" | ./essctrl

# Same result, different methods
./essctrl -c "STATUS"; ./essctrl -c "INFO"; ./essctrl -c "VERSION"
# vs
echo -e "STATUS\nINFO\nVERSION" | ./essctrl
```

### Service Switching in Interactive Mode

```
msg> hello world
msg> /db
db> SELECT * FROM users;
db> /ess  
ess> status
ess> /msg
msg> multi-line message:
line 1
line 2
```

### History Management

```
msg> /historylen 1000    # Set history to 1000 entries
msg> # Use up/down arrows to navigate history
```

### Tab Completion

Start typing and press `<Tab>` for command completion (currently supports basic completion for commands starting with 'h').

## Troubleshooting

### Common Issues

1. **Connection refused**: Ensure the target service is running on the expected port
2. **Command not found**: Check that the service supports the command you're trying to execute
3. **Permission denied**: Verify network access and firewall settings
4. **History not saving**: Check write permissions in the current directory (interactive mode only)
5. **Stdin not working**: Ensure stdin is properly redirected (use `cat file |` instead of `< file` if issues occur)
6. **Colors not showing**: Verify terminal supports ANSI colors and `TERM` environment variable is set

### Debugging Stdin Issues

```bash
# Test if stdin detection works
echo "test" | ./essctrl -s msg

# Check if file reading works  
cat > test_commands.txt <<EOF
STATUS
INFO
EOF
cat test_commands.txt | ./essctrl

# Verify command processing
echo -e "STATUS\nINFO" | ./essctrl | wc -l  # Should show 2+ lines of output

# Test error handling
echo -e "STATUS\ninvalid_command\nINFO" | ./essctrl -s dserv
echo "Exit code: $?"  # Should be 1 if invalid_command failed
```

### Debug Tips

- Use `-c` mode to test individual commands
- Check service logs for detailed error messages
- Verify network connectivity with standard tools (telnet, nc)
- Test error conditions: `./essctrl -s dserv -c "definitely_invalid_command"`
- Monitor exit codes in scripts for proper error handling

### Error Message Examples

```bash
# Server returns: "!TCL_ERROR command not found"
# essctrl displays: "command not found" (in red if interactive)

# Server returns: "!TCL_ERROR invalid syntax in command"  
# essctrl displays: "invalid syntax in command" (in red if interactive)

# Normal response: "OK: command completed"
# essctrl displays: "OK: command completed" (normal color)
```

## Development

### Building with Debug Information

```bash
gcc -g -DDEBUG -o essctrl essctrl.c linenoise.c sockapi.c
```

### Adding New Services

To add a new service:

1. Define the port and prompt constants
2. Add the service to `get_port_for_service()` function
3. Add interactive command handling in the main loop
4. Update this documentation

### Error Handling Implementation

The error handling is implemented in the `process_response()` function which:

1. Checks if response starts with `!TCL_ERROR `
2. Strips the error prefix if present
3. Applies color formatting in interactive mode (if supported)
4. Returns appropriate error codes for scripting

## License

MIT License

Copyright (c) 2025 David Sheinberg