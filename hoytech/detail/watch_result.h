#pragma once

namespace hoytech {
namespace detail {

enum class watch_result {
  changed,        // File content or attributes modified
  rewatch_needed, // File was deleted/renamed; caller should rewatch
  timeout,        // Timeout expired with no event
  shutdown        // Shutdown signal received
};

} // namespace detail
} // namespace hoytech
