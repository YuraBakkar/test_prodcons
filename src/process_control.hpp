#pragma once

#include "terminal_input.hpp"

#include <csignal>
#include <iostream>
#include <string_view>

namespace control {

class ProcessControl {
public:
    explicit ProcessControl(std::string_view activity_name)
        : activity_name_(activity_name) {
        std::signal(SIGINT, handle_stop_signal);
        std::signal(SIGTERM, handle_stop_signal);
        std::signal(SIGUSR1, handle_pause_signal);
        std::signal(SIGUSR2, handle_resume_signal);
    }

    void update() {
        bool new_state = paused_;
        if (requested_state_ == 1) new_state = true;
        if (requested_state_ == 2) new_state = false;
        requested_state_ = 0;
        if (terminal_.key_pressed()) new_state = !new_state;

        if (new_state != paused_) {
            paused_ = new_state;
            std::cout << activity_name_ << (paused_ ? " paused.\n" : " resumed.\n")
                      << std::flush;
        }
    }

    bool stop_requested() const { return stop_requested_ != 0; }
    bool paused() const { return paused_; }
    bool keyboard_active() const { return terminal_.active(); }

private:
    static void handle_stop_signal(int) { stop_requested_ = 1; }
    static void handle_pause_signal(int) { requested_state_ = 1; }
    static void handle_resume_signal(int) { requested_state_ = 2; }

    inline static volatile std::sig_atomic_t stop_requested_ = 0;
    inline static volatile std::sig_atomic_t requested_state_ = 0;

    std::string_view activity_name_;
    console::TerminalInput terminal_;
    bool paused_ = false;
};

} // namespace control
