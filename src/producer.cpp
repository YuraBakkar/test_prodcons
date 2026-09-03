#include "ipc_protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
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
#include <termios.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t requested_state = 0;

void handle_stop_signal(int) { stop_requested = 1; }
void handle_pause_signal(int) { requested_state = 1; }
void handle_resume_signal(int) { requested_state = 2; }

class TerminalInput {
public:
    TerminalInput() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original_termios_) == -1) {
            return;
        }

        termios raw = original_termios_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        original_flags_ = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (original_flags_ == -1 || tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1 ||
            fcntl(STDIN_FILENO, F_SETFL, original_flags_ | O_NONBLOCK) == -1) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_termios_);
            return;
        }
        active_ = true;
    }

    ~TerminalInput() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_termios_);
            fcntl(STDIN_FILENO, F_SETFL, original_flags_);
        }
    }

    TerminalInput(const TerminalInput&) = delete;
    TerminalInput& operator=(const TerminalInput&) = delete;

    bool key_pressed() const {
        if (!active_) return false;

        bool received_input = false;
        char input[64];
        while (true) {
            const ssize_t bytes_read = read(STDIN_FILENO, input, sizeof(input));
            if (bytes_read > 0) {
                received_input = true;
                continue;
            }
            if (bytes_read == -1 && errno == EINTR) continue;
            break;
        }
        return received_input;
    }

    bool active() const { return active_; }

private:
    termios original_termios_{};
    int original_flags_ = 0;
    bool active_ = false;
};

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

std::uint32_t crc32(const std::byte* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= std::to_integer<std::uint8_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
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

enum class WaitResult { acquired, retry, error };

WaitResult wait_for_free_slot(sem_t* semaphore) {
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100'000'000;
    if (deadline.tv_nsec >= 1'000'000'000) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1'000'000'000;
    }
    if (sem_timedwait(semaphore, &deadline) == 0) return WaitResult::acquired;
    if (errno == EINTR || errno == ETIMEDOUT) return WaitResult::retry;
    std::perror("sem_timedwait");
    return WaitResult::error;
}

void update_transfer_state(bool& paused, const TerminalInput& terminal) {
    bool new_state = paused;
    if (requested_state == 1) new_state = true;
    if (requested_state == 2) new_state = false;
    requested_state = 0;
    if (terminal.key_pressed()) new_state = !new_state;

    if (new_state != paused) {
        paused = new_state;
        std::cout << (paused ? "Transfer paused.\n" : "Transfer resumed.\n")
                  << std::flush;
    }
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
    header->magic = ipc::protocol_magic;
    header->version = ipc::protocol_version;
    header->slot_count = slot_count;
    header->slot_size = slot_size;
    if (sem_init(&header->free_slots, 1, static_cast<unsigned int>(slot_count)) == -1 ||
        sem_init(&header->ready_slots, 1, 0) == -1) {
        std::perror("sem_init");
        munmap(mapping, mapping_size);
        shm_unlink(ipc::shared_memory_name);
        return 1;
    }

    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);
    std::signal(SIGUSR1, handle_pause_signal);
    std::signal(SIGUSR2, handle_resume_signal);
    TerminalInput terminal;
    std::random_device random_device;
    std::mt19937_64 random_engine(random_device());
    std::cout << "Producing " << payload_size << "-byte packets in "
              << ipc::shared_memory_name << " (" << slot_count
              << " slots).\n"
              << "SIGUSR1 pauses, SIGUSR2 resumes, Ctrl+C stops.";
    if (terminal.active()) std::cout << " Any key toggles pause/resume.";
    std::cout << '\n';

    std::uint64_t sequence_number = 0;
    bool paused = false;
    bool failed = false;
    while (!stop_requested) {
        update_transfer_state(paused, terminal);
        if (paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        const WaitResult wait_result = wait_for_free_slot(&header->free_slots);
        if (wait_result == WaitResult::retry) continue;
        if (wait_result == WaitResult::error) {
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
        metadata->checksum = crc32(payload, payload_size);

        ++header->producer_index;
        sem_post(&header->ready_slots);
    }

    std::cout << "Stopped after producing " << sequence_number << " packets.\n";
    munmap(mapping, mapping_size);
    shm_unlink(ipc::shared_memory_name);
    return failed ? 1 : 0;
}
