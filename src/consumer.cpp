#include "crc32.hpp"
#include "ipc_protocol.hpp"
#include "process_control.hpp"
#include "process_lock.hpp"
#include "semaphore_utils.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace {
struct Statistics {
    std::uint64_t total_packets = 0;
    std::uint64_t interval_packets = 0;
    std::uint64_t interval_bytes = 0;
    std::uint64_t checksum_errors = 0;
    std::uint64_t metadata_errors = 0;
    std::uint64_t sequence_errors = 0;
    std::uint64_t expected_sequence = 0;
    bool has_sequence = false;
};

void report_if_due(Statistics& statistics,
                   std::chrono::steady_clock::time_point& last_report) {
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - last_report;
    if (elapsed < std::chrono::seconds(1)) return;

    const double packets_per_second = statistics.interval_packets / elapsed.count();
    const double bytes_per_second = statistics.interval_bytes / elapsed.count();
    std::cout << "total=" << statistics.total_packets
              << " packets/s=" << std::fixed << std::setprecision(1)
              << packets_per_second << " bytes/s=" << bytes_per_second
              << " checksum_errors=" << statistics.checksum_errors
              << " metadata_errors=" << statistics.metadata_errors
              << " sequence_errors=" << statistics.sequence_errors << '\n';

    statistics.interval_packets = 0;
    statistics.interval_bytes = 0;
    last_report = now;
}

bool valid_layout(const ipc::SharedHeader& header, std::size_t mapping_size) {
    if (header.magic != ipc::protocol_magic || header.version != ipc::protocol_version ||
        header.slot_count < 2 || header.slot_size < sizeof(ipc::PacketMetadata)) {
        return false;
    }
    if (header.payload_size == 0 || header.payload_size > ipc::maximum_payload_size ||
        header.payload_size > header.slot_size - sizeof(ipc::PacketMetadata)) {
        return false;
    }
    if (header.slot_count >
        (std::numeric_limits<std::size_t>::max() - sizeof(ipc::SharedHeader)) /
            header.slot_size) {
        return false;
    }
    return sizeof(ipc::SharedHeader) + header.slot_count * header.slot_size <= mapping_size;
}

struct SharedMemoryIdentity {
    dev_t device = 0;
    ino_t inode = 0;
};

bool shared_memory_was_replaced(const SharedMemoryIdentity& identity) {
    const int fd = shm_open(ipc::shared_memory_name, O_RDONLY, 0);
    if (fd == -1) return errno == ENOENT;

    struct stat status {};
    const bool replaced = fstat(fd, &status) == -1 ||
                          status.st_dev != identity.device ||
                          status.st_ino != identity.inode;
    close(fd);
    return replaced;
}
} // namespace

int main() {
    control::ProcessInstanceLock instance_lock("consumer");
    if (!instance_lock.acquired()) {
        if (instance_lock.another_instance()) {
            std::cerr << "Another Consumer is already running.\n";
        }
        return 1;
    }

    control::ProcessControl process_control("Reception");
    Statistics statistics;
    bool failed = false;
    bool controls_announced = false;
    while (!process_control.stop_requested()) {
        std::cout << "Waiting for Producer at " << ipc::shared_memory_name << "...\n";

        void* mapping = MAP_FAILED;
        std::size_t mapping_size = 0;
        ipc::SharedHeader* header = nullptr;
        SharedMemoryIdentity identity;
        while (!process_control.stop_requested() && header == nullptr) {
            process_control.update();
            if (process_control.paused()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            const int fd = shm_open(ipc::shared_memory_name, O_RDWR, 0);
            if (fd == -1) {
                if (errno != ENOENT) {
                    std::perror("shm_open");
                    failed = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            struct stat status {};
            if (fstat(fd, &status) == -1) {
                std::perror("fstat");
                close(fd);
                failed = true;
                break;
            }
            if (status.st_size < static_cast<off_t>(sizeof(ipc::SharedHeader))) {
                close(fd);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            mapping_size = static_cast<std::size_t>(status.st_size);
            mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (mapping == MAP_FAILED) {
                std::perror("mmap");
                failed = true;
                break;
            }

            auto* candidate = static_cast<ipc::SharedHeader*>(mapping);
            if (candidate->magic == 0) {
                munmap(mapping, mapping_size);
                mapping = MAP_FAILED;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (!valid_layout(*candidate, mapping_size)) {
                std::cerr << "Shared memory contains an incompatible or invalid protocol header.\n";
                munmap(mapping, mapping_size);
                failed = true;
                break;
            }
            header = candidate;
            identity = {status.st_dev, status.st_ino};
        }

        if (failed || process_control.stop_requested()) break;

        std::cout << "Receiving packets from " << ipc::shared_memory_name << ".\n";
        if (!controls_announced) {
            std::cout << "SIGUSR1 pauses, SIGUSR2 resumes, Ctrl+C stops.";
            if (process_control.keyboard_active()) {
                std::cout << " Any key toggles pause/resume.";
            }
            std::cout << '\n';
            controls_announced = true;
        }

        statistics.interval_packets = 0;
        statistics.interval_bytes = 0;
        statistics.has_sequence = false;
        auto last_report = std::chrono::steady_clock::now();
        bool reconnect = false;
        while (!process_control.stop_requested()) {
            const bool was_paused = process_control.paused();
            process_control.update();
            if (process_control.paused()) {
                if (!was_paused) {
                    statistics.interval_packets = 0;
                    statistics.interval_bytes = 0;
                }
                last_report = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (was_paused) {
                statistics.interval_packets = 0;
                statistics.interval_bytes = 0;
                last_report = std::chrono::steady_clock::now();
            }
            report_if_due(statistics, last_report);

            const ipc::SemaphoreWaitResult wait_result = ipc::timed_wait(&header->ready_slots);
            if (wait_result == ipc::SemaphoreWaitResult::retry) {
                if (shared_memory_was_replaced(identity)) {
                    std::cout << "Producer disconnected; reconnecting.\n";
                    reconnect = true;
                    break;
                }
                continue;
            }
            if (wait_result == ipc::SemaphoreWaitResult::error) {
                failed = true;
                break;
            }

            std::byte* slot = ipc::slot_at(header, header->consumer_index);
            const auto* metadata = reinterpret_cast<const ipc::PacketMetadata*>(slot);
            const std::size_t payload_capacity = header->slot_size - sizeof(*metadata);
            const bool metadata_valid = metadata->timestamp_ns != 0 &&
                                        metadata->payload_size == header->payload_size &&
                                        metadata->payload_size <= payload_capacity;

            ++statistics.total_packets;
            ++statistics.interval_packets;
            if (!metadata_valid) {
                ++statistics.metadata_errors;
            } else {
                statistics.interval_bytes += metadata->payload_size;
                const std::byte* payload = slot + sizeof(*metadata);
                if (checksum::crc32(payload, metadata->payload_size) != metadata->checksum) {
                    ++statistics.checksum_errors;
                }

                if (statistics.has_sequence &&
                    metadata->sequence_number != statistics.expected_sequence) {
                    ++statistics.sequence_errors;
                }
                statistics.expected_sequence = metadata->sequence_number + 1;
                statistics.has_sequence = true;
            }

            ++header->consumer_index;
            sem_post(&header->free_slots);
        }

        munmap(mapping, mapping_size);
        if (failed || !reconnect) break;
    }

    std::cout << "Stopped after receiving " << statistics.total_packets << " packets.\n";
    return failed ? 1 : 0;
}
