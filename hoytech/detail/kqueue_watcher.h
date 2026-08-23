#pragma once

#include <string>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/event.h>

#include "hoytech/error.h"
#include "hoytech/detail/watch_result.h"


namespace hoytech {
namespace detail {


class kqueue_watcher {
  private:
    std::string watched_path;
    int shutdown_pipe[2] = {-1, -1};
    int kq_fd = -1;
    int watch_fd = -1;
    int max_rewatch_attempts = 10;
    uint64_t rewatch_backoff_us = 5'000;
    time_t last_mtime = 0;
    off_t last_size = 0;

    void update_mtime() {
        struct stat st;
        if (::stat(watched_path.c_str(), &st) == 0) {
            last_mtime = st.st_mtime;
            last_size = st.st_size;
        }
    }
    void open_and_register() {
        watch_fd = ::open(watched_path.c_str(), O_RDONLY
#ifdef O_CLOEXEC
            | O_CLOEXEC
#endif
        );
        if (watch_fd < 0) throw hoytech::error("unable to open file for kqueue watch '", watched_path, "': ", ::strerror(errno));

#ifndef O_CLOEXEC
        ::fcntl(watch_fd, F_SETFD, FD_CLOEXEC);
#endif

        struct kevent ev;
        EV_SET(&ev, watch_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB, 0, nullptr);

        if (::kevent(kq_fd, &ev, 1, nullptr, 0, nullptr) < 0) {
            ::close(watch_fd);
            watch_fd = -1;
            throw hoytech::error("unable to register kqueue vnode filter on '", watched_path, "': ", ::strerror(errno));
        }
    }

  public:
    kqueue_watcher() = default;

    void init(const std::string &path) {
        watched_path = path;
        if (::pipe(shutdown_pipe) < 0) throw hoytech::error("unable to create shutdown pipe: ", ::strerror(errno));
        ::fcntl(shutdown_pipe[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(shutdown_pipe[1], F_SETFD, FD_CLOEXEC);


        kq_fd = ::kqueue();
        if (kq_fd < 0) throw hoytech::error("unable to create kqueue: ", ::strerror(errno));
        ::fcntl(kq_fd, F_SETFD, FD_CLOEXEC);

        // Register the shutdown pipe on the kqueue for unified event waiting
        struct kevent pipeEv;
        EV_SET(&pipeEv, shutdown_pipe[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
        if (::kevent(kq_fd, &pipeEv, 1, nullptr, 0, nullptr) < 0)
            throw hoytech::error("unable to register shutdown pipe on kqueue: ", ::strerror(errno));

        open_and_register();
        update_mtime();
    }

    ~kqueue_watcher() {
        if (shutdown_pipe[0] != -1) { ::close(shutdown_pipe[0]); shutdown_pipe[0] = -1; }
        if (shutdown_pipe[1] != -1) { ::close(shutdown_pipe[1]); shutdown_pipe[1] = -1; }
        if (watch_fd != -1) { ::close(watch_fd); watch_fd = -1; }
        if (kq_fd != -1)    { ::close(kq_fd);    kq_fd = -1;    }
    }

    void shutdown() {
        if (shutdown_pipe[1] != -1) {
            char byte = 1;
            int rv = ::write(shutdown_pipe[1], &byte, 1);
            (void)rv;
        }
    }

    watch_result wait_for_event(int timeout_ms) {
        struct timespec *tsp = nullptr;
        struct timespec ts;

        if (timeout_ms >= 0) {
            ts.tv_sec  = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1'000'000L;
            tsp = &ts;
        }

        struct kevent out;
        int rv = ::kevent(kq_fd, nullptr, 0, &out, 1, tsp);

        if (rv == -1 && errno == EINTR) return watch_result::timeout;
        if (rv == -1)                   return watch_result::shutdown;
        if (rv == 0)                    return watch_result::timeout;
        if (out.flags & EV_ERROR)       return watch_result::shutdown;

        if (static_cast<int>(out.ident) == shutdown_pipe[0]) return watch_result::shutdown;

        if (out.fflags & (NOTE_DELETE | NOTE_RENAME)) return watch_result::rewatch_needed;

        if (out.fflags & NOTE_WRITE) {
            update_mtime();
            return watch_result::changed;
        }

        if (out.fflags & NOTE_ATTRIB) {
            struct stat st;
            if (::stat(watched_path.c_str(), &st) == 0) {
                if (st.st_mtime == last_mtime && st.st_size == last_size) {
                    // Ignore pure atime/permission updates without mtime or size change
                    return watch_result::timeout;
                }
                last_mtime = st.st_mtime;
                last_size = st.st_size;
            }
        }

        return watch_result::changed;
    }

    void rewatch() {
        if (watch_fd != -1) {
            ::close(watch_fd);
            watch_fd = -1;
        }

        for (int attempt = 0; attempt < max_rewatch_attempts; attempt++) {
            try {
                open_and_register();
                update_mtime();
                return;
            } catch (...) {
                struct pollfd pfd = { shutdown_pipe[0], POLLIN, 0 };
                int timeout_ms = static_cast<int>((rewatch_backoff_us * (attempt + 1)) / 1000);
                if (timeout_ms == 0) timeout_ms = 1;
                int rv = ::poll(&pfd, 1, timeout_ms);
                if (rv > 0 && (pfd.revents & POLLIN)) return; // shutdown signaled
            }
        }

        open_and_register();
    }

    void setPollInterval(uint64_t) {} // No-op for event-driven backend
    void setMaxRewatchAttempts(int n) { if (n > 0) max_rewatch_attempts = n; }
    void setRewatchBackoff(uint64_t us) { rewatch_backoff_us = us; }

    kqueue_watcher(const kqueue_watcher &) = delete;
    kqueue_watcher &operator=(const kqueue_watcher &) = delete;
};


} // namespace detail
} // namespace hoytech
