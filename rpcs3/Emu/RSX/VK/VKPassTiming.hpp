#pragma once

// Diagnostic only (RPCS3_VK_PASS_TIMING=1): RSX-thread time between consecutive draws, attributed to the render
// target pair of the later draw, reported every 600 frames.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace vk::pass_timing
{
	inline bool enabled()
	{
		static const bool value = [] { const char* flag = std::getenv("RPCS3_VK_PASS_TIMING"); return flag && std::strcmp(flag, "1") == 0; }();
		return value;
	}

	// Phases of one draw: 0 between draws (FIFO), 1 state/program/environment, 2 vertex and index upload,
	// 3 layout entry, descriptors and binds, 4 draw call and the rest of end()
	struct pass_t
	{
		unsigned long long ns = 0;
		unsigned long long draws = 0;
		unsigned long long phase[5]{};
	};

	struct state_t
	{
		std::map<std::pair<unsigned, unsigned>, pass_t> passes;
		pass_t* current = nullptr;
		unsigned long long last_ns = 0;
		unsigned long long report_frame = 0;
	};

	inline state_t& state() { static state_t value; return value; }

	inline unsigned long long now_ns()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	inline void draw(unsigned color, unsigned depth)
	{
		auto& s = state();
		const auto now = now_ns();
		auto& entry = s.passes[{color, depth}];
		if (s.last_ns) entry.phase[0] += now - s.last_ns;
		entry.draws++;
		s.current = &entry;
		s.last_ns = now;
	}

	inline void mark(unsigned phase)
	{
		auto& s = state();
		if (!enabled() || !s.current) return;
		const auto now = now_ns();
		s.current->phase[phase] += now - s.last_ns;
		s.last_ns = now;
	}

	// Returns the report text every 600 frames, otherwise empty
	inline std::string frame(unsigned long long frame)
	{
		auto& s = state();
		if (frame - s.report_frame < 600) return {};
		const double frames = static_cast<double>(frame - s.report_frame);
		std::string out;
		double total = 0;
		for (const auto& [key, value] : s.passes)
		{
			unsigned long long ns = 0;
			for (const auto v : value.phase) ns += v;
			total += ns / frames / 1e6;
			if (value.draws / frames < 20) continue;
			char line[200];
			std::snprintf(line, sizeof(line), " [%08x/%08x: %.2f ms, %.0f draws, %.2f us/draw: fifo %.2f state %.2f upload %.2f bind %.2f draw %.2f]", key.first, key.second,
				ns / frames / 1e6, value.draws / frames, ns / 1e3 / value.draws, value.phase[0] / 1e3 / value.draws, value.phase[1] / 1e3 / value.draws,
				value.phase[2] / 1e3 / value.draws, value.phase[3] / 1e3 / value.draws, value.phase[4] / 1e3 / value.draws);
			out += line;
		}
		char tail[64];
		std::snprintf(tail, sizeof(tail), " total %.2f ms", total);
		out += tail;
		s.passes.clear();
		s.current = nullptr;
		s.report_frame = frame;
		return out;
	}
}
