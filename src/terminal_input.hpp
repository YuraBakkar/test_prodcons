#pragma once

#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace console {

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

} // namespace console
