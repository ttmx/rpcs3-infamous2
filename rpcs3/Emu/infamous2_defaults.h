#pragma once

#include <cstdlib>

// The runtime switches of the inFamous 2 work are read from the environment. The ones that were measured to help are
// set here, once, before anything reads them; a variable that is already set (to 0, say, to turn a change off) is
// left alone. Everything not listed stays off unless set.
namespace infamous2
{
	inline void apply_default_switches()
	{
		struct entry
		{
			const char* name;
			const char* value;
		};

		static constexpr entry defaults[]
		{
			{"RPCS3_EXPERIMENT_SPU_INSTCOMBINE", "1"},
			{"RPCS3_SPU_NATIVE_RWV", "1"},
			{"RPCS3_VK_READBACK_ADMISSION", "1"},
			{"RPCS3_VK_READBACK_SHARED_HITS", "1"},
			{"RPCS3_VK_READBACK_COMPRESSED_HITS", "1"},
			{"RPCS3_VK_READBACK_STREAM_COPY", "1"},
			{"RPCS3_VK_READBACK_OOP", "1"},
			{"RPCS3_VK_SEMAPHORE_PIPELINE_PREFETCH", "1"},
			{"RPCS3_VK_PERIODIC_SUBMIT_US", "1000"},
			{"RPCS3_VK_STREAM_DMA_LOAD_MIN", "65536"},
			{"RPCS3_EXPERIMENT_BLIT_COMPLEMENT", "1"},
			{"RPCS3_FIFO_INLINE_CACHE", "1"},
			{"RPCS3_EXPERIMENT_LAST_IMAGE_VIEW", "1"},
			{"RPCS3_NATIVE_SSAO", "5"},
			{"RPCS3_NATIVE_LIGHTING", "61"},
			{"RPCS3_VK_GEOMETRY_CACHE", "1"},
			{"RPCS3_VK_FAST_DRAWS", "6"},
			{"RPCS3_VK_PIPELINE_REUSE", "1"},
			{"RPCS3_VK_DESCRIPTOR_REUSE", "1"},
		};

		for (const entry& e : defaults)
		{
			if (!std::getenv(e.name))
			{
#ifdef _WIN32
				_putenv_s(e.name, e.value);
#else
				::setenv(e.name, e.value, 0);
#endif
			}
		}
	}
}
