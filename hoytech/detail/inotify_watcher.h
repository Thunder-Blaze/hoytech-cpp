#pragma once

#include <string>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <poll.h>
#include <sys/inotify.h>

#include "hoytech/error.h"
#include "hoytech/detail/watch_result.h"


namespace hoytech {
namespace detail {


class inotify_watcher {
  private:
    std::string watched_path;
    int shutdown_read_fd = -1;
    int inotify_fd = -1;
    int inotify_wd = -1;
    int max_rewatch_attempts = 10;
    uint64_t rewatch_backoff_us = 5'000;

    void add_watch() {
        inotify_wd = ::inotify_add_watch(inotify_fd, watched_path.c_str(),
            IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
        if (inotify_wd < 0) throw hoytech::error("unable to add inotify watch on '", watched_path, "': ", ::strerror(errno));
    }

  public:
    inotify_watcher() = default;

    void init(const std::string &path, int shutdown_fd) {
        watched_path = path;
        shutdown_read_fd = shutdown_fd;

        inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd < 0) throw hoytech::error("unable to create inotify descriptor: ", ::strerror(errno));

        add_watch();
    }

    ~inotify_watcher() {
        if (inotify_wd != -1 && inotify_fd != -1) {
            ::inotify_rm_watch(inotify_fd, inotify_wd);
            inotify_wd = -1;
        }
        if (inotify_fd != -1) {
            ::close(inotify_fd);
            inotify_fd = -1;
        }
    }

    watch_result wait_for_event(int timeout_ms) {
        struct pollfd pfd[2] = {
            { inotify_fd, POLLIN, 0 },
            { shutdown_read_fd, POLLIN, 0 }
        };

        int rv = ::poll(pfd, 2, timeout_ms);

        if (rv == -1 && errno == EINTR) return watch_result::timeout;
        if (rv == -1) return watch_result::shutdown;
        if (rv == 0)  return watch_result::timeout;

        if (pfd[1].revents & POLLIN) return watch_result::shutdown;

        // Drain all pending inotify events
        bool got_event = false;
        bool need_rewatch = false;

        while (true) {
            char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
            ssize_t n = ::read(inotify_fd, buf, sizeof(buf));

            if (n == -1 && (errno == EINTR || errno == EAGAIN)) break;
            if (n == -1 || n == 0) break;

            for (char *ptr = buf; ptr < buf + n; ) {
                auto *event = reinterpret_cast<struct inotify_event *>(ptr);

                if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                    need_rewatch = true;
                }

                got_event = true;
                ptr += sizeof(struct inotify_event) + event->len;
            }

            if (need_rewatch) break;
        }

        if (need_rewatch) return watch_result::rewatch_needed;
        if (got_event)    return watch_result::changed;
        return watch_result::timeout;
    }

    void rewatch() {
        if (inotify_wd != -1) {
            ::inotify_rm_watch(inotify_fd, inotify_wd);
            inotify_wd = -1;
        }

        for (int attempt = 0; attempt < max_rewatch_attempts; attempt++) {
            try {
                add_watch();
                return;
            } catch (...) {
                struct pollfd pfd = { shutdown_read_fd, POLLIN, 0 };
                int timeout_ms = static_cast<int>((rewatch_backoff_us * (attempt + 1)) / 1000);
                if (timeout_ms == 0) timeout_ms = 1;
                int rv = ::poll(&pfd, 1, timeout_ms);
                if (rv > 0 && (pfd.revents & POLLIN)) return; // shutdown signaled
            }
        }

        add_watch();
    }

    void setPollInterval(uint64_t) {} // No-op for event-driven backend
    void setMaxRewatchAttempts(int n) { if (n > 0) max_rewatch_attempts = n; }
    void setRewatchBackoff(uint64_t us) { rewatch_backoff_us = us; }

    inotify_watcher(const inotify_watcher &) = delete;
    inotify_watcher &operator=(const inotify_watcher &) = delete;
};


} // namespace detail
} // namespace hoytech
