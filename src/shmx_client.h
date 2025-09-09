#ifndef SHMX_CLIENT_H
#define SHMX_CLIENT_H
#include "shmx_common.h"
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace shmx {
    struct StaticStreamInfo {
        std::uint32_t id, elem_type, components, layout, bytes_per_elem;
        std::string name;
        std::vector<std::uint8_t> extra;
    };
    struct StaticState {
        std::uint64_t session_id, static_hash;
        std::uint32_t static_gen, payload_bytes;
        std::vector<StaticStreamInfo> dir;
    };
    struct FrameView {
        const FrameHeader* fh{};
        const std::uint8_t* payload{};
        std::uint32_t bytes{};
        bool session_mismatch{};
        std::uint32_t checksum_mismatch{};
    };
    struct DecodedItem {
        const void* ptr;
        std::uint32_t bytes, elem_count;
    };
    struct DecodedFrame {
        std::vector<std::pair<std::uint32_t, DecodedItem>> streams;
    };

    class Client {
    public:
        Client() = default;
        ~Client() {
            close();
        }
        Client(const Client&)            = delete;
        Client& operator=(const Client&) = delete;

        [[nodiscard]] bool open(const std::string& name) {
            close();
            const std::size_t hdr_bytes = align_up(static_cast<std::uint32_t>(sizeof(GlobalHeader)), 64);
            if (!map_.open(name, hdr_bytes)) return false;
            GH_ = reinterpret_cast<GlobalHeader*>(map_.data());
            if (!validate_header_min()) {
                close();
                return false;
            }
            const auto total_bytes = compute_total_bytes(*GH_);
            if (total_bytes == 0) {
                close();
                return false;
            }
            if (!map_.remap(total_bytes)) {
                close();
                return false;
            }
            GH_ = reinterpret_cast<GlobalHeader*>(map_.data());
            if (!validate_header_min()) {
                close();
                return false;
            }
            attach_slot();
            return true;
        }

        void close() noexcept {
            detach_slot();
            map_.close();
            GH_                = nullptr;
            reader_slot_index_ = UINT32_MAX;
            reader_id_         = 0;
        }

        [[nodiscard]] GlobalHeader* header() noexcept {
            GH_ = reinterpret_cast<GlobalHeader*>(map_.data());
            return GH_;
        }

        [[nodiscard]] bool refresh_static(StaticState& out) {
            const auto* GH = header();
            if (!GH || !basic_sanity(*GH)) return false;
            const auto g1     = GH->static_gen.load(std::memory_order_acquire);
            out.session_id    = GH->session_id;
            out.static_gen    = g1;
            out.static_hash   = GH->static_hash;
            out.payload_bytes = GH->static_bytes_used;
            out.dir.clear();
            const std::uint8_t* cur = map_.data() + GH->static_offset;
            const std::uint8_t* end = cur + GH->static_bytes_used;
            while (cur + sizeof(TLV) <= end) {
                TLV tlv{};
                std::memcpy(&tlv, cur, sizeof(TLV));
                const auto tlv_end = cur + sizeof(TLV) + tlv.length;
                if (tlv_end > end) break;
                if (tlv.type == TLV_STATIC_DIR) {
                    if (tlv.length < sizeof(StaticStreamDesc)) break;
                    StaticStreamDesc ss{};
                    std::memcpy(&ss, cur + sizeof(TLV), sizeof(StaticStreamDesc));
                    const auto* pName = reinterpret_cast<const char*>(cur + sizeof(TLV) + sizeof(StaticStreamDesc));
                    if (sizeof(StaticStreamDesc) + ss.name_len + ss.extra_len > tlv.length) break;
                    StaticStreamInfo si{ss.stream_id, ss.element_type, ss.components, ss.layout, ss.bytes_per_elem, std::string(pName, pName + ss.name_len), {}};
                    if (ss.extra_len) {
                        const auto* pExtra = reinterpret_cast<const std::uint8_t*>(pName + ss.name_len);
                        si.extra.assign(pExtra, pExtra + ss.extra_len);
                    }
                    out.dir.emplace_back(std::move(si));
                }
                cur += align_up(static_cast<std::uint32_t>(sizeof(TLV)) + tlv.length, 16);
            }
            const auto g2 = GH->static_gen.load(std::memory_order_acquire);
            return g1 == g2;
        }

        [[nodiscard]] bool latest(FrameView& out) {
            const auto* GH = header();
            if (!GH || !basic_sanity(*GH)) return false;
            if (GH->slots == 0) return false;
            const auto w = GH->write_index.load(std::memory_order_acquire);
            if (w == 0u) return false;
            const auto slot       = (w - 1u) % GH->slots;
            const auto* base_slot = map_.data() + GH->slots_offset + slot * GH->slot_stride;
            const auto* FH        = reinterpret_cast<const FrameHeader*>(base_slot);
            const auto* payload   = base_slot + align_up(static_cast<std::uint32_t>(sizeof(FrameHeader)), 64);
            const auto bytes      = FH->payload_bytes;
            if (bytes == 0 || bytes > GH->frame_bytes_cap) return false;
            const bool mismatch = FH->session_id_copy != GH->session_id;
            if (mismatch) return false;
            const auto calc        = checksum32(payload, bytes);
            const std::uint32_t cm = FH->checksum;
            out                    = FrameView{FH, payload, bytes, false, static_cast<std::uint32_t>(calc != cm)};
            heartbeat_seen(FH->frame_id.load(std::memory_order_acquire));
            return out.checksum_mismatch == 0u;
        }

        [[nodiscard]] static bool decode(const FrameView& fv, DecodedFrame& df) {
            df.streams.clear();
            const auto* cur = fv.payload;
            const auto* end = fv.payload + fv.bytes;
            while (cur + sizeof(TLV) <= end) {
                TLV tlv{};
                std::memcpy(&tlv, cur, sizeof(TLV));
                const auto tlv_end = cur + sizeof(TLV) + tlv.length;
                if (tlv_end > end) break;
                if (tlv.type == TLV_FRAME_STREAM) {
                    if (tlv.length < sizeof(FrameStreamTLV)) break;
                    FrameStreamTLV fs{};
                    std::memcpy(&fs, cur + sizeof(TLV), sizeof(FrameStreamTLV));
                    const auto* body = cur + sizeof(TLV) + sizeof(FrameStreamTLV);
                    const auto have  = static_cast<std::size_t>(end - body);
                    if (have < fs.bytes_payload) break;
                    df.streams.emplace_back(fs.stream_id, DecodedItem{body, fs.bytes_payload, fs.elem_count});
                }
                cur += align_up(static_cast<std::uint32_t>(sizeof(TLV)) + tlv.length, 16);
            }
            return true;
        }

        [[nodiscard]] bool control_send(std::uint32_t tlv_type, const void* data, std::uint32_t bytes) {
            auto* GH = header();
            if (!GH || GH->control_per_reader == 0) return false;
            if (reader_slot_index_ == UINT32_MAX) {
                if (!attach_slot()) return false;
            }
            auto* RS = reinterpret_cast<ReaderSlot*>(map_.data() + GH->readers_offset + reader_slot_index_ * GH->reader_slot_stride);
            RS->heartbeat.store(now_ticks(), std::memory_order_release);
            auto* const CH  = map_.data() + GH->control_offset + reader_slot_index_ * GH->control_stride;
            const auto cap  = GH->control_per_reader;
            auto* const r64 = reinterpret_cast<std::atomic<std::uint64_t>*>(CH);
            auto* const w64 = r64 + 1;
            const auto need = align_up(static_cast<std::uint32_t>(sizeof(TLV)) + bytes, 16);
            const auto rv0  = r64->load(std::memory_order_acquire);
            const auto wv0  = w64->load(std::memory_order_acquire);
            if (((wv0 + need) - rv0) > (cap - 16u)) return false;
            auto wv           = wv0;
            auto off          = 16u + static_cast<std::uint32_t>(wv % (cap - 16u));
            auto space_to_end = cap - off;
            if (need > space_to_end) {
                const auto span_to_end = (cap - 16u) - static_cast<std::uint32_t>(wv % (cap - 16u));
                if (((wv + span_to_end) - rv0) > (cap - 16u)) return false;
                if (space_to_end >= sizeof(TLV)) {
                    TLV pad{};
                    pad.type   = 0u;
                    pad.length = static_cast<std::uint32_t>(space_to_end - sizeof(TLV));
                    std::memcpy(CH + off, &pad, sizeof(TLV));
                    std::atomic_thread_fence(std::memory_order_release);
                }
                wv += span_to_end;
                w64->store(wv, std::memory_order_release);
                off = 16u;
                if (((wv + need) - rv0) > (cap - 16u)) return false;
            }
            TLV tlv{};
            tlv.type   = tlv_type;
            tlv.length = bytes;
            std::memcpy(CH + off, &tlv, sizeof(TLV));
            std::memcpy(CH + off + sizeof(TLV), data, bytes);
            std::atomic_thread_fence(std::memory_order_release);
            wv += need;
            w64->store(wv, std::memory_order_release);
            return true;
        }

    private:
        static std::uint64_t now_ticks() noexcept {
            return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }
        static std::uint64_t make_reader_id() noexcept {
            const auto t          = now_ticks();
            const std::size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
            const int local       = 0;
            const auto mix        = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&local) ^ static_cast<std::uintptr_t>(tid));
            return t ^ (mix * 0x9E3779B97F4A7C15ull);
        }
        void heartbeat_seen(std::uint64_t fid) {
            if (reader_slot_index_ == UINT32_MAX) return;
            const auto* GH = header();
            if (!GH) return;
            auto* RS = reinterpret_cast<ReaderSlot*>(map_.data() + GH->readers_offset + reader_slot_index_ * GH->reader_slot_stride);
            RS->last_frame_seen.store(fid, std::memory_order_release);
            RS->heartbeat.store(now_ticks(), std::memory_order_release);
        }
        static bool basic_sanity(const GlobalHeader& H) noexcept {
            if (H.magic != MAGIC || H.ver_major != VER_MAJOR || H.ver_minor != VER_MINOR || H.endianness != ENDIAN_TAG) return false;
            if (H.slot_stride == 0u || H.slots_offset == 0u || H.frame_bytes_cap == 0u) return false;
            if (H.reader_slot_stride == 0u || H.readers_offset == 0u) return false;
            if (H.control_stride != 0u && (H.control_offset == 0u || H.control_per_reader == 0u)) return false;
            return true;
        }
        static std::size_t compute_total_bytes(const GlobalHeader& H) noexcept {
            if (H.slots == 0u) return 0;
            const std::uint64_t total64 = static_cast<std::uint64_t>(H.slots_offset) + static_cast<std::uint64_t>(H.slots) * static_cast<std::uint64_t>(H.slot_stride);
            if (total64 > std::numeric_limits<std::uint32_t>::max()) return 0;
            return static_cast<std::size_t>(total64);
        }
        [[nodiscard]] bool validate_header_min() const noexcept {
            if (!GH_) return false;
            if (GH_->magic != MAGIC || GH_->ver_major != VER_MAJOR || GH_->ver_minor != VER_MINOR || GH_->endianness != ENDIAN_TAG) return false;
            return true;
        }
        bool attach_slot() {
            auto* GH = header();
            if (!GH) return false;
            if (reader_slot_index_ != UINT32_MAX) return true;
            for (std::uint32_t i = 0; i < GH->reader_slots; ++i) {
                auto* RS             = reinterpret_cast<ReaderSlot*>(map_.data() + GH->readers_offset + i * GH->reader_slot_stride);
                std::uint32_t expect = 0;
                if (RS->in_use.compare_exchange_strong(expect, 1u, std::memory_order_acq_rel)) {
                    reader_slot_index_ = i;
                    reader_id_         = make_reader_id();
                    RS->reader_id.store(reader_id_, std::memory_order_release);
                    RS->heartbeat.store(now_ticks(), std::memory_order_release);
                    GH->readers_connected.fetch_add(1u, std::memory_order_acq_rel);
                    return true;
                }
            }
            return false;
        }
        void detach_slot() {
            auto* GH = header();
            if (!GH) return;
            if (reader_slot_index_ == UINT32_MAX) return;
            auto* RS = reinterpret_cast<ReaderSlot*>(map_.data() + GH->readers_offset + reader_slot_index_ * GH->reader_slot_stride);
            RS->reader_id.store(0u, std::memory_order_release);
            RS->heartbeat.store(0u, std::memory_order_release);
            RS->last_frame_seen.store(0u, std::memory_order_release);
            RS->in_use.store(0u, std::memory_order_release);
            GH->readers_connected.fetch_sub(1u, std::memory_order_acq_rel);
            reader_slot_index_ = UINT32_MAX;
            reader_id_         = 0;
        }

        Map map_;
        GlobalHeader* GH_ = nullptr;
        std::uint32_t reader_slot_index_{UINT32_MAX};
        std::uint64_t reader_id_{0};
    };
} // namespace shmx
#endif // SHMX_CLIENT_H
