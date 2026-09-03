#include "crc32.hpp"
#include "ipc_protocol.hpp"
#include "process_control.hpp"
#include "semaphore_utils.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace {
void print_usage(std::string_view program_name) {
    std::cerr << "Usage: " << program_name << " <payload_size_bytes>\n";
}

bool parse_payload_size(std::string_view argument, std::size_t& payload_size) {
    if (argument.empty() || argument.front() == '-') return false;

    unsigned long long value = 0;
    const char* begin = argument.data();
    const char* end = begin + argument.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc{} || result.ptr != end || value == 0 ||
        value > ipc::maximum_payload_size ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    payload_size = static_cast<std::size_t>(value);
    return true;
}

void fill_random_bytes(std::byte* data, std::size_t size, std::mt19937_64& engine) {
    std::size_t offset = 0;
    while (offset + sizeof(std::uint64_t) <= size) {
        const std::uint64_t value = engine();
        std::memcpy(data + offset, &value, sizeof(value));
        offset += sizeof(value);
    }
    if (offset < size) {
        const std::uint64_t value = engine();
        std::memcpy(data + offset, &value, size - offset);
    }
}

std::uint64_t current_timestamp_ns() {
    const auto duration = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::size_t payload_size = 0;
    if (!parse_payload_size(argv[1], payload_size)) {
        std::cerr << "Error: payload size must be between 1 and "
                  << ipc::maximum_payload_size << " bytes.\n";
        print_usage(argv[0]);
        return 1;
    }

    const std::size_t slot_size = ipc::align_up(
        sizeof(ipc::PacketMetadata) + payload_size, alignof(std::max_align_t));
    const std::size_t available = ipc::shared_memory_budget - sizeof(ipc::SharedHeader);
    const std::size_t slot_count = std::max<std::size_t>(
        2, std::min(ipc::maximum_slot_count, available / slot_size));
    const std::size_t mapping_size = sizeof(ipc::SharedHeader) + slot_count * slot_size;

    const int fd = shm_open(ipc::shared_memory_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        std::perror("shm_open");
        std::cerr << "Another Producer may be running, or stale shared memory exists.\n";
        return 1;
    }
    if (ftruncate(fd, static_cast<off_t>(mapping_size)) == -1) {
        std::perror("ftruncate");
        close(fd);
        shm_unlink(ipc::shared_memory_name);
        return 1;
    }

    void* mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        std::perror("mmap");
        shm_unlink(ipc::shared_memory_name);
        return 1;
    }

    auto* header = new (mapping) ipc::SharedHeader{};
    header->version = ipc::protocol_version;
    header->slot_count = slot_count;
    header->slot_size = slot_size;
    header->payload_size = payload_size;
    if (sem_init(&header->free_slots, 1, static_cast<unsigned int>(slot_count)) == -1 ||
        sem_init(&header->ready_slots, 1, 0) == -1) {
        std::perror("sem_init");
        munmap(mapping, mapping_size);
        shm_unlink(ipc::shared_memory_name);
        return 1;
    }
    header->magic = ipc::protocol_magic;

    control::ProcessControl process_control("Transfer");
    std::random_device random_device;
    std::mt19937_64 random_engine(random_device());
    std::cout << "Producing " << payload_size << "-byte packets in "
              << ipc::shared_memory_name << " (" << slot_count
              << " slots).\n"
              << "SIGUSR1 pauses, SIGUSR2 resumes, Ctrl+C stops.";
    if (process_control.keyboard_active()) std::cout << " Any key toggles pause/resume.";
    std::cout << '\n';

    std::uint64_t sequence_number = 0;
    bool failed = false;
    while (!process_control.stop_requested()) {
        process_control.update();
        if (process_control.paused()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        const ipc::SemaphoreWaitResult wait_result = ipc::timed_wait(&header->free_slots);
        if (wait_result == ipc::SemaphoreWaitResult::retry) continue;
        if (wait_result == ipc::SemaphoreWaitResult::error) {
            failed = true;
            break;
        }

        std::byte* slot = ipc::slot_at(header, header->producer_index);
        auto* metadata = reinterpret_cast<ipc::PacketMetadata*>(slot);
        std::byte* payload = slot + sizeof(*metadata);

        fill_random_bytes(payload, payload_size, random_engine);
        metadata->timestamp_ns = current_timestamp_ns();
        metadata->sequence_number = sequence_number++;
        metadata->payload_size = static_cast<std::uint32_t>(payload_size);
        metadata->checksum = checksum::crc32(payload, payload_size);

        ++header->producer_index;
        sem_post(&header->ready_slots);
    }

    std::cout << "Stopped after producing " << sequence_number << " packets.\n";
    munmap(mapping, mapping_size);
    shm_unlink(ipc::shared_memory_name);
    return failed ? 1 : 0;
}
