#ifndef SHMX_SERVER_H
#define SHMX_SERVER_H
#include "shmx_common.h"
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace shmx {

    struct StaticStream {
        std::uint32_t stream_id, element_type, components, layout, bytes_per_elem;
        std::string name_utf8;
        std::vector<std::uint8_t> extra;
    };

    class Server {
    public:
        struct Config {
            std::string name;
            std::uint32_t slots{3}, reader_slots{16};
            std::uint32_t static_bytes_cap{0}, frame_bytes_cap{0};
            std::uint32_t control_per_reader{0};
        };
        struct ControlMsg {
            std::uint64_t reader_id;
            std::uint32_t type;
            std::vector<std::uint8_t> data;
        };

        Server() = default;
        ~Server() {
            destroy();
        }
        Server(const Server&)            = delete;
        Server& operator=(const Server&) = delete;

        [[nodiscard]] bool create(const Config& cfg, const std::vector<StaticStream>& streams) {
            destroy();
            if (cfg.name.empty() || cfg.slots == 0u || cfg.frame_bytes_cap == 0u) return false;

            const auto static_dir_bytes = build_static_dir(streams, static_dir_);
            if (cfg.static_bytes_cap && static_dir_bytes > cfg.static_bytes_cap) return false;

            const auto slot_stride    = align_up(static_cast<std::uint32_t>(sizeof(FrameHeader)), 64) + align_up(cfg.frame_bytes_cap, 64);
            const auto readers_stride = align_up(static_cast<std::uint32_t>(sizeof(ReaderSlot)), 64);
            const auto control_stride = align_up(cfg.control_per_reader ? (cfg.control_per_reader) : 0u, 64);

            const auto static_off  = align_up(static_cast<std::uint32_t>(sizeof(GlobalHeader)), 64);
            const auto static_cap  = align_up(cfg.static_bytes_cap ? cfg.static_bytes_cap : static_dir_bytes, 64);
            const auto readers_off = align_up(static_off + static_cap, 64);
            const auto control_off = align_up(readers_off + cfg.reader_slots * readers_stride, 64);
            const auto slots_off   = align_up(control_off + control_stride * cfg.reader_slots, 64);

            const auto total64 = static_cast<std::uint64_t>(slots_off) + static_cast<std::uint64_t>(cfg.slots) * slot_stride;
            if (total64 > std::numeric_limits<std::uint32_t>::max()) return false;

            if (!map_.create(cfg.name, static_cast<std::size_t>(total64))) return false;

            hdr_ = reinterpret_cast<GlobalHeader*>(map_.data());
            new (hdr_) GlobalHeader{};

            session_id_      = make_session_id();
            hdr_->magic      = MAGIC;
            hdr_->ver_major  = VER_MAJOR;
            hdr_->ver_minor  = VER_MINOR;
            hdr_->endianness = ENDIAN_TAG;
            hdr_->session_id = session_id_;
            hdr_->static_gen.store(0u, std::memory_order_relaxed);
            hdr_->static_offset      = static_off;
            hdr_->static_bytes_cap   = static_cap;
            hdr_->slots              = cfg.slots;
            hdr_->slot_stride        = slot_stride;
            hdr_->slots_offset       = slots_off;
            hdr_->frame_bytes_cap    = cfg.frame_bytes_cap;
            hdr_->reader_slots       = cfg.reader_slots;
            hdr_->reader_slot_stride = readers_stride;
            hdr_->readers_offset     = readers_off;
            hdr_->control_offset     = control_off;
            hdr_->control_per_reader = cfg.control_per_reader;
            hdr_->control_stride     = control_stride;
            hdr_->frame_seq.store(0u, std::memory_order_relaxed);
            hdr_->write_index.store(0u, std::memory_order_relaxed);
            hdr_->readers_connected.store(0u, std::memory_order_relaxed);
            hdr_->reserve_index.store(0u, std::memory_order_relaxed);

            if (!static_dir_.empty()) {
                if (static_dir_.size() > static_cap) return false;
                std::memcpy(map_.data() + static_off, static_dir_.data(), static_dir_.size());
                hdr_->static_bytes_used = static_cast<std::uint32_t>(static_dir_.size());
                hdr_->static_hash       = fnv1a64(map_.data() + static_off, hdr_->static_bytes_used);
                hdr_->static_gen.fetch_add(1u, std::memory_order_release);
            }

            for (std::uint32_t i = 0; i < cfg.reader_slots; ++i) {
                auto* RS = reinterpret_cast<ReaderSlot*>(map_.data() + readers_off + i * readers_stride);
                RS->reader_id.store(0u, std::memory_order_relaxed);
                RS->heartbeat.store(0u, std::memory_order_relaxed);
                RS->last_frame_seen.store(0u, std::memory_order_relaxed);
                RS->in_use.store(0u, std::memory_order_relaxed);
            }
            for (std::uint32_t s = 0; s < cfg.slots; ++s) {
                auto* FH            = reinterpret_cast<FrameHeader*>(map_.data() + slots_off + s * slot_stride);
                FH->session_id_copy = session_id_;
                FH->frame_id.store(0u, std::memory_order_relaxed);
                FH->sim_time      = 0.0;
                FH->payload_bytes = 0u;
                FH->tlv_count     = 0u;
                FH->checksum      = 0u;
            }

            slots_off_   = slots_off;
            readers_off_ = readers_off;
            return true;
        }

        void destroy() noexcept {
            hdr_         = nullptr;
            slots_off_   = 0;
            readers_off_ = 0;
            session_id_  = 0;
            static_dir_.clear();
            map_.close();
        }

        [[nodiscard]] bool write_static_append(const void* data, std::uint32_t bytes) const {
            if (!hdr_) return false;
            if (!data || bytes == 0u) return true;
            const auto cap  = hdr_->static_bytes_cap;
            const auto used = hdr_->static_bytes_used;
            if (used + bytes > cap) return false;
            auto* dst = map_.data() + hdr_->static_offset + used;
            std::memcpy(dst, data, bytes);
            std::atomic_thread_fence(std::memory_order_release);
            hdr_->static_bytes_used = used + bytes;
            hdr_->static_hash       = fnv1a64(map_.data() + hdr_->static_offset, hdr_->static_bytes_used);
            hdr_->static_gen.fetch_add(1u, std::memory_order_release);
            return true;
        }

        struct FrameMap {
            FrameHeader* fh;
            std::uint8_t* payload;
            std::uint32_t capacity, slot, tlv_count, used;
            std::uint32_t seq;
        };

        [[nodiscard]] FrameMap begin_frame() const {
            const auto seq1 = hdr_->reserve_index.fetch_add(1u, std::memory_order_acq_rel) + 1u;
            const auto slot = hdr_->slots ? ((seq1 - 1u) % hdr_->slots) : 0u;
            auto* base_slot = map_.data() + slots_off_ + slot * hdr_->slot_stride;
            auto* fh        = reinterpret_cast<FrameHeader*>(base_slot);
            auto* payload   = base_slot + align_up(static_cast<std::uint32_t>(sizeof(FrameHeader)), 64);
            return FrameMap{fh, payload, hdr_->frame_bytes_cap, slot, 0u, 0u, static_cast<std::uint32_t>(seq1)};
        }

        static bool append_stream(FrameMap& fm, std::uint32_t stream_id, const void* data, std::uint32_t elem_count, std::uint32_t elem_bytes_total) {
            if (!fm.fh || !data) return false;
            constexpr std::uint32_t tlv_head  = sizeof(TLV);
            constexpr std::uint32_t body_head = sizeof(FrameStreamTLV);
            const auto need                   = align_up(tlv_head + body_head + elem_bytes_total, 16);
            if (fm.used + need > fm.capacity) return false;
            auto* p = fm.payload + fm.used;
            TLV tlv{};
            tlv.type   = TLV_FRAME_STREAM;
            tlv.length = body_head + elem_bytes_total;
            std::memcpy(p, &tlv, sizeof(TLV));
            FrameStreamTLV fs{};
            fs.stream_id     = stream_id;
            fs.elem_count    = elem_count;
            fs.bytes_payload = elem_bytes_total;
            fs.reserved      = 0u;
            std::memcpy(p + tlv_head, &fs, sizeof(FrameStreamTLV));
            std::memcpy(p + tlv_head + body_head, data, elem_bytes_total);
            fm.used += need;
            fm.tlv_count += 1u;
            return true;
        }

        [[nodiscard]] bool publish_frame(FrameMap& fm, double sim_time) const {
            if (!hdr_ || !fm.fh) return false;
            if (fm.used > fm.capacity) return false;
            const auto fid         = hdr_->frame_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
            fm.fh->session_id_copy = hdr_->session_id;
            fm.fh->sim_time        = sim_time;
            fm.fh->payload_bytes   = fm.used;
            fm.fh->tlv_count       = fm.tlv_count;
            fm.fh->checksum        = checksum32(fm.payload, fm.used);
            std::atomic_thread_fence(std::memory_order_release);
            fm.fh->frame_id.store(fid, std::memory_order_release);
            hdr_->write_index.store(fm.seq, std::memory_order_release);
            return true;
        }

        struct ReaderInfo {
            std::uint64_t reader_id, heartbeat, last_frame_seen;
            bool in_use;
        };

        [[nodiscard]] std::vector<ReaderInfo> snapshot_readers() const {
            std::vector<ReaderInfo> v;
            v.reserve(hdr_->reader_slots);
            for (std::uint32_t i = 0; i < hdr_->reader_slots; ++i) {
                auto* RS = reinterpret_cast<ReaderSlot*>(map_.data() + readers_off_ + i * hdr_->reader_slot_stride);
                v.push_back(ReaderInfo{RS->reader_id.load(std::memory_order_acquire), RS->heartbeat.load(std::memory_order_acquire), RS->last_frame_seen.load(std::memory_order_acquire), RS->in_use.load(std::memory_order_acquire) != 0u});
            }
            return v;
        }

        [[nodiscard]] bool poll_control(std::vector<ControlMsg>& out, std::uint32_t max_msgs) const {
            out.clear();
            if (!hdr_) return false;
            if (hdr_->control_per_reader == 0u) return true;

            bool ok_all = true;
            for (std::uint32_t i = 0; i < hdr_->reader_slots; ++i) {
                auto* const CH  = map_.data() + hdr_->control_offset + i * hdr_->control_stride;
                const auto cap  = hdr_->control_per_reader;
                auto* const r64 = reinterpret_cast<std::atomic<std::uint64_t>*>(CH);
                auto* const w64 = r64 + 1;

                const auto rv = r64->load(std::memory_order_acquire);
                const auto wv = w64->load(std::memory_order_acquire);
                auto rd       = rv;

                while (rd != wv && out.size() < max_msgs) {
                    auto off = 16u + static_cast<std::uint32_t>(rd % (cap - 16u));
                    if (off + sizeof(TLV) > cap) {
                        rd += static_cast<std::uint64_t>(cap - off);
                        off = 16u;
                    }
                    TLV tlv{};
                    std::memcpy(&tlv, CH + off, sizeof(TLV));
                    const auto body = align_up(static_cast<std::uint32_t>(sizeof(TLV)) + tlv.length, 16);
                    if (off + body > cap) {
                        if (tlv.type == 0u) {
                            rd += static_cast<std::uint64_t>(cap - off);
                            continue;
                        }
                        ok_all = false;
                        rd     = wv;
                        break;
                    }
                    if (tlv.type != 0u) {
                        ControlMsg m{};
                        m.reader_id = reader_id_of(i);
                        m.type      = tlv.type;
                        m.data.resize(tlv.length);
                        std::memcpy(m.data.data(), CH + off + sizeof(TLV), tlv.length);
                        out.push_back(std::move(m));
                    }
                    rd += body;
                }
                if (rd != rv) r64->store(rd, std::memory_order_release);
            }
            return ok_all;
        }

        [[nodiscard]] const GlobalHeader* header() const noexcept {
            return hdr_;
        }
        [[nodiscard]] std::uint8_t* static_ptr() const noexcept {
            return map_.data() + hdr_->static_offset;
        }
        [[nodiscard]] std::uint32_t static_used() const noexcept {
            return hdr_->static_bytes_used;
        }
        [[nodiscard]] std::uint32_t readers_connected() const noexcept {
            return hdr_->readers_connected.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool reap_stale_readers(std::uint64_t now_ticks, std::uint64_t timeout_ticks) const {
            if (!hdr_) return false;
            bool any = false;
            for (std::uint32_t i = 0; i < hdr_->reader_slots; ++i) {
                auto* RS = reinterpret_cast<ReaderSlot*>(map_.data() + readers_off_ + i * hdr_->reader_slot_stride);
                if (RS->in_use.load(std::memory_order_acquire) == 0u) continue;
                const auto hb = RS->heartbeat.load(std::memory_order_acquire);
                if (hb == 0) continue;
                if (now_ticks > hb && now_ticks - hb > timeout_ticks) {
                    RS->reader_id.store(0u, std::memory_order_release);
                    RS->heartbeat.store(0u, std::memory_order_release);
                    RS->last_frame_seen.store(0u, std::memory_order_release);
                    RS->in_use.store(0u, std::memory_order_release);
                    hdr_->readers_connected.fetch_sub(1u, std::memory_order_acq_rel);
                    any = true;
                }
            }
            return any;
        }

    private:
        static std::uint64_t make_session_id() noexcept {
            const auto now        = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            const std::size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
            const int local       = 0;
            const auto mix        = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&local) ^ static_cast<std::uintptr_t>(tid));
            return static_cast<std::uint64_t>(now) ^ (mix * 0x9E3779B97F4A7C15ull);
        }
        static std::uint32_t build_static_dir(const std::vector<StaticStream>& streams, std::vector<std::uint8_t>& out) {
            out.clear();
            std::vector<std::uint8_t> tmp;
            for (const auto& [stream_id, element_type, components, layout, bytes_per_elem, name_utf8, extra] : streams) {
                const auto name_len  = static_cast<std::uint32_t>(name_utf8.size());
                const auto extra_len = static_cast<std::uint32_t>(extra.size());
                const auto body_len  = static_cast<std::uint32_t>(sizeof(StaticStreamDesc)) + name_len + extra_len;
                tmp.resize(align_up(static_cast<std::uint32_t>(sizeof(TLV)) + body_len, 16));
                auto* p = tmp.data();
                TLV tlv{};
                tlv.type   = TLV_STATIC_DIR;
                tlv.length = body_len;
                std::memcpy(p, &tlv, sizeof(TLV));
                StaticStreamDesc ss{};
                ss.stream_id      = stream_id;
                ss.element_type   = element_type;
                ss.components     = components;
                ss.layout         = layout;
                ss.bytes_per_elem = bytes_per_elem;
                ss.reserved       = 0u;
                ss.name_len       = name_len;
                ss.extra_len      = extra_len;
                std::memcpy(p + sizeof(TLV), &ss, sizeof(StaticStreamDesc));
                std::memcpy(p + sizeof(TLV) + sizeof(StaticStreamDesc), name_utf8.data(), name_len);
                if (extra_len) std::memcpy(p + sizeof(TLV) + sizeof(StaticStreamDesc) + name_len, extra.data(), extra_len);
                out.insert(out.end(), tmp.begin(), tmp.end());
            }
            return static_cast<std::uint32_t>(out.size());
        }
        [[nodiscard]] std::uint64_t reader_id_of(std::uint32_t idx) const noexcept {
            auto* RS = reinterpret_cast<ReaderSlot*>(map_.data() + readers_off_ + idx * hdr_->reader_slot_stride);
            return RS->reader_id.load(std::memory_order_acquire);
        }

        Map map_;
        GlobalHeader* hdr_       = nullptr;
        std::uint32_t slots_off_ = 0, readers_off_ = 0;
        std::vector<std::uint8_t> static_dir_;
        std::uint64_t session_id_ = 0;
    };
} // namespace shmx
#endif // SHMX_SERVER_H
