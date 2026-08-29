#pragma once

#include <cstdint>
#include <chrono>
#include <ctime>

namespace hoytech {

inline uint64_t curr_time_s() {
    return static_cast<uint64_t>(std::time(nullptr));
}

inline uint64_t curr_time_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

inline uint64_t curr_time_ms() {
    return curr_time_us() / 1000;
}

}
