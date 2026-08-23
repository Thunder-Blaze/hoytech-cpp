#pragma once

#include <string>
#include <chrono>
#include <filesystem>
#include <stdint.h>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "hoytech/detail/watch_result.h"


namespace hoytech {
namespace detail {


class polling_watcher {
  private:
    std::string watched_path;
    std::mutex shutdown_mutex;
    std::condition_variable shutdown_cv;
    bool is_shutdown = false;
    uint64_t poll_interval_ms = 100;
    std::filesystem::file_time_type last_write_time = std::filesystem::file_time_type::min();

  public:
    polling_watcher() = default;

    void init(const std::string &path) {
        watched_path = path;
        is_shutdown = false;
        try {
            last_write_time = std::filesystem::last_write_time(watched_path);
        } catch (...) {
            last_write_time = std::filesystem::file_time_type::min();
        }
    }

    ~polling_watcher() { shutdown(); }

    void shutdown() {
        std::lock_guard<std::mutex> lock(shutdown_mutex);
        is_shutdown = true;
        shutdown_cv.notify_all();
    }

    watch_result wait_for_event(int timeout_ms) {
        // Determine how long to sleep: the minimum of the requested timeout
        // and our polling interval, so we check the file periodically.
        int sleep_ms = static_cast<int>(poll_interval_ms);
        if (timeout_ms >= 0 && timeout_ms < sleep_ms) {
            sleep_ms = timeout_ms;
        }

        {
            std::unique_lock<std::mutex> lock(shutdown_mutex);
            if (shutdown_cv.wait_for(lock, std::chrono::milliseconds(sleep_ms), [this] { return is_shutdown; })) {
                return watch_result::shutdown;
            }
        }

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
