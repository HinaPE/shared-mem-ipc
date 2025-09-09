#ifndef SHMX_COMMON_H
#define SHMX_COMMON_H
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace shmx {

    inline constexpr std::uint64_t MAGIC        = 0x48494E415F53484Dull;
    inline constexpr std::uint32_t VER_MAJOR    = 2;
    inline constexpr std::uint32_t VER_MINOR    = 0;
    inline constexpr std::uint32_t ENDIAN_TAG   = 0x01020304u;
    inline constexpr std::uint32_t ALIGN_STATIC = 64;
    inline constexpr std::uint32_t ALIGN_SLOT   = 64;
    inline constexpr std::uint32_t ALIGN_TLV    = 16;

    inline constexpr std::uint32_t TLV_STATIC_DIR   = 0x1000;
    inline constexpr std::uint32_t TLV_FRAME_STREAM = 0x2000;
    inline constexpr std::uint32_t TLV_CONTROL_USER = 0x3000;

    inline constexpr std::uint32_t DT_BOOL = 1;
    inline constexpr std::uint32_t DT_I8   = 2;
    inline constexpr std::uint32_t DT_U8   = 3;
    inline constexpr std::uint32_t DT_I16  = 4;
    inline constexpr std::uint32_t DT_U16  = 5;
    inline constexpr std::uint32_t DT_I32  = 6;
    inline constexpr std::uint32_t DT_U32  = 7;
    inline constexpr std::uint32_t DT_I64  = 8;
    inline constexpr std::uint32_t DT_U64  = 9;
    inline constexpr std::uint32_t DT_F16  = 10;
    inline constexpr std::uint32_t DT_BF16 = 11;
    inline constexpr std::uint32_t DT_F32  = 12;
    inline constexpr std::uint32_t DT_F64  = 13;

    inline constexpr std::uint32_t LAYOUT_SOA_SCALAR = 0;
    inline constexpr std::uint32_t LAYOUT_AOS_VECTOR = 1;

    constexpr std::uint32_t align_up(std::uint32_t x, std::uint32_t a) noexcept {
        return (x + (a - 1u)) & ~(a - 1u);
    }

    inline std::uint64_t fnv1a64(const void* data, std::size_t n) noexcept {
        const auto* p   = static_cast<const std::uint8_t*>(data);
        std::uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

#pragma pack(push, 1)
    struct TLV {
        std::uint32_t type;
        std::uint32_t length;
    };
    struct StaticStreamDesc {
        std::uint32_t stream_id, element_type, components, layout, bytes_per_elem, reserved, name_len, extra_len;
    };
    struct FrameStreamTLV {
        std::uint32_t stream_id, elem_count, bytes_payload, reserved;
    };
#pragma pack(pop)

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) GlobalHeader {
        std::uint64_t magic;
        std::uint32_t ver_major, ver_minor, endianness;
        std::uint64_t session_id;
        std::atomic<std::uint32_t> static_gen;
        std::uint64_t static_hash;
        std::uint32_t static_offset, static_bytes_cap, static_bytes_used;
        std::uint32_t slots, slot_stride, slots_offset, frame_bytes_cap;
        std::uint32_t reader_slots, reader_slot_stride, readers_offset;
        std::uint32_t control_offset, control_per_reader, control_stride;
        std::atomic<std::uint64_t> frame_seq;
        std::atomic<std::uint32_t> write_index;
        std::atomic<std::uint32_t> readers_connected;
    };
    struct alignas(64) FrameHeader {
        std::uint64_t session_id_copy;
        std::atomic<std::uint64_t> frame_id;
        double sim_time;
        std::uint32_t payload_bytes, tlv_count;
        std::uint64_t reserved[3];
    };
    struct alignas(64) ReaderSlot {
        std::atomic<std::uint64_t> reader_id, heartbeat, last_frame_seen;
        std::atomic<std::uint32_t> in_use;
        std::uint32_t pad;
        std::uint64_t reserved[4];
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    class Map {
    public:
        Map() = default;
        ~Map() {
            close();
        }
        Map(const Map&)            = delete;
        Map& operator=(const Map&) = delete;

        [[nodiscard]] bool create(std::string_view name, std::size_t bytes) noexcept {
#if defined(_WIN32)
            const std::wstring w = widen(name);
            hMap_                = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<::DWORD>((static_cast<unsigned long long>(bytes)) >> 32u), static_cast<::DWORD>((static_cast<unsigned long long>(bytes)) & 0xFFFFFFFFull), w.c_str());
            if (!hMap_) return false;
            base_ = static_cast<std::uint8_t*>(::MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
            if (!base_) {
                ::CloseHandle(hMap_);
                hMap_ = nullptr;
                return false;
            }
#else
            const std::string nm = normalize(name);
            fd_                  = ::shm_open(nm.c_str(), O_CREAT | O_RDWR, 0666);
            if (fd_ < 0) return false;
            if (::ftruncate(fd_, static_cast<off_t>(bytes)) != 0) {
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            base_ = static_cast<std::uint8_t*>(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
            if (base_ == MAP_FAILED) {
                ::close(fd_);
                fd_   = -1;
                base_ = nullptr;
                return false;
            }
#endif
            size_ = bytes;
            return true;
        }

        [[nodiscard]] bool open(std::string_view name, std::size_t bytes) noexcept {
#if defined(_WIN32)
            const std::wstring w = widen(name);
            hMap_                = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, w.c_str());
            if (!hMap_) return false;
            base_ = static_cast<std::uint8_t*>(::MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
            if (!base_) {
                ::CloseHandle(hMap_);
                hMap_ = nullptr;
                return false;
            }
            size_ = bytes;
#else
            const std::string nm = normalize(name);
            fd_                  = ::shm_open(nm.c_str(), O_RDWR, 0666);
            if (fd_ < 0) return false;
            base_ = static_cast<std::uint8_t*>(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
            if (base_ == MAP_FAILED) {
                ::close(fd_);
                fd_   = -1;
                base_ = nullptr;
                return false;
            }
            size_ = bytes;
#endif
            return true;
        }

        [[nodiscard]] bool remap(std::size_t bytes) noexcept {
#if defined(_WIN32)
            if (!hMap_) return false;
            if (base_) {
                ::UnmapViewOfFile(base_);
                base_ = nullptr;
            }
            base_ = static_cast<std::uint8_t*>(::MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
            if (!base_) return false;
            size_ = bytes;
#else
            if (fd_ < 0) return false;
            if (base_) {
                ::munmap(base_, size_);
                base_ = nullptr;
            }
            base_ = static_cast<std::uint8_t*>(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
            if (base_ == MAP_FAILED) {
                base_ = nullptr;
                return false;
            }
            size_ = bytes;
#endif
            return true;
        }

        void close() noexcept {
#if defined(_WIN32)
            if (base_) {
                ::UnmapViewOfFile(base_);
                base_ = nullptr;
            }
            if (hMap_) {
                ::CloseHandle(hMap_);
                hMap_ = nullptr;
            }
#else
            if (base_) {
                ::munmap(base_, size_);
                base_ = nullptr;
            }
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
#endif
            size_ = 0;
        }

        [[nodiscard]] std::uint8_t* data() const noexcept {
            return base_;
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }

    private:
#if defined(_WIN32)
        static std::wstring widen(std::string_view s) {
            if (s.empty()) return {};
            const int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<std::size_t>(len), L'\0');
            if (len > 0) {
                ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
            }
            return w;
        }
#else
        static std::string normalize(std::string_view in) {
            if (!in.empty() && in.front() == '/') return std::string(in);
            std::string r;
            r.reserve(in.size() + 1);
            r.push_back('/');
            r.append(in);
            return r;
        }
#endif

        std::uint8_t* base_ = nullptr;
        std::size_t size_   = 0;
#if defined(_WIN32)
        ::HANDLE hMap_ = nullptr;
#else
        int fd_ = -1;
#endif
    };

} // namespace shmx
#endif // SHMX_COMMON_H
