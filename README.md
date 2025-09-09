# SHMX: Shared-Memory IPC for Frames and Streams

A tiny, lock-free(ish) shared-memory transport for high-rate frame streaming with a typed static directory, per-frame TLVs, and a per-reader control ring. Cross-platform (Windows/Linux).

This README reflects the current code in `shmx_common.h`, `shmx_server.h`, `shmx_client.h`, and `shmx_inspector.h`.

---

## What problems it solves

* **Low-latency producer→consumers** over shared memory.
* **Versioned, self-describing schema** via a static directory (stream metadata).
* **Per-frame payload** composed of TLVs (one TLV per stream per frame).
* **Back-pressure tolerant** ring of frame slots: slow readers drop frames, never see torn data.
* **Client→server control** via per-reader lock-free ring buffers.
* **Introspection** (read-only) into layout, readers, frames via `Inspector`.

---

## Core concepts

### Static directory (schema)

* Server publishes stream metadata once (and can append).
* Each entry: `stream_id`, `element_type` (e.g., `DT_F64`), `components`, `layout` (SoA/AoS), `bytes_per_elem`, human-readable `name`, optional `extra`.
* Client/Inspector parse it into `StaticState` / `InspectDirEntry`.

### Frames and streams

* A frame is written to one slot in the slots area.
* Frame payload = a sequence of TLVs; each `TLV_FRAME_STREAM` carries one stream for this frame.
* Server computes a `checksum` over the frame payload; client validates before use.

### Ring of slots

* Server publishes frames round-robin over `slots`.
* Write ordering: payload → fence → header (`frame_id`) → `write_index`.
* Readers select the latest slot by reading `write_index`; torn or mismatched frames are rejected (checksum/session guard).
* Slow readers may **drop** frames; they never read half-written data.

### Control rings (client→server)

* Each reader has a dedicated circular buffer (`control_per_reader` bytes, 16-aligned).
* Client writes TLVs (type + bytes) into its ring; server polls them with `poll_control`.

---

## Wire format and ABI

* **Magic/version/endianness** checked in `GlobalHeader`.
* **Alignment**: header/slots 64B, TLVs 16B.
* **Checksum**: 32-bit derived from FNV-1a64 of payload (`checksum32`).
* **Endianness tag**: `0x01020304`.

---

## API overview

### Server (`shmx::Server`)

Create and publish frames:

```cpp
shmx::Server::Config cfg{
    .name = "shmx_demo",
    .slots = 4,
    .reader_slots = 16,
    .static_bytes_cap = 4096,
    .frame_bytes_cap = 65536,
    .control_per_reader = 4096
};

std::vector<shmx::StaticStream> streams{
    {42, shmx::DT_U64, 1, shmx::LAYOUT_SOA_SCALAR, sizeof(uint64_t), "tick_seq", {}},
    {43, shmx::DT_F64, 1, shmx::LAYOUT_SOA_SCALAR, sizeof(double),   "tick_sim", {}}
};

shmx::Server srv;
if (!srv.create(cfg, streams)) throw std::runtime_error("create failed");

auto fm = srv.begin_frame();
uint64_t seq = 1;
double sim = 0.0;
shmx::Server::append_stream(fm, 42, &seq, 1, sizeof(seq));
shmx::Server::append_stream(fm, 43, &sim, 1, sizeof(sim));
srv.publish_frame(fm, sim);
```

Control messages (read client→server TLVs):

```cpp
std::vector<shmx::Server::ControlMsg> msgs;
srv.poll_control(msgs, 256);
```

Utilities:

* `write_static_append(data, bytes)` to extend static area.
* `snapshot_readers()` to inspect reader slots.
* `reap_stale_readers(now_ticks, timeout_ticks)` to free dead readers.

### Client (`shmx::Client`)

Connect, read latest frame, decode TLVs, send control:

```cpp
shmx::Client cli;
if (!cli.open("shmx_demo")) throw std::runtime_error("open failed");

// static directory
shmx::StaticState st;
cli.refresh_static(st);

// latest frame
shmx::FrameView fv;
if (cli.latest(fv)) {
    shmx::DecodedFrame df;
    shmx::Client::decode(fv, df);
    for (auto& [sid, item] : df.streams) {
        // item.ptr, item.bytes, item.elem_count
    }
}

// send control TLV
uint64_t stamp = 123;
cli.control_send(0x48425254, &stamp, sizeof(stamp));
```

Lifecycle:

* `open(name)` maps and validates the shm, attaches a reader slot.
* `close()` detaches the slot and unmaps.

Client safety:

* Rejects frames if `session_id_copy` changes.
* Verifies `checksum`; returns false on mismatch.

### Inspector (`shmx::Inspector`)

Read-only, no server changes required. Exposes layout and introspection:

```cpp
shmx::Inspector ins;
if (!ins.open("shmx_demo")) throw std::runtime_error("open failed");

const auto* H = ins.header();
auto L = ins.layout();
auto dir = ins.decode_static_dir();
auto readers = ins.snapshot_readers();

shmx::InspectFrameView fv;
if (ins.latest(fv)) {
    std::vector<std::pair<uint32_t, shmx::InspectItem>> streams;
    shmx::Inspector::decode_frame(fv, streams);
}
```

Use the included `test_inspector` for a live, colored “dashboard” of:

* total shared memory and section capacities,
* static/reader/control/frames offsets and sizes,
* reader slots (in-use, id, last seen frame, heartbeat),
* latest frame summary and per-stream TLVs,
* a proportional “memory bar” with legends.

---

## Memory layout (high level)

```
[ GlobalHeader | Static (cap) | ReaderSlots (reader_stride * reader_slots)
  | Control (control_stride * reader_slots) | Slots (slot_stride * slots) ]

slot_stride = align(sizeof(FrameHeader),64) + align(frame_bytes_cap,64)
```

* Total size is computed and fully mapped by client/inspector.
* All offsets/strides/caps are present in `GlobalHeader` (reflected by `InspectLayout`).

---

## Design guarantees

* **Writer order**: payload → fence → header.frame\_id → write\_index.
* **Reader correctness**: either gets a full, checksum-valid frame or rejects; no torn reads.
* **Drop policy**: readers may skip frames if writer laps them.
* **Multi-writer frames**: supported via `reserve_index` sequencing; publish uses `seq` to set `write_index`.

---

## Type system

Primitive element types:

* `DT_BOOL`, `DT_I8/U8`, `DT_I16/U16`, `DT_I32/U32`, `DT_I64/U64`, `DT_F16/BF16`, `DT_F32/F64`

Layouts:

* `LAYOUT_SOA_SCALAR`, `LAYOUT_AOS_VECTOR`

Clients interpret bytes using `StaticState.dir` metadata and the per-frame TLVs.

---

## Building

### Dependencies

* C++20 (or “latest” on MSVC).
* Windows: Win32 API (CreateFileMapping/MapViewOfFile).
* Linux: POSIX shm (`shm_open`, `mmap`).

### CMake snippet

```cmake
add_library(shmx INTERFACE)
target_include_directories(shmx INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(test_server test/test_server.cpp)
target_link_libraries(test_server PRIVATE shmx)

add_executable(test_client test/test_client.cpp)
target_link_libraries(test_client PRIVATE shmx)

add_executable(test_inspector test/test_inspector.cpp)
target_link_libraries(test_inspector PRIVATE shmx)
```

On Windows terminals, ANSI color is enabled at runtime for the inspector.

---

## Runtime knobs

`Server::Config`

* `name`: shared memory name (Windows object name or POSIX shm path).
* `slots`: ring size (≥3 recommended for fast producers).
* `reader_slots`: maximum concurrent readers.
* `static_bytes_cap`: capacity for static directory.
* `frame_bytes_cap`: per-frame payload cap.
* `control_per_reader`: per-reader control ring capacity.

---

## Typical patterns

* **Sim→Render decouple**: Sim publishes positions/velocities/etc.; Render is a client that decodes frames, can push control TLVs (e.g., parameter changes) back.
* **Algo visualization**: Algo publishes step arrays; UI client renders, time-scrubs, or records.

---

## Troubleshooting

* Client `open` fails: name mismatch or server not up.
* `latest` returns false: no frames yet or checksum/session mismatch or caps wrong.
* `control_send` returns false: ring full; consumer not polling; or `control_per_reader==0`.
* Readers never show up: ensure client `open` succeeded (attaches slot on first control).

---

## Versioning

* Header constants: `MAGIC`, `VER_MAJOR=2`, `VER_MINOR=0`, `ENDIAN_TAG`.
* Any breaking layout change should bump `VER_MAJOR`.

---

## License

MIT (or your choice; adjust as needed).

---

## Acknowledgements

This design borrows common patterns from game engines and HFT style ring buffers: monotonic seq, publish after fence, reader best-effort freshness, and per-client control rings.
