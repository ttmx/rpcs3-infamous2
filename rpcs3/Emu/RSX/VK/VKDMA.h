#pragma once
#include "vkutils/buffer_object.h"

namespace vk
{
	using dma_mapping_handle = std::pair<u32, vk::buffer*>;

	dma_mapping_handle map_dma(u32 local_address, u32 length);
	void load_dma(u32 local_address, u32 length);
	struct dma_source_observer
	{
		void* context = nullptr;
		void (*observe)(void*, const void*, const buffer*, u32, u32, u32) = nullptr;
	};
	void flush_dma(u32 local_address, u32 length, const dma_source_observer* observer = nullptr);
	void mark_dma_gpu_written(u32 local_address, u32 length);
	void unmap_dma(u32 local_address, u32 length);

	void clear_dma_resources();

	class dma_block
	{
	protected:
		struct
		{
			dma_block* parent = nullptr;
			u32 block_offset = 0;
		}
		inheritance_info;

		u32 base_address = 0;
		u8* memory_mapping = nullptr;
		bool m_gpu_written = false;
		std::unique_ptr<buffer> allocated_memory;

		virtual void allocate(const render_device& dev, usz size);
		virtual void free();
		virtual void* map_range(const utils::address_range32& range);
		virtual void unmap();

	public:

		dma_block() = default;
		virtual ~dma_block();

		virtual void init(const render_device& dev, u32 addr, usz size);
		virtual void init(dma_block* parent, u32 addr, usz size);
		virtual void flush(const utils::address_range32& range, const dma_source_observer* observer = nullptr);
		virtual void load(const utils::address_range32& range);
		void mark_gpu_written();
		std::pair<u32, buffer*> get(const utils::address_range32& range);

		u32 start() const;
		u32 end() const;
		u32 size() const;

		dma_block* head();
		const dma_block* head() const;
		virtual void set_parent(dma_block* parent);
		virtual void extend(const render_device& dev, usz new_size);
	};

	class dma_block_EXT: public dma_block
	{
	private:
		void allocate(const render_device& dev, usz size) override;
		void* map_range(const utils::address_range32& range) override;
		void unmap() override;

	public:
		void flush(const utils::address_range32& range, const dma_source_observer* observer = nullptr) override;
		void load(const utils::address_range32& range) override;
	};
}
