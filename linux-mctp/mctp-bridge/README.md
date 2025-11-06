# MCTP Bridge

This repository contains a user-space MCTP bridge that connects the Linux kernel MCTP subsystem, user-space applications, and a physical serial transport. The bridge translates between three logical ports: a kernel-facing unicast interface (used by AF_MCTP sockets), a user-space broadcast socket (AF_UNIX datagram), and a serial device that carries MCTP frames on the wire. The bridge is intended to let applications participate in MCTP unicast and broadcast exchanges without handling framing, checksums, or low-level link details.

## Architecture (high level)

The bridge sits in user space with three logical links:

```
+--------------------------------+        +----------------------+        +----------------------+
|  Port 1: Kernel MCTP           |◄──A───►|   Bridge (User Space)|◄──────►| Port 2: Serial Device|
|  Interface: mctpserial<x>      |        |                      |        | (e.g. /dev/ttyUSB0)  |
+--------------------------------+        +----------------------+        +----------------------+
                                                  ▲
                                                B │
                                                  ▼
                                 +--------------------------------+
                                 | Port 3: Broadcast Interface    |
                                 | Interface: bcast_mctpserial<x> |
                                 +--------------------------------+
```

- Port 1 — Kernel MCTP: a kernel-visible `mctp-serial<N>` interface. Kernel-land AF_MCTP sockets send unicast packets here and the kernel routes packets to the correct interface based on destination EID.  Optionally this interface may be renamed by command line arguments when the mctp-bridge is instantialed.
- Port 2 — Serial Transport: the physical serial device (e.g. `/dev/ttyUSB0`). The bridge encodes and decodes MCTP frames to/from this link.
- Port 3 — Broadcast interface that user-space processes use to send and receive broadcast MCTP messages via the bridge.  The name of this interface always matches the name of the kernel-facing interface, but prepended by `bcast`

These three pieces let user processes talk to remote MCTP endpoints (unicast) via the kernel stack and participate in broadcast exchanges via the bridge's UNIX datagram socket.

## User view

For most users the bridge is an application you run against a serial device. When started, the bridge:

- creates a kernel-visible `mctp-serial<N>` (or user-named) interface for unicast MCTP traffic (this is used by AF_MCTP sockets), and
- creates a UNIX datagram interface `bcast_mctp-serial<N>` (or `bcast_<kernel interface name>` for user-space broadcast messages.

### Unicast flow (what an application sees):

Applications create AF_MCTP sockets and bind to a local EID. When they send a unicast packet to a remote EID that is reachable via the bridge's kernel-facing interface, the kernel forwards the packet to the bridge, which transmits it over the serial link. Replies arrive back through the serial transport and are forwarded into the kernel/AF_MCTP path.

### Broadcast flow (user-space broadcast):

Applications that want to send broadcast MCTP messages use a `AF_UNIX`/`SOCK_DGRAM` socket and `sendto()` the bridge's broadcast interface. The bridge transmits the datagram as an MCTP broadcast on the serial bus. Any device that responds will have the response relayed back to the sender (if the sender had a bound socket address).

### Usage — quick example (run as root or via sudo):

```bash
sudo ./mctp-bridge --mode BRIDGE --tty /dev/ttyUSB0 --ifname testbridge --baud B115200 --local-eid 8 --dest-eid 9 
```



## Developer view

The implementation handles framing, reassembly, and routing between three transports. Key responsibilities include:

- decoding and encoding MCTP frames to/from the serial line,
- forwarding unicast messages between the kernel and the serial transport,
- handling user-space broadcast datagrams on the AF_UNIX socket and mapping sender addresses for replies,
- ensuring correct behavior under blocking I/O and clean shutdown.

### Prerequisites

- Linux (Debian/Ubuntu, Fedora, etc.)
- C++17-capable compiler (g++ or clang)
- CMake >= 3.16
- pkg-config
- libnl development packages (names vary by distro): on Debian/Ubuntu install `libnl-3-dev libnl-genl-3-dev libnl-route-3-dev`; on Fedora `libnl3-devel`.

(Note: `pkg-config` must be able to locate the `libnl` `.pc` files; see Troubleshooting below if CMake cannot find `libnl-3`, `libnl-genl-3` or `libnl-route-3`.)

### Quick build (out-of-tree)

The CMakeLists for the bridge lives under `src/`. From the `mctp-bridge/` directory use an out-of-tree build that points at `src/` so the build files are created in `build/` and the source tree is left untouched.

```bash
# install deps (example for Debian/Ubuntu)
sudo apt update
# ensure pkg-config and apt-file are present; apt-file helps locate which package provides a given file
sudo apt install build-essential cmake pkg-config apt-file libnl-3-dev libnl-genl-3-dev libnl-route-3-dev

# (optional) initialize apt-file database so you can locate files provided by packages
sudo apt-file update

# create an out-of-tree build and configure (from mctp-bridge/)
cmake -S src -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

# build the project
cmake --build build -- -j$(nproc)

# run the built binary (example)
sudo build/mctp-bridge --mode BRIDGE --tty /dev/ttyUSB0 --baud B115200
```

