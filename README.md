# shared-mem-ipc

**shared-mem-ipc** is a lightweight, high-performance, and cross-platform library for inter-process communication (IPC) based on **shared memory**.

It provides:
- **Publisher/Reader model** – multiple readers can connect to a single publisher.
- **Frame-based streaming** – data is transmitted as frames with headers and TLV (Type-Length-Value) metadata.
- **Zero-copy efficiency** – avoids extra memory copies for fast communication.
- **Cross-platform support** – works on both Windows and Linux using native `CreateFileMapping/MapViewOfFile` or `shm_open/mmap`.

### ✨ Key Features
- Slot-based frame buffer management
- Static stream directory for describing per-frame data layout
- Reader heartbeat and connection tracking
- Configurable capacity for static, frame, and control memory regions
- Simple C++17 API

### 🚀 Use Cases
- Real-time physics or graphics simulation visualization
- High-throughput producer-consumer pipelines
- Multi-process data sharing in games or media engines  

