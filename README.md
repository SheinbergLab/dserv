# dserv

## Overview

dserv is a real-time datapoint server for experimental control and data acquisition. At its core is a shared datapoint store (the "bus") implemented in C/C++ that acts as the central communication hub. Attached subprocesses, each running an isolated Tcl interpreter, can listen to, subscribe to, update, and react to changes in datapoints on the bus. This architecture provides extensive scriptability while retaining high-performance C/C++ internals.

All dserv instances include built-in HTTP and WebSocket support. Clients on connected subnets can discover each using a mesh topology, and web dashboards provide inspection and control of active systems.

## Architecture

```
                        ┌──────────────────────────┐
                        │     Datapoint Store      │
                        │        (the bus)         │
                        └───┬───┬───┬───┬───┬──────┘
                            │   │   │   │   │
               ┌─────────┬──┘   │   │   │   └──┬──────────┐
               │         │      │   │   │      │          │
            ┌──┴──┐  ┌───┴──┐ ┌─┴─┐ │ ┌─┴──┐ ┌─┴──┐  ┌───┴───┐
            │ ess │  │ mesh │ │ em│ │ │stim│ │ df │  │configs│
            └─────┘  └──────┘ └───┘ │ └────┘ └────┘  └───────┘
                                    │
                          ... more subprocesses ...
```
Datapoints are generic data stores that can point to one or more elements of a particular datatype (bytes, short and long integers, strings, floating point and double precision numbers, images, etc. 

Each subprocess is an isolated Tcl interpreter that has direct shared memory access to the datastore. Subprocesses can send messages to other subprocesses and communicate with remote dserv instances over TCP/IP. The startup configuration (`config/dsconf.tcl`) launches the standard set of subprocesses for experiment control, data management, mesh networking, and more.

### Core (`src/`)

The C/C++ core provides:
- **Datapoint store**: thread-safe key-value store with publish/subscribe semantics
- **Datapoint processors**: native C plugins for low-latency datapoint transformations
- **TclServer**: embedded Tcl interpreters for each subprocess with access to the bus
- **HTTP/WebSocket server**: built-in web server for GUIs and API access

### Modules (`modules/`)

Loadable shared libraries that extend subprocess capabilities with hardware and system integration:

| Module | Description |
|--------|-------------|
| `ain` | Analog input |
| `camera` | Camera capture (libcamera) |
| `eventlog` | Event logging |
| `gpio_input` / `gpio_output` | GPIO pin control |
| `ina226` | Power/current monitoring |
| `joystick4` | Joystick input |
| `juicer` | Pump/reward control |
| `mesh` | Mesh network communication |
| `rmt` | Remote stimulus display control |
| `sound` | Audio playback |
| `timer` | Precision timing |
| `touch` | Touchscreen input |
| `usbio` | USB I/O devices |

### Processors (`processors/`)

Native C plugins loaded into the datapoint bus for fast, low-latency processing. These run in the bus thread itself and are used for tasks that require deterministic timing:

- **windows / touch_windows**: define virtual spatial windows and track entry/exit events for eye or touch position data
- **sampler**: datapoint sampling/decimation
- **in_out**: binary region membership testing
- **up_down_left_right**: directional movement detection

### Configuration (`config/`)

Tcl scripts that configure and launch subprocesses. `dsconf.tcl` is the main startup script that initializes the system, loading subprocesses for experiment control (`ess`), eye tracking (`em`), reward delivery (`juicer`), stimulus control (`stim`), data file management (`df`), mesh networking (`mesh`), and others.

### Localization (`local/`)

Local configuration files allow each instance to be customized depending on attached hardware or software requirements. The `local/` folder includes example `.tcl` configuration files that can be copied and customized (removing the .EXAMPLE suffix). These will be maintained across dserv updates.

### Web Interfaces (`www/`)

Browser-based GUIs served by the built-in HTTP server:

- **Dashboard** (`index.html`) - system overview and terminal access
- **ESS Control** (`ess_control.html`) - experiment state system control
- **ESS Workbench** (`ess_workbench.html`) - experiment design and monitoring
- **Data Manager** (`data_manager.html`) - data file management
- **DG Table Viewer** (`DGTableViewer.html`) - data group inspection
- **StimDG Viewer** (`stimdg_viewer.html`) - stimulus data group viewer
- **Event Viewer** (`event_viewer.html`) - real-time event stream
- **Explorer** (`explorer.html`) - datapoint browser
- **Terminal** (`terminal.html`) - Tcl command terminal
- **Tutorial** (`tutorial.html`) - interactive tutorial

Available web interfaces can be accessed based on the names above, as in:

-* http://localhost:2565/ (gui)
-* http://localhost:2565/terminal
-* http://localhost:2565/explorer

### dserv-agent (`dserv-agent/`)

A complementary Go service that provides out-of-band management for dserv systems. Key capabilities:

- **Mesh discovery**: find and connect to dserv instances on the network
- **Script browsing**: browse and manage experiment scripts
- **Registry mode**: agents can run as registries that store and serve experiment scripts for connected dserv systems
- **Service management**: start/stop/restart dserv, install updates, and monitor health even when dserv is down

### dservctl CLI (`tools/dservctl`)

A command line program called `dservctl` provides a single executable (written in Go) that can be run from the terminal to interact with the dserv process and other subprocesses and to retrieve and update scripts on a dserv-agent registry server.

## Default Endpoints

| Port | Service |
|------|---------|
| 2560 | dserv TCP/IP message port |
| 2565 | dserv WebSocket (dashboard + GUIs) |

## Git Submodules

This repo uses [git submodules](https://git-scm.com/book/en/v2/Git-Tools-Submodules) for several dependencies:
 - [tcl](https://github.com/tcltk/tcl) 9
 - [jansson](https://github.com/akheron/jansson) JSON
 - [FLTK](https://github.com/fltk/fltk) GUI framework

To clone with submodules:

```
git clone --recurse-submodules https://github.com/SheinbergLab/dserv.git
```

To update submodules in an existing clone:

```
git submodule update --init --recursive
git pull --recurse-submodules
```

## Building

dserv uses CMake:

```
mkdir build && cd build
cmake ..
make
```

## Releases with GitHub Actions

This repo uses GitHub Actions to build and test on push, and to create releases on tag push. To release:

```
git tag -a 0.0.2 -m "Release description"
git push --tags
```

See [package_and_release.yml](./.github/workflows/package_and_release.yml) for workflow details and [Releases](https://github.com/SheinbergLab/dserv/releases) for artifacts.
