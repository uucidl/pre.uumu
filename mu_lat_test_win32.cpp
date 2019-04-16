#include <chrono>
#include <ctime>
#include <thread>

extern "C" int win32_thrd_sleep(const struct timespec *time_point, struct timespec *remaining) noexcept {
    using Clock = std::chrono::high_resolution_clock;
    auto const then = Clock::now() + std::chrono::nanoseconds(time_point->tv_nsec) + std::chrono::seconds(time_point->tv_sec);
    std::this_thread::sleep_until(then);
    auto const elapsed = Clock::now() - then;
    if (remaining) {
        remaining = {};
    }
    return 0;
}
