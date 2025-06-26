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
```# essctrl - Multi-Service Command Line Client

`essctrl` is a versatile command-line client that provides both interactive and non-interactive access to multiple network services. It supports various service types including databases, messaging systems, git repositories, and more.

The default service is the messaging service (port 2560), which supports commands with newline characters and provides robust message-based communication.

## Features

- **Interactive Mode**: Full-featured command-line interface with history and tab completion
- **Command-Line Execution**: Execute single commands and exit (perfect for scripting)
- **Multi-Service Support**: Connect to different services on various ports
- **Newline Support**: Default messaging service handles multi-line commands and responses
- **History Management**: Persistent command history across sessions
- **Tab Completion**: Smart completion for common commands

## Supported Services

| Service   | Port | Description |
|-----------|------|-------------|
| msg       | 2560 | **Default** - Messaging service (supports newlines) |
| ess       | 2570 | ESS service |
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

### Command Line Syntax

```
essctrl [server] [options]
```

### Options

- `server` - Server address (default: localhost)
- `-c command` - Execute command and exit
- `-s service` - Target service (msg, ess, db, dserv, stim, pg, git, openiris)
- `-h` - Show help message

### Interactive Mode

Start essctrl without the `-c` option to enter interactive mode:

```bash
# Connect to localhost with default messaging service
./essctrl

# Connect to remote server
./essctrl server.example.com
```

In interactive mode, you can:
- Execute commands directly
- Switch between services using `/service` commands
- Use tab completion
- Access command history with up/down arrows
- Type `exit` to quit

#### Interactive Service Commands

- `/msg` - Switch to messaging service (default)
- `/ess` - Switch to ESS service
- `/db` - Switch to database service
- `/dserv` - Switch to data server
- `/stim` - Switch to stimulation service
- `/pg` - Switch to PostgreSQL service
- `/git` - Switch to git service
- `/openiris` - Switch to OpenIris service
- `/historylen N` - Set history length to N entries

You can also execute one-off commands on other services:
- `/db SELECT * FROM users` - Execute SQL command on database
- `/ess status` - Check ESS service status

### Non-Interactive Mode (Command Execution)

Execute single commands and exit - perfect for scripting and automation:

```bash
# Execute command on default service (messaging)
./essctrl -c "hello world"

# Execute multi-line command (newlines supported)
./essctrl -c "line 1
line 2
line 3"

# Execute command on specific service
./essctrl -s db -c "SELECT * FROM users"

# Execute on remote server
./essctrl server.example.com -s ess -c "status"

# Check PostgreSQL tables
./essctrl -s pg -c "\\dt"
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

#### Messaging (Default Service)

```bash
# Send a simple message
./essctrl -c "Hello world"

# Send multi-line message
./essctrl -c "Line 1
Line 2
Line 3"

# Check message queue status
./essctrl -c "STATUS"

# Send structured data
./essctrl -c "JSON:
{
  \"message\": \"hello\",
  \"timestamp\": \"$(date)\"
}"
```

#### ESS Service

```bash
# Check ESS service status
./essctrl -s ess -c "status"

# Get ESS information
./essctrl -s ess -c "info"
```

#### Database Operations

```bash
# Check database status
./essctrl -s db -c "STATUS"

# Run SQL query
./essctrl -s db -c "SELECT COUNT(*) FROM users"

# Multi-line SQL (using default messaging service for complex queries)
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

#### Scripting Examples

```bash
#!/bin/bash

# Health check script
echo "Checking service health..."

# Check messaging service (default)
msg_status=$(./essctrl -c "STATUS" 2>/dev/null)
if [ $? -eq 0 ]; then
    echo "Messaging: OK"
else
    echo "Messaging: FAILED"
fi

# Check ESS service
ess_status=$(./essctrl -s ess -c "status" 2>/dev/null)
if [ $? -eq 0 ]; then
    echo "ESS: OK"
else
    echo "ESS: FAILED"
fi

# Check database
db_status=$(./essctrl -s db -c "SELECT 1" 2>/dev/null)
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

## Configuration

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

## Error Handling

- **Connection failures**: Commands return non-zero exit codes when connection fails
- **Invalid services**: Program exits with error message for unknown service names
- **Command errors**: Server errors are displayed but don't crash the client

## Return Codes

- `0` - Success
- `1` - Error (connection failed, invalid arguments, etc.)

## Advanced Usage

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
4. **History not saving**: Check write permissions in the current directory

### Debug Tips

- Use `-c` mode to test individual commands
- Check service logs for detailed error messages
- Verify network connectivity with standard tools (telnet, nc)

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

## License

MIT License

Copyright (c) 2025 David Sheinberg

