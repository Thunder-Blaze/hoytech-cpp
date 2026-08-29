#pragma once

#include <windows.h>
#include <string>
#include <filesystem>
#include <stdexcept>

#include "hoytech/detail/watch_result.h"

namespace hoytech {
namespace detail {

class windows_watcher {
private:
    std::string watched_path;

    HANDLE directory_handle = INVALID_HANDLE_VALUE;
    HANDLE shutdown_event = NULL;

    OVERLAPPED overlapped{};
    HANDLE change_event = NULL;

    BYTE buffer[64 * 1024];

    std::filesystem::path directory;
    std::string filename;
    std::wstring target_filename_w;
    int max_rewatch_attempts = 10;
    uint64_t rewatch_backoff_us = 5'000;

    void open_and_register() {
        if (directory_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(directory_handle);
            directory_handle = INVALID_HANDLE_VALUE;
        }

        directory_handle = CreateFileW(
            directory.wstring().c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (directory_handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("windows_watcher: CreateFileW failed on directory");
        }

        start_read();
    }

    void start_read() {
        ResetEvent(change_event);

        DWORD bytes_returned = 0;
        BOOL result = ReadDirectoryChangesW(
            directory_handle,
            buffer,
            sizeof(buffer),
            FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_CREATION,
            &bytes_returned,
            &overlapped,
            nullptr
        );

        if (!result && GetLastError() != ERROR_IO_PENDING) {
            throw std::runtime_error("windows_watcher: ReadDirectoryChangesW failed");
        }
    }

    bool matches_target(const FILE_NOTIFY_INFORMATION* info) {
        std::wstring_view changed(
            info->FileName,
            info->FileNameLength / sizeof(WCHAR)
        );
        return changed == target_filename_w;
    }

public:
    windows_watcher() = default;

    void init(const std::string& path) {
        watched_path = path;

        std::filesystem::path p(path);
        directory = p.parent_path();
        filename = p.filename().string();
        target_filename_w = p.filename().wstring();
        if (directory.empty()) {
            directory = ".";
        }

        shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!shutdown_event) throw std::runtime_error("windows_watcher: CreateEventW failed for shutdown");

        change_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!change_event) {
            CloseHandle(shutdown_event);
            shutdown_event = NULL;
            throw std::runtime_error("windows_watcher: CreateEventW failed for change event");
        }

        overlapped = {};
        overlapped.hEvent = change_event;

        try {
            open_and_register();
        } catch (...) {
            CloseHandle(change_event);
            change_event = NULL;
            CloseHandle(shutdown_event);
            shutdown_event = NULL;
            throw;
        }
    }

    ~windows_watcher() {
        shutdown();
        if (directory_handle != INVALID_HANDLE_VALUE) {
            CancelIoEx(directory_handle, &overlapped);
            CloseHandle(directory_handle);
            directory_handle = INVALID_HANDLE_VALUE;
        }
        if (shutdown_event) { CloseHandle(shutdown_event); shutdown_event = NULL; }
        if (change_event) { CloseHandle(change_event); change_event = NULL; }
    }

    void shutdown() {
        if (shutdown_event) {
            SetEvent(shutdown_event);
        }
    }

    watch_result wait_for_event(int timeout_ms) {
        HANDLE handles[] = { change_event, shutdown_event };
        DWORD wait_time = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);

        DWORD result = WaitForMultipleObjects(2, handles, FALSE, wait_time);

        if (result == WAIT_OBJECT_0 + 1) return watch_result::shutdown;
        if (result == WAIT_TIMEOUT) return watch_result::timeout;
        if (result != WAIT_OBJECT_0) return watch_result::shutdown; // Error or abandoned

        // File system changed
        DWORD bytes = 0;
        if (!GetOverlappedResult(directory_handle, &overlapped, &bytes, FALSE)) {
            return watch_result::rewatch_needed;
        }

        if (bytes == 0) {
            // Buffer overflow - need to rewatch
            return watch_result::rewatch_needed;
        }

        bool got_event = false;
        bool need_rewatch = false;
        FILE_NOTIFY_INFORMATION* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);

        while (true) {
            switch (info->Action) {
                case FILE_ACTION_ADDED:
                case FILE_ACTION_MODIFIED:
                case FILE_ACTION_REMOVED:
                case FILE_ACTION_RENAMED_OLD_NAME:
                case FILE_ACTION_RENAMED_NEW_NAME:
                    if (matches_target(info)) {
                        if (info->Action == FILE_ACTION_REMOVED || 
                            info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
                            need_rewatch = true;
                        }
                        got_event = true;
                    }
                    break;
            }

            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<BYTE*>(info) + info->NextEntryOffset
            );
        }

        start_read(); // Queue next read

        if (need_rewatch) return watch_result::rewatch_needed;
        if (got_event) return watch_result::changed;
        
        return watch_result::timeout;
    }

    void rewatch() {
        if (directory_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(directory_handle);
            directory_handle = INVALID_HANDLE_VALUE;
        }

        for (int attempt = 0; attempt < max_rewatch_attempts; attempt++) {
            try {
                open_and_register();
                return;
            } catch (...) {
                DWORD timeout_ms = static_cast<DWORD>((rewatch_backoff_us * (attempt + 1)) / 1000);
                if (timeout_ms == 0) timeout_ms = 1;
                DWORD result = WaitForSingleObject(shutdown_event, timeout_ms);
                if (result == WAIT_OBJECT_0) return; // shutdown signaled
            }
        }

        open_and_register();
    }

    void setPollInterval(uint64_t) {} // No-op for event-driven backend
    void setMaxRewatchAttempts(int n) { if (n > 0) max_rewatch_attempts = n; }
    void setRewatchBackoff(uint64_t us) { rewatch_backoff_us = us; }

    windows_watcher(const windows_watcher&) = delete;
    windows_watcher& operator=(const windows_watcher&) = delete;
};

} // namespace detail
} // namespace hoytech
