#pragma once

#include <string>
#include <chrono>
#include <filesystem>
#include <poll.h>
#include <stdint.h>

#include "hoytech/detail/watch_result.h"


namespace hoytech {
namespace detail {


class polling_watcher {
  private:
    std::string watched_path;
    int shutdown_read_fd = -1;
    uint64_t poll_interval_ms = 100;
    std::filesystem::file_time_type last_write_time = std::filesystem::file_time_type::min();

  public:
    polling_watcher() = default;

    void init(const std::string &path, int shutdown_fd) {
        watched_path = path;
        shutdown_read_fd = shutdown_fd;

        try {
            last_write_time = std::filesystem::last_write_time(watched_path);
        } catch (...) {
            last_write_time = std::filesystem::file_time_type::min();
        }
    }

    ~polling_watcher() = default;

    watch_result wait_for_event(int timeout_ms) {
        // Determine how long to sleep: the minimum of the requested timeout
        // and our polling interval, so we check the file periodically.
        int sleep_ms = static_cast<int>(poll_interval_ms);
        if (timeout_ms >= 0 && timeout_ms < sleep_ms) {
            sleep_ms = timeout_ms;
        }

        // Use poll() on the shutdown pipe so we wake instantly on shutdown
        struct pollfd pfd = { shutdown_read_fd, POLLIN, 0 };
        int rv = ::poll(&pfd, 1, sleep_ms);

        if (rv > 0 && (pfd.revents & POLLIN)) return watch_result::shutdown;

        // Check filesystem for changes
        std::filesystem::file_time_type current_write_time = std::filesystem::file_time_type::min();
        try {
            current_write_time = std::filesystem::last_write_time(watched_path);
        } catch (...) {
            if (last_write_time != std::filesystem::file_time_type::min()) {
                last_write_time = std::filesystem::file_time_type::min();
                return watch_result::changed;
            }
            return watch_result::timeout;
        }

        if (current_write_time != last_write_time) {
            last_write_time = current_write_time;
            return watch_result::changed;
        }

        return watch_result::timeout;
    }

    void rewatch() {
        try {
            last_write_time = std::filesystem::last_write_time(watched_path);
        } catch (...) {
            last_write_time = std::filesystem::file_time_type::min();
        }
    }

    void setPollInterval(uint64_t ms) {
        if (ms > 0) poll_interval_ms = ms;
    }
    void setMaxRewatchAttempts(int) {}  // No retry needed for polling backend
    void setRewatchBackoff(uint64_t) {} // No retry needed for polling backend

    polling_watcher(const polling_watcher &) = delete;
    polling_watcher &operator=(const polling_watcher &) = delete;
};


} // namespace detail
} // namespace hoytech
