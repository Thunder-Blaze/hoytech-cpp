#pragma once

#include <atomic>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <poll.h>

#include <string>
#include <thread>
#include <functional>

#include "hoytech/error.h"
#include "hoytech/time.h"

#ifdef __linux__
#include <sys/inotify.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/types.h>
#else
#include <chrono>
#include <filesystem>
#endif



namespace hoytech {


class file_change_monitor {
  private:
    std::thread t;
    std::string watched_path;
    uint64_t debounce_us = 50 * 1000;
    int shutdown_pipe[2] = {-1, -1};
    std::atomic<bool> shutdown{false};

#ifdef __linux__
    int inotify_fd = -1;
    int inotify_wd = -1;

    void add_watch() {
        inotify_wd = ::inotify_add_watch(inotify_fd, watched_path.c_str(),
            IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
        if (inotify_wd < 0) throw hoytech::error("unable to add watch to inotify descriptor: ", ::strerror(errno));
    }

    void rewatch() {
        if (inotify_wd != -1) {
            ::inotify_rm_watch(inotify_fd, inotify_wd);
            inotify_wd = -1;
        }

        for (int attempt = 0; attempt < 10; attempt++) {
            try {
                add_watch();
                return;
            } catch (...) {
                ::usleep(static_cast<useconds_t>(5'000 * (attempt + 1)));
            }
        }

        add_watch();
    }
#elif defined(__APPLE__)
    int kq_fd = -1;
    int watch_fd = -1;

    void open_and_register() {
        watch_fd = ::open(watched_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (watch_fd < 0) throw hoytech::error("unable to open file for kqueue watch: ", ::strerror(errno));

        struct kevent ev;
        EV_SET(&ev, watch_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB, 0, nullptr);

        if (::kevent(kq_fd, &ev, 1, nullptr, 0, nullptr) < 0) {
            ::close(watch_fd);
            watch_fd = -1;
            throw hoytech::error("unable to register kqueue vnode filter: ", ::strerror(errno));
        }
    }

    void rewatch() {
        if (watch_fd != -1) {
            ::close(watch_fd);
            watch_fd = -1;
        }

        for (int attempt = 0; attempt < 10; attempt++) {
            try {
                open_and_register();
                return;
            } catch (...) {
                ::usleep(static_cast<useconds_t>(5'000 * (attempt + 1)));
            }
        }

        open_and_register();
    }
#else
    std::filesystem::file_time_type last_write_time = std::filesystem::file_time_type::min();
#endif

    void cleanup() {
        if (shutdown_pipe[0] != -1) { ::close(shutdown_pipe[0]); shutdown_pipe[0] = -1; }
        if (shutdown_pipe[1] != -1) { ::close(shutdown_pipe[1]); shutdown_pipe[1] = -1; }

#ifdef __linux__
        if (inotify_wd != -1) {
            ::inotify_rm_watch(inotify_fd, inotify_wd);
            inotify_wd = -1;
        }
        if (inotify_fd != -1) {
            ::close(inotify_fd);
            inotify_fd = -1;
        }
#elif defined(__APPLE__)
        if (watch_fd != -1) { ::close(watch_fd); watch_fd = -1; }
        if (kq_fd != -1) { ::close(kq_fd); kq_fd = -1; }
#endif
    }

  public:
    file_change_monitor(std::string path) : watched_path(std::move(path)) {
        if (::pipe(shutdown_pipe) < 0) throw hoytech::error("unable to create pipe: ", ::strerror(errno));
        ::fcntl(shutdown_pipe[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(shutdown_pipe[1], F_SETFD, FD_CLOEXEC);

#ifdef __linux__
        inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd < 0) throw hoytech::error("unable to create inotify descriptor: ", ::strerror(errno));

        add_watch();
#elif defined(__APPLE__)
        kq_fd = ::kqueue();
        if (kq_fd < 0) throw hoytech::error("unable to create kqueue: ", ::strerror(errno));
        ::fcntl(kq_fd, F_SETFD, FD_CLOEXEC);

        struct kevent pipeEv;
        EV_SET(&pipeEv, shutdown_pipe[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
        if (::kevent(kq_fd, &pipeEv, 1, nullptr, 0, nullptr) < 0)
            throw hoytech::error("unable to register shutdown pipe on kqueue: ", ::strerror(errno));

        open_and_register();
#else
        try {
            last_write_time = std::filesystem::last_write_time(watched_path);
        } catch (...) {
            last_write_time = std::filesystem::file_time_type::min();
        }
#endif
    }

    void setDebounce(uint64_t ms) {
        debounce_us = ms * 1000;
    }

    void run(std::function<void()> cb) {
        if (shutdown) throw hoytech::error("file watcher already shutdown");

        t = std::thread([cb, this]() {
            uint64_t trigger_time = 0;

#ifdef __linux__
            struct pollfd pollfd_array[2] = {
                { inotify_fd, POLLIN, 0 },
                { shutdown_pipe[0], POLLIN, 0 }
            };

            while (1) {
                int timeout_ms = -1;

                if (trigger_time) {
                    uint64_t now = hoytech::curr_time_us();

                    if (now < trigger_time) {
                        timeout_ms = (trigger_time - now) / 1000;
                    } else {
                        timeout_ms = 0;
                    }

                    if (timeout_ms == 0) {
                        trigger_time = 0;
                        cb();
                        continue;
                    }
                }

                int rv = ::poll(pollfd_array, 2, timeout_ms);
                if (shutdown) return;

                if (rv == -1 && errno == EINTR) continue;
                if (rv == -1) return;
                if (rv == 0) continue;

                if (pollfd_array[1].revents & POLLIN) return;

                while (1) {
                    char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
                    ssize_t n = ::read(inotify_fd, buf, sizeof(buf));
                    if (shutdown) return;

                    if (n == -1 && (errno == EINTR || errno == EAGAIN)) break;
                    if (n == -1) return;
                    if (n == 0) break;

                    bool need_rewatch = false;

                    for (char *ptr = buf; ptr < buf + n; ) {
                        auto *event = reinterpret_cast<struct inotify_event *>(ptr);

                        if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                            need_rewatch = true;
                        }

                        if (trigger_time == 0) trigger_time = hoytech::curr_time_us() + debounce_us;

                        ptr += sizeof(struct inotify_event) + event->len;
                    }

                    if (need_rewatch) {
                        try {
                            rewatch();
                        } catch (const std::exception &e) {
                            ::fprintf(stderr, "file_change_monitor: rewatch failed: %s\n", e.what());
                        } catch (...) {
                            ::fprintf(stderr, "file_change_monitor: rewatch failed: unknown error\n");
                        }
                        break;
                    }
                }
            }

#elif defined(__APPLE__)
            while (1) {
                struct timespec *tsp = nullptr;
                struct timespec ts;

                if (trigger_time) {
                    uint64_t now = hoytech::curr_time_us();

                    if (now >= trigger_time) {
                        trigger_time = 0;
                        cb();
                        continue;
                    }

                    uint64_t remaining_us = trigger_time - now;
                    ts.tv_sec = remaining_us / 1'000'000;
                    ts.tv_nsec = (remaining_us % 1'000'000) * 1000;
                    tsp = &ts;
                }

                struct kevent out;
                int rv = ::kevent(kq_fd, nullptr, 0, &out, 1, tsp);
                if (shutdown) return;

                if (rv == -1 && errno == EINTR) continue;
                if (rv == -1) return;
                if (rv == 0) continue;
                if (out.flags & EV_ERROR) return;

                if (static_cast<int>(out.ident) == shutdown_pipe[0]) return;

                if (out.fflags & (NOTE_DELETE | NOTE_RENAME)) {
                    try {
                        rewatch();
                    } catch (const std::exception &e) {
                        ::fprintf(stderr, "file_change_monitor: rewatch failed: %s\n", e.what());
                    } catch (...) {
                        ::fprintf(stderr, "file_change_monitor: rewatch failed: unknown error\n");
                    }
                }

                if (trigger_time == 0) trigger_time = hoytech::curr_time_us() + debounce_us;
            }
#else
            while (!shutdown) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (shutdown) return;

                std::filesystem::file_time_type current_write_time = std::filesystem::file_time_type::min();
                try {
                    current_write_time = std::filesystem::last_write_time(watched_path);
                } catch (...) {}

                if (current_write_time != last_write_time) {
                    last_write_time = current_write_time;
                    if (trigger_time == 0) trigger_time = hoytech::curr_time_us() + debounce_us;
                }

                if (trigger_time != 0 && hoytech::curr_time_us() >= trigger_time) {
                    trigger_time = 0;
                    cb();
                }
            }
#endif
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

        cleanup();
    }
};


}
