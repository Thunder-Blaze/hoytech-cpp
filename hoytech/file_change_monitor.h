#pragma once

#include <atomic>
#include <string>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <thread>
#include <functional>
#include <sys/types.h>
#include <sys/stat.h>

#include "hoytech/error.h"
#include "hoytech/time.h"
#include "hoytech/detail/watch_result.h"

// Platform backend selection

#if defined(__linux__)
#  include "hoytech/detail/inotify_watcher.h"
   namespace hoytech { namespace detail { using platform_watcher = inotify_watcher; } }

#elif defined(__APPLE__)  || defined(__FreeBSD__) || defined(__OpenBSD__) \
   || defined(__NetBSD__) || defined(__DragonFly__)
#  include "hoytech/detail/kqueue_watcher.h"
   namespace hoytech { namespace detail { using platform_watcher = kqueue_watcher; } }

#else
#  include "hoytech/detail/polling_watcher.h"
   namespace hoytech { namespace detail { using platform_watcher = polling_watcher; } }

#endif

// Platform-agnostic file change monitor

namespace hoytech {


class file_change_monitor {
  private:
    std::string watched_path;
    uint64_t debounce_us = 50'000;
    int shutdown_pipe[2] = {-1, -1};
    std::atomic<bool> shutdown{false};
    std::thread t;
    detail::platform_watcher watcher;

    // Inode tracking for file replacement detection
    dev_t stored_dev = 0;
    ino_t stored_ino = 0;
    bool inode_valid = false;
    uint64_t inode_check_interval_ms = 5'000; // 0 = disabled

    void update_stored_inode() {
        struct stat st;
        if (::stat(watched_path.c_str(), &st) == 0) {
            stored_dev = st.st_dev;
            stored_ino = st.st_ino;
            inode_valid = true;
        } else {
            inode_valid = false;
        }
    }
    bool has_inode_changed() {
        struct stat st;
        if (::stat(watched_path.c_str(), &st) != 0) {
            // File gone — changed if it previously existed
            return inode_valid;
        }
        if (!inode_valid) {
            // File appeared — treat as changed
            return true;
        }
        return st.st_dev != stored_dev || st.st_ino != stored_ino;
    }
    void close_pipe() {
        if (shutdown_pipe[0] != -1) { ::close(shutdown_pipe[0]); shutdown_pipe[0] = -1; }
        if (shutdown_pipe[1] != -1) { ::close(shutdown_pipe[1]); shutdown_pipe[1] = -1; }
    }

  public:
    explicit file_change_monitor(std::string path) : watched_path(std::move(path)) {
        if (::pipe(shutdown_pipe) < 0)
            throw hoytech::error("unable to create shutdown pipe: ", ::strerror(errno));
        ::fcntl(shutdown_pipe[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(shutdown_pipe[1], F_SETFD, FD_CLOEXEC);

        watcher.init(watched_path, shutdown_pipe[0]);
        update_stored_inode();
    }

    void setDebounce(uint64_t ms) {
        debounce_us = ms * 1000;
    }

    void setPollInterval(uint64_t ms) {
        watcher.setPollInterval(ms);
    }

    void setMaxRewatchAttempts(int n) {
        watcher.setMaxRewatchAttempts(n);
    }

    void setRewatchBackoff(uint64_t initial_us) {
        watcher.setRewatchBackoff(initial_us);
    }

    /// Set how often to stat() the file to detect inode changes (file replacement).
    /// Set to 0 to disable. Default: 5000ms.
    void setInodeCheckInterval(uint64_t ms) {
        inode_check_interval_ms = ms;
    }

    void run(std::function<void()> cb) {
        if (shutdown) throw hoytech::error("file watcher already shutdown");

        t = std::thread([cb = std::move(cb), this]() {
            uint64_t trigger_time = 0;
            uint64_t last_inode_check_us = hoytech::curr_time_us();

            while (true) {
                int timeout_ms = -1;
                uint64_t now = hoytech::curr_time_us();

                if (trigger_time) {
                    if (now >= trigger_time) {
                        trigger_time = 0;
                        cb();
                        now = hoytech::curr_time_us();
                        continue;
                    }

                    timeout_ms = static_cast<int>((trigger_time - now) / 1000);
                    if (timeout_ms == 0) timeout_ms = 1; // Avoid busy-spin
                }

                // Cap timeout to inode check interval so we periodically stat()
                if (inode_check_interval_ms > 0) {
                    uint64_t next_check_us = last_inode_check_us + inode_check_interval_ms * 1000;
                    int inode_timeout = 0;
                    if (next_check_us > now) {
                        inode_timeout = static_cast<int>((next_check_us - now) / 1000);
                        if (inode_timeout == 0) inode_timeout = 1;
                    }
                    if (timeout_ms < 0 || inode_timeout < timeout_ms) {
                        timeout_ms = inode_timeout;
                    }
                }

                auto result = watcher.wait_for_event(timeout_ms);
                if (shutdown) return;

                now = hoytech::curr_time_us();

                if (inode_check_interval_ms > 0 && (now - last_inode_check_us >= inode_check_interval_ms * 1000)) {
                    last_inode_check_us = now;
                    if (has_inode_changed()) {
                        try {
                            watcher.rewatch();
                            update_stored_inode();
                        } catch (const std::exception &e) {
                            ::fprintf(stderr, "file_change_monitor: inode-triggered rewatch failed on '%s': %s\n",
                                      watched_path.c_str(), e.what());
                        } catch (...) {
                            ::fprintf(stderr, "file_change_monitor: inode-triggered rewatch failed on '%s': unknown error\n",
                                      watched_path.c_str());
                        }
                        if (trigger_time == 0) trigger_time = hoytech::curr_time_us() + debounce_us;
                        continue;
                    }
                }

                switch (result) {
                    case detail::watch_result::shutdown:
                        return;

                    case detail::watch_result::rewatch_needed:
                        try {
                            watcher.rewatch();
                            update_stored_inode();
                        } catch (const std::exception &e) {
                            ::fprintf(stderr, "file_change_monitor: rewatch failed on '%s': %s\n",
                                      watched_path.c_str(), e.what());
                        } catch (...) {
                            ::fprintf(stderr, "file_change_monitor: rewatch failed on '%s': unknown error\n",
                                      watched_path.c_str());
                        }
                        [[fallthrough]];

                    case detail::watch_result::changed:
                        update_stored_inode();
                        if (trigger_time == 0) trigger_time = hoytech::curr_time_us() + debounce_us;
                        break;

                    case detail::watch_result::timeout:
                        break;
                }
            }
        });
    }

    ~file_change_monitor() {
        shutdown = true;

        if (t.joinable()) {
            char byte = 1;
            int rv = ::write(shutdown_pipe[1], &byte, 1);
            (void)rv;

            t.join();
        }

        close_pipe();
    }

    file_change_monitor(const file_change_monitor &) = delete;
    file_change_monitor &operator=(const file_change_monitor &) = delete;
};


} // namespace hoytech
