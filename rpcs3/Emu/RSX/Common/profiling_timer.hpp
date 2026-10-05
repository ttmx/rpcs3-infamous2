#pragma once

#include <util/types.hpp>
#include <cstdlib>
#include "Emu/Cell/timers.hpp"

namespace rsx
{
	struct profiling_timer
	{
		// Diagnostic only: use existing profiling clock sites without rendering an overlay.
		static bool native_phase_diagnostics_enabled() noexcept
		{
			static const bool active = []
			{
				const char* flag = std::getenv("RPCS3_VK_NATIVE_PHASE_COUNTERS");
				return flag && flag[0] == '1' && flag[1] == '\0';
			}();
			return active;
		}

		bool enabled = false;
		u64 last;

		profiling_timer() = default;

		void start()
		{
			if (enabled) [[unlikely]]
			{
				last = get_system_time();
			}
		}

		s64 duration()
		{
			if (!enabled) [[likely]]
			{
				return 0ll;
			}

			auto old = last;
			last = get_system_time();
			return static_cast<s64>(last - old);
		}
	};
}
