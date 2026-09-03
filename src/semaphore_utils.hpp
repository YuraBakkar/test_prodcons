#pragma once

#include <cerrno>
#include <cstdio>
#include <semaphore.h>
#include <time.h>

namespace ipc {

enum class SemaphoreWaitResult { acquired, retry, error };

inline SemaphoreWaitResult timed_wait(sem_t* semaphore) {
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100'000'000;
    if (deadline.tv_nsec >= 1'000'000'000) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1'000'000'000;
    }

    if (sem_timedwait(semaphore, &deadline) == 0) {
        return SemaphoreWaitResult::acquired;
    }
    if (errno == EINTR || errno == ETIMEDOUT) {
        return SemaphoreWaitResult::retry;
    }

    std::perror("sem_timedwait");
    return SemaphoreWaitResult::error;
}

} // namespace ipc
