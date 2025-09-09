# shmx – Shared Memory IPC Framework

`shmx` is a cross-platform (Windows / POSIX) library that provides high-performance inter-process communication via shared memory.  
It is designed for low-latency data streaming and control messaging between a producer (server) and one or more consumers (clients).

---

## Features

- **Cross-platform shared memory abstraction** (`shmx::Map`)
- **Static metadata plane**
    - Describes streams (name, type, layout, components, extras)
    - Change detection via hash + generation counter
- **Frame ring buffer**
    - Multiple slots, each with a `FrameHeader` + payload TLVs
    - Readers always see the latest completed frame
- **Control plane**
    - Per-reader lock-free ring buffer for messages back to the server
    - Supports HELLO / HEARTBEAT / BYE and arbitrary TLVs
- **Reader tracking**
    - Each client registers a `ReaderSlot` with heartbeat + last frame seen
    - Server can snapshot readers and reap stale ones
- **Session safety**
    - Session IDs embedded in headers detect stale mappings
    - `session_mismatch` flag in `FrameView`

---

## Components

### Core headers
- **`shmx_common.h`** – constants, alignment helpers, TLV structures, global/shared memory headers, `Map` abstraction
- **`shmx_server.h`** – `Server` class, frame publishing, static metadata builder, control polling, reader snapshotting
- **`shmx_client.h`** – `Client` class, frame acquisition, static metadata refresh, decoding helpers, control sending

### Test applications
- **`server_test.cpp`**
    - Creates a shared memory segment with stream definitions
    - Publishes frames at ~30 FPS (tick counter + simulation time)
    - Handles client HELLO/HEARTBEAT/BYE control messages
    - Periodically logs connected readers and throughput
- **`client_test.cpp`**
    - Connects to the shared memory segment
    - Sends HELLO and periodic HEARTBEAT messages
    - Receives latest frames, decodes streams, prints values
    - Reconnects if frames are missing for too long

---
