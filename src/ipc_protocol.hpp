#pragma once

#include <cstddef>
#include <cstdint>
#include <semaphore.h>

namespace ipc {
inline constexpr char shared_memory_name[] = "/test_prodcons_packets";
inline constexpr std::uint32_t protocol_magic = 0x50434B54;
inline constexpr std::uint32_t protocol_version = 1;
inline constexpr std::size_t shared_memory_budget = 64U * 1024U * 1024U;
inline constexpr std::size_t maximum_payload_size = 16U * 1024U * 1024U;
inline constexpr std::size_t maximum_slot_count = 256;

struct PacketMetadata {
    std::uint64_t timestamp_ns;
    std::uint64_t sequence_number;
    std::uint32_t checksum;
    std::uint32_t payload_size;
};

struct alignas(64) SharedHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t slot_count;
    std::uint64_t slot_size;
    std::uint64_t payload_size;
    alignas(64) std::uint64_t producer_index;
    alignas(64) std::uint64_t consumer_index;
    sem_t free_slots;
    sem_t ready_slots;
};

inline std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

inline std::byte* slot_at(SharedHeader* header, std::uint64_t index) {
    auto* slots = reinterpret_cast<std::byte*>(header) + sizeof(SharedHeader);
    return slots + (index % header->slot_count) * header->slot_size;
}
} // namespace ipc
