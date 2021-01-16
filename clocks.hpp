// C++ chrono interface to "fast"/"coarse" clock_gettime clocks
#pragma once
#include <cassert>
#include <chrono>
#include <ctime>

template <clockid_t CLK, bool STEADY>
class posix_clock {
 public:
	using duration = std::chrono::nanoseconds;
	using rep = duration::rep;
	using period = duration::period;
	using time_point = std::chrono::time_point<posix_clock<CLK, STEADY>>;

	static constexpr bool is_steady = STEADY;

	static inline time_point now() noexcept {
		struct timespec t;
		const auto result = clock_gettime(CLK, &t);
		assert(result == 0);
		return time_point(std::chrono::seconds(t.tv_sec) + std::chrono::nanoseconds(t.tv_nsec));
	}

	static inline time_t to_time_t(const time_point& t) noexcept {
		return time_t(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
	}

	static inline duration get_resolution() noexcept {
		struct timespec t;
		const auto result = clock_getres(CLK, &t);
		assert(result == 0);
		return std::chrono::seconds(t.tv_sec) + std::chrono::nanoseconds(t.tv_nsec);
	}
};

#if defined(CLOCK_MONOTONIC_FAST)
using fast_mono_clock = posix_clock<CLOCK_MONOTONIC_FAST, true>;
#elif defined(CLOCK_MONOTONIC_COARSE)
using fast_mono_clock = posix_clock<CLOCK_MONOTONIC_COARSE, true>;
#else
using fast_mono_clock = posix_clock<CLOCK_MONOTONIC, true>;
#endif

#if defined(CLOCK_REALTIME_FAST)
using fast_wall_clock = posix_clock<CLOCK_REALTIME_FAST, false>;
#elif defined(CLOCK_REALTIME_COARSE)
using fast_wall_clock = posix_clock<CLOCK_REALTIME_COARSE, false>;
#else
using fast_wall_clock = posix_clock<CLOCK_REALTIME, false>;
#endif

#undef CLOCK_CLOCKS_HPP
