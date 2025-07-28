# EssQt - ESS Control System Qt Frontend

A modern Qt6-based frontend for the ESS (Experiment State System) neuroscience experiment control system.

## Overview

EssQt provides a graphical interface for controlling experiments, monitoring real-time data, and interacting with the ESS backend through multiple communication channels.

## Features

- Multi-channel terminal with Tcl interpreter integration
- Real-time data visualization
- Experiment control interface
- Host discovery and connection management
- Dockable, customizable UI components

## Building

### Requirements

- Qt 6.2 or later
- CMake 3.16 or later
- Tcl 9.0
- C++17 compiler

### Build Instructions

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

## Architecture

The application is built with a component-based architecture:

- **Terminal Component**: Multi-mode terminal supporting Tcl, Dserv, and ESS commands
- **Communication Layer**: Handles connections to dserv (port 4620) and ESS (port 2560)
- **Signal Router**: Distributes real-time data to interested components
- **Component System**: Modular UI components that can be docked and arranged

## Development Status

Currently in Phase 1: Foundation
- [x] Project structure
- [x] Basic main window
- [ ] Terminal component
- [ ] Communication integration

See docs/development_plan.md for the full roadmap.
