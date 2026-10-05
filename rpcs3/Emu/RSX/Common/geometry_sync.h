#pragma once

// Guest/RSX synchronisation points observed by the RSX thread: the places where the command stream waits for the
// guest (new PUT value, empty FIFO, jump-to-self, semaphore acquire). Vertex and index memory read by commands
// that follow such a point may have been rewritten up to that point, so content cached from guest memory is
// revalidated after each of them. Plain jumps, calls and returns do not wait for anything; they are only counted.
// Single writer (the RSX thread).
#include <utility>
#include <vector>

namespace rsx::geometry_sync
{
	enum kind : unsigned { put_changed, jump, call, ret, semaphore, fifo_empty, self_jump, kind_count };

	inline unsigned long long counters[kind_count]{};
	inline unsigned long long epoch = 1;
	inline unsigned last_put = ~0u;

	// Diagnostic: when set, every dispatched FIFO method (register offset, value) and flow-control word (offset ~0)
	// is appended here; the geometry trace drains it at each draw
	inline std::vector<std::pair<unsigned, unsigned>>* fifo_log = nullptr;

	// Diagnostic (fast draw verification): when tracking, counts dispatched methods that the fast draw path would not consume
	inline bool track_methods = false;
	inline unsigned other_methods = 0;

	// Diagnostics: the last point that advanced the epoch, and the draws since
	inline unsigned last_kind = 0;
	inline unsigned long long draws = 0;
	inline unsigned long long draws_at_last_point = 0;

	inline void point(kind k)
	{
		counters[k]++;

		if (fifo_log && (k == jump || k == call || k == ret || k == self_jump)) [[unlikely]]
		{
			fifo_log->emplace_back(~0u, static_cast<unsigned>(k));
		}

		if (k != jump && k != call && k != ret)
		{
			epoch++;
			last_kind = k;
			draws_at_last_point = draws;
		}
	}

	inline void observe_put(unsigned put)
	{
		if (put != last_put)
		{
			last_put = put;
			point(put_changed);
		}
	}
}
