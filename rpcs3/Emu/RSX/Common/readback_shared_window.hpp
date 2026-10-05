#pragma once

#include "Utilities/address_range.h"
#include "Utilities/mutex.h"
#include "cache_wait_diagnostics.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <type_traits>
#include <optional>

namespace rsx
{
	// Classification only: observes two measured remaining texture addresses.
	// Use existing bounded CSV output, without altering barriers or lookup state.
	inline bool readback_fallback_classification_enabled(u32 address)
	{
		if (!cache_wait_trace::enabled || (address != 0xc0ab8000 && address != 0xc0010000))
			return false;
		static const bool enabled = []
		{
			const char* value = std::getenv("RPCS3_VK_READBACK_FALLBACK_CLASSIFY");
			return value && std::strcmp(value, "1") == 0;
		}();
		return enabled;
	}

	inline void readback_fallback_trace(const char* label, u32 address, u32 length)
	{
		if (!readback_fallback_classification_enabled(address))
			return;
		const auto now = cache_wait_trace::now();
		cache_wait_trace::record(label, now, now, thread_ctrl::get_tid(), 0, 0, address, length);
	}

	inline bool readback_shared_window_enabled()
	{
		static const bool enabled = []
		{
			const char* value = std::getenv("RPCS3_VK_READBACK_SHARED_HITS");
			return value && std::strcmp(value, "1") == 0;
		}();
		return enabled;
	}

	inline bool readback_shared_rejections_enabled()
	{
		static const bool enabled = []
		{
			const char* value = std::getenv("RPCS3_VK_READBACK_SHARED_REJECTIONS");
			return value && std::strcmp(value, "1") == 0;
		}();
		return enabled;
	}

	inline bool readback_shared_compressed_enabled()
	{
		static const bool enabled = []
		{
			const char* value = std::getenv("RPCS3_VK_READBACK_COMPRESSED_HITS");
			return value && std::strcmp(value, "1") == 0;
		}();
		return enabled;
	}


 inline bool readback_bound_framebuffer_enabled()
 {
  static const bool value=[]{const char* p=std::getenv("RPCS3_VK_READBACK_BOUND_DESCRIPTOR");return p&&std::strcmp(p,"1")==0;}();return value;
 }
 inline bool readback_bound_framebuffer_shadow()
 {
  static const bool value=[]{const char* p=std::getenv("RPCS3_VK_READBACK_BOUND_SHADOW");return p&&std::strcmp(p,"1")==0;}();return value;
 }
 struct frozen_resource_identity {const void* object=nullptr;const void* memory=nullptr;u64 image=0;};
 template<class H> u64 frozen_handle_key(H h)
 {if constexpr(std::is_pointer_v<H>)return reinterpret_cast<std::uintptr_t>(h);else return static_cast<u64>(h);}
 // Strict, 100%-scale compatible2D shadow: pure descriptor only, never access tag.
 template<class D> bool readback_bound_descriptor_equal(const D& a,const D& b)
 {
  if(a.image_handle!=b.image_handle||a.upload_context!=b.upload_context||a.format_class!=b.format_class||a.image_type!=b.image_type||a.ref_address!=b.ref_address||a.surface_cache_tag!=b.surface_cache_tag||a.is_cyclic_reference!=b.is_cyclic_reference||a.samples!=b.samples)return false;
  for(unsigned i=0;i<3;++i)if(a.texcoord_xform.scale[i]!=b.texcoord_xform.scale[i]||a.texcoord_xform.bias[i]!=b.texcoord_xform.bias[i])return false;
  if(a.texcoord_xform.clamp!=b.texcoord_xform.clamp||a.flag!=b.flag)return false;
  if(a.texcoord_xform.clamp)for(unsigned i=0;i<2;++i)if(a.texcoord_xform.clamp_min[i]!=b.texcoord_xform.clamp_min[i]||a.texcoord_xform.clamp_max[i]!=b.texcoord_xform.clamp_max[i])return false;
  if(a.format_ex.format_bits!=b.format_ex.format_bits||a.format_ex.features!=b.format_ex.features||a.format_ex.encoded_remap!=b.format_ex.encoded_remap||a.format_ex.texel_remap_control!=b.format_ex.texel_remap_control||a.format_ex.host_features!=b.format_ex.host_features)return false;
  const auto& x=a.external_subresource_desc;const auto& y=b.external_subresource_desc;
  if(x.op!=y.op||x.external_handle!=y.external_handle||x.external_ref_addr!=y.external_ref_addr||x.cache_range!=y.cache_range||x.do_not_cache!=y.do_not_cache||x.force_bg_load!=y.force_bg_load)return false;
  if(x.address!=y.address||x.gcm_format!=y.gcm_format||x.pitch!=y.pitch||x.width!=y.width||x.height!=y.height||x.depth!=y.depth||x.mipmaps!=y.mipmaps||x.slice_h!=y.slice_h||x.bpp!=y.bpp||x.swizzled!=y.swizzled||x.edge_clamped!=y.edge_clamped||x.remap.encoded!=y.remap.encoded||x.remap.control_map!=y.remap.control_map||x.remap.channel_map!=y.remap.channel_map||x.sections_to_copy.size()!=y.sections_to_copy.size())return false;
  for(usz i=0;i<x.sections_to_copy.size();++i){const auto& p=x.sections_to_copy[i];const auto& q=y.sections_to_copy[i];if(p.src!=q.src||p.xform!=q.xform||p.base_addr!=q.base_addr||p.level!=q.level||p.src_x!=q.src_x||p.src_y!=q.src_y||p.dst_x!=q.dst_x||p.dst_y!=q.dst_y||p.dst_z!=q.dst_z||p.src_w!=q.src_w||p.src_h!=q.src_h||p.dst_w!=q.dst_w||p.dst_h!=q.dst_h)return false;}
  return true;
 }
	// The owner retains the exclusive cache mutex. Only the dedicated immutable
	// shader-read hit lookup may inspect frozen metadata through a counted lease.
	class readback_shared_window
	{
		static constexpr unsigned closed = 1u << 31;
		std::atomic<unsigned> state{closed};

	public:
		std::vector<utils::address_range32> reserved_pages;
		std::vector<frozen_resource_identity> reserved_resources;
		bool resource_identities_complete=true;
		bool resource_disjoint(const frozen_resource_identity& r) const
		{
			if(!resource_identities_complete||!r.object||!r.memory||!r.image||reserved_resources.empty())return false;
			for(const auto& owner:reserved_resources)if(r.object==owner.object||r.image==owner.image||r.memory==owner.memory)return false;
			return true;
		}
		std::atomic<unsigned long long> hits{0};
		std::atomic<unsigned long long> misses{0};

		bool try_acquire()
		{
			auto value = state.load(std::memory_order_acquire);
			return !(value & closed) && value < closed - 1 &&
				state.compare_exchange_strong(value, value + 1, std::memory_order_acquire, std::memory_order_relaxed);
		}

		bool disjoint(const utils::address_range32& range) const
		{
			for (const auto& reserved : reserved_pages)
				if (range.overlaps(reserved))
					return false;
			return true;
		}

		class lease
		{
			readback_shared_window& window;
		public:
			explicit lease(readback_shared_window& window_) : window(window_) {}
			lease(const lease&) = delete;
			lease& operator=(const lease&) = delete;
			~lease() { window.state.fetch_sub(1, std::memory_order_release); }
		};

		class frozen_guard
		{
			readback_shared_window& window;
		public:
			explicit frozen_guard(readback_shared_window& window_) : window(window_)
			{
				window.state.store(0, std::memory_order_release);
			}
			frozen_guard(const frozen_guard&) = delete;
			frozen_guard& operator=(const frozen_guard&) = delete;
			~frozen_guard()
			{
				window.state.fetch_or(closed, std::memory_order_acq_rel);
				while (window.state.load(std::memory_order_acquire) != closed)
					std::this_thread::yield();
				window.reserved_pages.clear();
				window.reserved_resources.clear();
				window.resource_identities_complete=true;
			}
		};

		// Adopts a reader already acquired by the window-aware normal acquisition.
		// Its upgrade behavior is identical to the existing reader_lock.
		class adopted_reader
		{
			shared_mutex& mutex;
			bool upgraded = false;
		public:
			explicit adopted_reader(shared_mutex& mutex_) : mutex(mutex_) {}
			adopted_reader(const adopted_reader&) = delete;
			adopted_reader& operator=(const adopted_reader&) = delete;
			void upgrade()
			{
				if (!upgraded)
				{
					mutex.lock_upgrade();
					upgraded = true;
				}
			}
			~adopted_reader() { upgraded ? mutex.unlock() : mutex.unlock_shared(); }
		};
	};
}
