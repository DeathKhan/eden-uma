// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_multi_range_buffer.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class DescriptorPool;
class Scheduler;
struct HostVertexBinding;

class BufferCacheRuntime;

/// UMA host aliasing: imports guest RAM (the memfd backing behind the device memory manager)
/// with VK_EXT_external_memory_host in 64 MiB granules on first use, and creates cache buffers
/// that are bound straight to it. The GPU then reads and writes guest memory in place.
class HostAliasPool {
public:
    explicit HostAliasPool(const Device& device, std::span<u8> backing);
    ~HostAliasPool();

    HostAliasPool(const HostAliasPool&) = delete;
    HostAliasPool& operator=(const HostAliasPool&) = delete;

    /// Buffer whose memory is [ptr, ptr + size) of guest RAM; empty if that is not possible.
    [[nodiscard]] vk::Buffer CreateBuffer(u8* ptr, VkBufferCreateInfo ci);

    /// Dedicated import for a buffer whose guest RAM is physically scattered: the pieces are
    /// mapped back to back into a private view of the backing file (never write-protected, so
    /// the import can pin it) and imported as one allocation. Keeps the view and the memory
    /// alive until the buffer is destroyed.
    static constexpr u64 PLACEHOLDER_PAGE = ~u64{0};
    struct ScatterAlias {
        VkDevice device{};
        const vk::DeviceDispatch* dld{};
        VkDeviceMemory memory{};
        u8* view{};
        size_t size{};
        ~ScatterAlias();
    };
    [[nodiscard]] vk::Buffer CreateScatteredBuffer(std::span<const u64> page_offsets, int fd,
                                                   VkBufferCreateInfo ci,
                                                   std::shared_ptr<ScatterAlias>& owner);

    [[nodiscard]] bool IsUsable() const noexcept {
        return memory_type != INVALID_TYPE;
    }

    /// Storage-buffer window covering [ptr, ptr + size) of guest RAM for GPU reads, and ptr's
    /// offset in it: an existing import if one covers it, else a private view of the backing
    /// file's 64 MiB granule (never write-protected, so the import succeeds; the direct import
    /// fails on write-protected pages and would stop aliasing).
    [[nodiscard]] std::optional<std::pair<VkBuffer, VkDeviceSize>> FindWindow(u8* ptr, u64 size,
                                                                              int fd);
    [[nodiscard]] size_t ImportCount() const noexcept {
        return imports.size();
    }
    [[nodiscard]] u64 ImportedBytes() const noexcept {
        return imported_bytes;
    }

private:
    struct Import {
        u8* begin;
        u8* end;
        VkDeviceMemory memory;
        vk::Buffer window{}; ///< whole-import storage buffer for GPU reads (FindWindow)
        bool window_failed = false;
    };
    static constexpr u32 INVALID_TYPE = ~0U;

    const Import* FindOrImport(u8* ptr, u64 size);

    const Device& device;
    std::span<u8> backing;
    u32 memory_type = INVALID_TYPE;
    std::vector<Import> imports;
    struct ReadView {
        u64 begin; ///< offset in the backing
        u64 end;
        std::shared_ptr<ScatterAlias> owner;
        vk::Buffer buffer; ///< destroyed before owner
    };
    std::vector<ReadView> read_views;
    u32 read_view_failures = 0;
    u64 imported_bytes = 0;
    bool import_failed = false;
};

class Buffer : public VideoCommon::BufferBase {
public:
    explicit Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams null_params);
    explicit Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr_, u64 size_bytes_,
                    bool sparse_compatible_);

    [[nodiscard]] VkBufferView View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format);

    [[nodiscard]] VkBuffer Handle() const noexcept {
        return *buffer;
    }

    [[nodiscard]] VkDeviceAddress DeviceAddress() const noexcept {
        return device_address;
    }

    [[nodiscard]] bool IsSparseCompatible() const noexcept {
        return sparse_compatible;
    }

    [[nodiscard]] vk::MemoryLocation Location() const noexcept {
        return buffer.Location();
    }

    [[nodiscard]] bool IsRegionUsed(u64 offset, u64 size) const noexcept {
        return tracker.IsUsed(offset, size);
    }

    void MarkUsage(u64 offset, u64 size) noexcept;

    /// Records that GPU work recorded at the current tick may access this buffer.
    void MarkGpuUse() noexcept;

    /// UMA fast path: mapped span if the buffer is host-visible and idle on the GPU, else empty.
    [[nodiscard]] std::span<u8> DirectWriteSpan() noexcept;

    /// UMA fast path, range granular: mapped span if [offset, offset+size) has no recorded or
    /// in-flight GPU access (same condition the upload-reorder path uses) and no untracked
    /// whole-buffer GPU use is pending. Empty otherwise.
    [[nodiscard]] std::span<u8> DirectWriteRange(u64 offset, u64 size) noexcept;

    /// Records a GPU access that the range tracker does not see (indirect args, CPU-obtained).
    void MarkUntrackedGpuUse() noexcept;

    /// UMA readback: mapped span if the buffer is host-visible+coherent on the UMA path.
    [[nodiscard]] std::span<const u8> DirectReadSpan() const noexcept {
        if (!device || is_null || !buffer.IsHostVisible() || !buffer.IsHostCoherent() ||
            !device->IsUmaDirectReadback()) {
            return {};
        }
        return buffer.Mapped();
    }

    /// UMA lean: DirectReadSpan gated on uma_lean (async flushes read the cache buffer in place).
    [[nodiscard]] std::span<const u8> LeanReadSpan() const noexcept {
        if (!device || !device->IsUmaLean()) {
            return {};
        }
        return DirectReadSpan();
    }

    void FlushDirectWrite(u64 offset, u64 size) noexcept;

    /// UMA host aliasing: this buffer's memory is guest RAM itself.
    [[nodiscard]] bool IsHostAliased() const noexcept {
        return host_aliased;
    }

    /// GPU work recorded for this buffer may not have finished yet.
    [[nodiscard]] bool IsGpuBusy() const noexcept;

    /// GPU work that may access [offset, offset + size) has not finished yet (range-granular
    /// last-use ticks plus untracked whole-buffer uses).
    [[nodiscard]] bool IsRangeBusy(u64 offset, u64 size) const noexcept;

    /// Tick of the last recorded GPU use of this buffer.
    [[nodiscard]] u64 GpuUseTick() const noexcept {
        return gpu_use_tick;
    }

    /// The device mapping behind an aliased buffer changed; it must be recreated before use.
    void MarkHostAliasStale() noexcept {
        host_alias_stale = true;
    }
    [[nodiscard]] bool IsHostAliasStale() const noexcept {
        return host_alias_stale;
    }

    void ResetUsageTracking() noexcept {
        tracker.Reset();
    }

    [[nodiscard]] u64 LastUsageTick() const noexcept {
        return last_usage_tick;
    }

    operator VkBuffer() const noexcept {
        return *buffer;
    }

private:
    struct BufferView {
        u32 offset;
        u32 size;
        VideoCore::Surface::PixelFormat format;
        vk::BufferView handle;
    };

    const Device* device{};
    Scheduler* scheduler{};
    /// Owns the private view + import of a scattered alias; declared before buffer so the
    /// VkBuffer is destroyed first.
    std::shared_ptr<HostAliasPool::ScatterAlias> scatter_alias;
    vk::Buffer buffer;
    std::vector<BufferView> views;
    VideoCommon::UsageTracker tracker;
    VkDeviceAddress device_address{};
    u64 last_usage_tick{};
    u64 gpu_use_tick{};
    u64 untracked_use_tick{};
    bool is_null{};
    bool sparse_compatible{};
    bool host_aliased{};
    bool host_alias_stale{};
    /// Last GPU-use tick per 16 KiB of the buffer. Unlike the usage tracker (which only resets
    /// once the whole buffer is idle), this tells which ranges of a constantly used buffer are
    /// free again.
    static constexpr u64 USE_PAGE_SHIFT = 14;
    std::vector<u64> use_ticks;
};

class QuadArrayIndexBuffer;
class QuadStripIndexBuffer;

class BufferCacheRuntime {
    friend Buffer;

    using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

public:
    explicit BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_manager_,
                                Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                GuestDescriptorQueue& guest_descriptor_queue,
                                ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                DescriptorPool& descriptor_pool);

    void TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept;

    u64 CurrentTick();

    bool IsFree(u64 tick);

    void Wait(u64 tick);

    void Finish();

    u64 GetDeviceLocalMemory() const;

    u64 GetDeviceMemoryUsage() const;

    bool CanReportMemoryUsage() const;

    u32 GetUniformBufferAlignment() const;

    u32 GetStorageBufferAlignment() const;

    [[nodiscard]] StagingBufferRef UploadStagingBuffer(size_t size);

    [[nodiscard]] StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);

    bool CanReorderUpload(const Buffer& buffer, std::span<const VideoCommon::BufferCopy> copies);

    void FreeDeferredStagingBuffer(StagingBufferRef& ref);

    void PreCopyBarrier();

    void CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool can_reorder_upload = false);

    void PostCopyBarrier();

    /// UMA readback: make all prior GPU writes available to host reads (pair with Finish()).
    void HostReadBarrier();

    /// UMA host aliasing (see HostAliasPool).
    void BindDeviceMemory(Tegra::MaxwellDeviceMemoryManager& device_memory);
    [[nodiscard]] bool HasHostAliases() const noexcept {
        return has_host_aliases.load(std::memory_order_relaxed);
    }
    /// Called once per guest fence; true when the fence has to wait for the host GPU.
    bool CountHostSyncedFence() noexcept;
    /// Keeps [addr, addr + size) out of aliasing from now on.
    void ForbidHostAlias(DAddr addr, u64 size);
    /// GPU engines wrote [addr, addr + size) from the command stream (4 KiB granularity).
    void MarkEngineWritten(DAddr addr, u64 size);
    [[nodiscard]] bool IsEngineWritten(DAddr addr, u64 size) const;
    /// Storage-buffer window of guest RAM [addr, addr + size) for GPU reads (UMA texture
    /// uploads), as (buffer, offset); empty if that range is not contiguous and imported.
    [[nodiscard]] std::optional<std::pair<VkBuffer, VkDeviceSize>> HostAliasWindow(
        DAddr addr, u64 size, bool* scattered = nullptr);

    /// Aliased cache buffer for [addr, addr + size), or an empty buffer.
    [[nodiscard]] vk::Buffer CreateHostAliasBuffer(
        DAddr addr, u64 size, bool sparse_compatible,
        std::shared_ptr<HostAliasPool::ScatterAlias>& scatter_owner);

    void ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value);

    void BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format, u32 num_indices,
                         u32 base_vertex, VkBuffer buffer, u32 offset, u32 size);

    void BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count);

    /// UMA: converts quad / quad-strip indices to a triangle list on the CPU (same result as
    /// QuadIndexedPass) into stream memory and binds it. False when not applicable.
    bool BindQuadIndicesFromHost(PrimitiveTopology topology, IndexFormat index_format,
                                 u32 base_vertex, u32 num_indices, std::span<const u8> source);

    void BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size, u32 stride);

    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings);

    void BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size);

    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings);

    std::span<u8> BindMappedUniformBuffer([[maybe_unused]] size_t stage,
                                          [[maybe_unused]] u32 binding_index,
                                          u32 size) {
        const StagingBufferRef ref = staging_pool.Request(size, MemoryUsage::Upload);
        guest_descriptor_queue.AddBuffer(ref.buffer, ref.device_address,
                                         static_cast<u32>(ref.offset), size);
        return ref.mapped_span;
    }

    [[nodiscard]] VkDeviceSize SparseAlignmentFor(bool sparse_compatible) const noexcept {
        if (!sparse_compatible || !multi_range_buffers.use_sparse) {
            return 0;
        }
        return multi_range_buffers.block_size;
    }

    [[nodiscard]] bool PrefersSparseSources() const noexcept {
        return multi_range_buffers.use_sparse;
    }

    void ResetMultiRange() noexcept {
        multi_range_sources.clear();
        multi_range_total = 0;
    }

    void PushMultiRangeSource(const Buffer& buffer, u32 offset, u32 size) {
        const vk::MemoryLocation location = buffer.Location();
        multi_range_sources.push_back(MultiRangeSource{
            .handle = buffer.Handle(),
            .memory = location.memory,
            .memory_offset = location.offset,
            .offset = offset,
            .size = size,
            .write_tick = buffer.getWriteTick(),
            .memory_type = location.memory_type,
        });
        multi_range_total += size;
    }

    bool BindMultiRangeStorageBuffer(u64 key, bool is_written);

    void InvalidateMultiRange(u64 key) {
        multi_range_buffers.Invalidate(key);
    }

    void OnBufferDeleted(const Buffer& buffer) {
        multi_range_buffers.DropOwner(scheduler, buffer.Handle());
    }

    void BindUniformBuffer(const Buffer& buffer, u32 offset, u32 size) {
        BindBuffer(buffer, offset, size);
    }

    void BindStorageBuffer(const Buffer& buffer, u32 offset, u32 size,
                           [[maybe_unused]] bool is_written) {
        BindBuffer(buffer, offset, size);
    }

    void BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat format) {
        if (guest_descriptor_queue.UsesDescriptorBuffer()) {
            const VkDeviceAddress address = buffer.DeviceAddress();
            const VkFormat vk_format = address == 0 ? VK_FORMAT_UNDEFINED : TexelBufferFormat(format);
            guest_descriptor_queue.AddTexelBuffer(VK_NULL_HANDLE, address, offset, size, vk_format);
        } else {
            guest_descriptor_queue.AddTexelBuffer(buffer.View(offset, size, format));
        }
    }

    bool ShouldLimitDynamicStorageBuffers() const {
        return limit_dynamic_storage_buffers;
    }

    u32 GetMaxDynamicStorageBuffers() const {
        return max_dynamic_storage_buffers;
    }

private:
    void BindBuffer(const Buffer& buffer, u32 offset, u32 size) {
        const VkBuffer handle = buffer.Handle();
        if (handle == VK_NULL_HANDLE) {
            guest_descriptor_queue.AddBuffer(handle, 0, 0, VK_WHOLE_SIZE);
        } else {
            guest_descriptor_queue.AddBuffer(handle, buffer.DeviceAddress(), offset, size);
        }
    }

    VkFormat TexelBufferFormat(VideoCore::Surface::PixelFormat format) const;

    void ReserveNullBuffer();
    vk::Buffer CreateNullBuffer();

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;
    GuestDescriptorQueue& guest_descriptor_queue;

    std::shared_ptr<QuadArrayIndexBuffer> quad_array_index_buffer;
    std::shared_ptr<QuadStripIndexBuffer> quad_strip_index_buffer;

    vk::Buffer null_buffer;

    std::unique_ptr<Uint8Pass> uint8_pass;
    QuadIndexedPass quad_index_pass;

    MultiRangeBufferCache multi_range_buffers;
    boost::container::small_vector<MultiRangeSource, 16> multi_range_sources;
    VkDeviceSize multi_range_total{};

    bool limit_dynamic_storage_buffers = false;
    u32 max_dynamic_storage_buffers = (std::numeric_limits<u32>::max)();

    Tegra::MaxwellDeviceMemoryManager* device_memory = nullptr;
    std::unique_ptr<HostAliasPool> host_alias_pool;
    std::vector<u64> host_alias_forbidden; ///< Bit per 64 KiB device page.
    std::vector<u64> engine_written;       ///< Bit per 4 KiB device page.
    std::vector<u64> scatter_pages;        ///< Scratch: backing offset per page.
    std::atomic<bool> has_host_aliases{false};
    struct HostAliasStats {
        u64 buffers = 0;
        u64 bytes = 0;
        u64 noncontiguous = 0;
        u64 scattered = 0;
        u64 forbidden = 0;
        u64 rejected = 0;
        u64 dealiased = 0;
        u64 fences = 0;
    } host_alias_stats;
};

struct BufferCacheParams {
    using Runtime = Vulkan::BufferCacheRuntime;
    using Buffer = Vulkan::Buffer;
    using Async_Buffer = Vulkan::StagingBufferRef;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;

    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;

} // namespace Vulkan
