# ESS Qt Control System - Project Context

## Project Overview

ESS (Experiment State System) is a neuroscience experiment control system built around a C++ backend server (dserv) with an embedded Tcl interpreter. We're rebuilding the Qt frontend to provide a robust, component-based GUI for experiment control and real-time data visualization.

## System Architecture

### Backend Infrastructure
- **dserv**: C++ server with multiple threads and embedded Tcl interpreter
- **Dynamic Groups (DG)**: Binary format for efficient data tables with C API
- **Datapoint System**: Unified format (name, timestamp, dataType, payload) for all data
- **Event System**: Experiment state machine events flow through `ess/events` datapoint

### Communication Channels
1. **DservClient (port 4620)**: Datapoint access, subscription, and direct dserv commands
2. **EssClient (port 2560)**: Experiment control via ESS Tcl module commands
3. **DservListener (TCP server)**: Receives real-time datapoint updates from dserv
4. **Local Tcl Interpreter**: Embedded Tcl for local scripting and data processing

### Core Qt Architecture Principles
- **Immediate Processing**: No queuing - process datapoints as they arrive (proven in existing prototypes)
- **Qt Native Messaging**: Use Qt signals/slots exclusively for performance and type safety
- **Component-Based**: Dockable components with automatic signal routing
- **DG Integration**: Keep data in native DG format, avoid JSON conversion overhead
- **Embedded Tcl**: Leverage existing Tcl decoders and command infrastructure

## Data Flow

### Incoming Data (Real-time)
```
dserv backend → DservListener → EssDataProcessor → EssSignalRouter → Components
```

### Bidirectional Command/Control
```
Terminal/Components ↔ EssClient (port 2560) ↔ ESS Tcl Module ↔ Shared Memory Datapoints
Terminal/Components ↔ DservClient (port 4620) ↔ dserv ↔ Datapoints  
Terminal/Components ↔ Local Tcl Interpreter (embedded)
```

### ESS Command Examples
- `::ess::load_system system protocol variant`
- `::ess::reload_system` 
- `::ess::start` / `::ess::stop` / `::ess::reset`
- `::ess::save_script type content`
- `::ess::set_param name value`
- Host discovery and connection management

### Key Data Types
- **Eye Tracking**: High-frequency position data (QVector<QPointF>)
- **Experiment Events**: State machine events from `ess/events` datapoint
- **Stimulus Data**: Binary DG tables (stimdg) 
- **Trial Data**: Binary DG tables (trialdg)
- **System State**: ess/* datapoints (status, system, protocol, variant, parameters)
- **Binary Data**: All handled via existing DG C API

## Core Classes

### EssDataProcessor
- Central hub for immediate datapoint processing
- Routes by datapoint name patterns (ess/*, stimdg, ain/*, etc.)
- Integrates with DG C API for binary data
- Maintains efficient circular buffers for high-frequency data
- Emits typed Qt signals for different data categories

### EssCommandInterface
- Unified interface for sending commands to all three channels:
  - ESS commands via EssClient (experiment control)
  - Datapoint commands via DservClient (data access)
  - Local Tcl commands via embedded interpreter
- Handles command routing and response parsing
- Provides async command execution with completion signals

### EssSignalRouter (Singleton)
- Loose coupling between processor and components
- Components declare interested signals via `interestedSignals()`
- Auto-connects components to relevant signals
- Pure Qt signal forwarding - zero runtime overhead

### EssTerminalComponent
- Multi-channel Tcl terminal for testing and control
- Command routing: automatically detect target (ESS/dserv/local)
- Command history and auto-completion
- Real-time response display
- Essential for testing communication layer and experiment control

### EssComponentBase
- Base class for all UI components
- Auto-registration with signal router
- Virtual slots for different data types
- Dockable by default via Qt's dock widget system
- Access to command interface for sending ESS commands

## Component Design Patterns

### Signal Interest Declaration
```cpp
QStringList interestedSignals() const override { 
    return {"eyeData", "experimentEvent", "systemState"}; 
}
```

### Automatic Signal Routing
Components receive only the signals they declare interest in, with full type safety.

### Example Component Structure
```cpp
class ExampleComponent : public EssComponentBase {
    QString componentName() const override { return "Example"; }
    QStringList interestedSignals() const override { return {"eyeData"}; }
    
protected slots:
    void onEyeDataUpdated(const QPointF& position) override {
        // Handle eye position updates
    }
};
```

## Implementation Priority & Phased Development

### Phase 1a: Terminal Component (CURRENT PRIORITY)
- [x] Architecture design and datapoint flow
- [ ] **START HERE**: Implement standalone terminal component with embedded Tcl
- [ ] Terminal should work independently (no dserv/network dependencies)
- [ ] Basic Tcl command execution and response display
- [ ] Command history, auto-completion, and terminal features
- [ ] Dockable widget integration

### Phase 1b: Communication Integration  
- [ ] Integrate DservClient/EssClient/DservListener into terminal
- [ ] Multi-channel command routing (ESS/dserv/local Tcl) 
- [ ] Test host discovery, connection, and basic datapoint flow
- [ ] Implement EssDataProcessor with immediate processing
- [ ] Create signal routing foundation

### Phase 2: Core Data Processing
- [ ] Complete EssSignalRouter implementation
- [ ] DG integration for binary data handling
- [ ] Event processing pipeline
- [ ] Component base classes and auto-registration

### Phase 3: UI Components (One at a time)
- [ ] Eye position visualization
- [ ] System control panel  
- [ ] Parameter editor
- [ ] Script editor
- [ ] Event timeline
- [ ] Performance metrics

## Development Workflow
- **Multi-session approach**: Build incrementally over multiple focused sessions
- **Terminal-first**: Terminal component serves as testing foundation for all other work
- **Leverage existing code**: Always ask about working prototypes before implementing from scratch
- **Component isolation**: Each component should work independently and validate signal routing

## Key Design Decisions

### Data Format Strategy
- **Keep DG format native**: No JSON conversion for performance
- **Pass DG pointers**: Components access data via DG C API
- **Efficient storage**: Circular buffers for high-frequency data
- **Type safety**: Qt signals provide compile-time type checking

### Communication Strategy  
- **Immediate processing**: Process datapoints as they arrive (no queuing)
- **Pattern-based routing**: Route by datapoint name patterns
- **Unified format**: All datapoints use (name, timestamp, dataType, payload)
- **Special handling**: `ess/events` contains all experiment state machine events

### Component Strategy
- **Built-in components**: Compiled into application (not plugins)
- **Dockable interface**: Use Qt's built-in dock widget system
- **Signal-based communication**: Components declare interests, router connects
- **Tcl integration**: Terminal component provides direct Tcl access

## Existing Code Assets

### Communication Classes (Working)
- `DservClient`: Handles dserv communication and subscriptions
- `DservListener`: TCP server for receiving datapoint updates  
- `EssClient`: Synchronous Tcl command execution

### Data Infrastructure
- Dynamic Groups C API for binary table access
- Tcl interpreter integration for data decoding
- Existing event type/subtype system
- Parameter management system

## Testing Strategy

### Terminal Component Priority
The terminal component serves as both a debugging tool and validation of the core architecture:
- Test host discovery and connection
- Execute Tcl commands directly
- Monitor datapoint flow
- Validate signal routing
- Provide fallback for any missing GUI functionality

### Development Workflow
1. Implement core classes with terminal component
2. Test communication layer thoroughly via terminal
3. Add visualization components incrementally
4. Each component validates signal routing independently

## Performance Requirements

- **Real-time eye tracking**: ~60-1000Hz position updates
- **Event processing**: Handle experiment events immediately  
- **No blocking**: UI must remain responsive during data processing
- **Memory efficient**: Circular buffers, minimal allocations in hot paths
- **Type safe**: Compile-time checked signal connections

## Future Considerations

### Extensibility
- Easy addition of new component types
- Script-driven canvas components for custom visualizations
- Integration with experiment-specific drawing scripts

### Deployment
- Single executable with embedded Tcl
- Configuration persistence via QSettings
- Layout save/restore for dockable components

---

## Notes for Future Development Sessions

- All datapoints follow unified format: (name, timestamp, dataType, payload)
- Use immediate processing - proven to work without overruns
- Keep data in native DG format for performance
- Components auto-connect via signal router - no manual subscription management
- Terminal component is critical for testing and debugging
- Qt native signals provide best performance and type safety