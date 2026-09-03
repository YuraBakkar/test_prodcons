#pragma once

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <sys/file.h>

namespace control {

class ProcessInstanceLock {
public:
    explicit ProcessInstanceLock(std::string_view process_name) {
        const std::string path = "/tmp/test_prodcons_" + std::string(process_name) + "_" +
                                 std::to_string(static_cast<unsigned long long>(getuid())) +
                                 ".lock";
        descriptor_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor_ == -1) {
            std::perror("open process lock");
            return;
        }
        if (flock(descriptor_, LOCK_EX | LOCK_NB) == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                another_instance_ = true;
            } else {
                std::perror("flock process lock");
            }
            close(descriptor_);
            descriptor_ = -1;
        }
    }

    ~ProcessInstanceLock() {
        if (descriptor_ != -1) close(descriptor_);
    }

    ProcessInstanceLock(const ProcessInstanceLock&) = delete;
    ProcessInstanceLock& operator=(const ProcessInstanceLock&) = delete;

    bool acquired() const { return descriptor_ != -1; }
    bool another_instance() const { return another_instance_; }

private:
    int descriptor_ = -1;
    bool another_instance_ = false;
};

} // namespace control
