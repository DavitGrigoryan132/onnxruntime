// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "precomp.h"
#include "PooledUploadHeap.h"
#include "ExecutionContext.h"

namespace Dml
{
    PooledUploadHeap::PooledUploadHeap(ID3D12Device* device, std::shared_ptr<ExecutionContext> executionContext)
        : m_device(device)
        , m_executionContext(std::move(executionContext))
    {
    }

    static size_t Align(size_t offset, size_t alignment)
    {
        assert(alignment != 0);
        return (offset + alignment - 1) & ~(alignment - 1);
    }

    std::optional<size_t> PooledUploadHeap::FindOffsetForAllocation(const Chunk& chunk, size_t sizeInBytes)
    {
        assert(sizeInBytes != 0);

        if (chunk.capacityInBytes < sizeInBytes)
        {
            // This chunk isn't even big enough to accommodate this allocation
            return std::nullopt;
        }

        if (chunk.allocations.empty())
        {
            // The entire chunk is empty - allocate from the beginning
            return 0;
        }

        // Chunks are used as ring buffers, which means this allocation should go after the most recent previous
        // allocation

        const auto& lastAllocation = chunk.allocations.back();
        size_t newAllocationBegin = lastAllocation.offsetInChunk + lastAllocation.sizeInBytes;
        newAllocationBegin = Align(newAllocationBegin, c_allocationAlignment);

        if (newAllocationBegin + sizeInBytes < newAllocationBegin)
        {
            // Overflow
            return std::nullopt;
        }

        const auto& firstAllocation = chunk.allocations.front();
        if (firstAllocation.offsetInChunk <= lastAllocation.offsetInChunk)
        {
            // This is the case where there's potentially free space at the beginning and end of the chunk, but not
            // the middle:
            // e.g.
            //   |------XXXXYYYZZ------|
            //          ^^^^   ^^
            //          first  last

            if (newAllocationBegin + sizeInBytes <= chunk.capacityInBytes)
            {
                // There's enough space between the end of the last allocation and the end of the chunk
                return newAllocationBegin;
            }
            else
            {
                // Otherwise there's not enough space at the end of the chunk - try the beginning of the chunk instead
                newAllocationBegin = 0;
                if (newAllocationBegin + sizeInBytes <= firstAllocation.offsetInChunk)
                {
                    // There was enough space between the start of the buffer, and the start of the first allocation
                    return newAllocationBegin;
                }
            }
        }
        else
        {
            // This is the case where there's potentially free space in the middle of the chunk, but not at the edges
            // e.g.
            //   |YYYZZ---------XXXX-|
            //       ^^         ^^^^
            //       last       first

            if (newAllocationBegin + sizeInBytes <= firstAllocation.offsetInChunk)
            {
                // There's enough space between the end of the last allocation, and the start of the first one
                return newAllocationBegin;
            }
        }

        // Not enough space in this chunk to accommodate the requested allocation
        return std::nullopt;
    }

    /* static */ PooledUploadHeap::Chunk PooledUploadHeap::CreateChunk(ID3D12Device* device, size_t sizeInBytes)
    {
        ComPtr<ID3D12Resource> uploadBuffer;
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);

        ORT_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(uploadBuffer.ReleaseAndGetAddressOf())));

        return Chunk{ sizeInBytes, std::move(uploadBuffer) };
    }

    std::pair<PooledUploadHeap::Chunk*, size_t> PooledUploadHeap::Reserve(size_t sizeInBytes)
    {
        // Try to find a chunk with enough free space to accommodate the requested allocation size
        for (Chunk& chunk : m_chunks)
        {
            std::optional<size_t> offsetForAllocation = FindOffsetForAllocation(chunk, sizeInBytes);
            if (offsetForAllocation)
            {
                // There's enough space in this chunk - return
                return std::make_pair(&chunk, *offsetForAllocation);
            }
        }

        // No chunks were able to accommodate the allocation - create a new chunk and return that instead

        // At least double the capacity of the pool
        const size_t newChunkSize = std::max({ m_totalCapacity, c_minChunkSize, sizeInBytes });
        m_chunks.push_back(CreateChunk(m_device.Get(), newChunkSize));
        m_totalCapacity += newChunkSize;

        // Allocate from the beginning of the new chunk
        return std::make_pair(&m_chunks.back(), 0);
    }

    void PooledUploadHeap::ReclaimAllocations()
    {
        for (Chunk& chunk : m_chunks)
        {
            auto* allocs = &chunk.allocations;
            allocs->remove_if([](const Allocation& allocation) {
                return !allocation.locked && allocation.doneEvent.IsSignaled();
            });
        }
    }

    GpuEvent PooledUploadHeap::BeginUploadToGpu(
        ID3D12Resource* dst,
        uint64_t dstOffset,
        D3D12_RESOURCE_STATES dstState,
        gsl::span<const std::byte> src)
    {
        assert(!src.empty());
        assert(dst->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

        InvariantChecker checker(this);

        ReclaimAllocations();

        // Allocate space from the upload heap
        Chunk* chunk = nullptr;
        size_t offsetInChunk = 0;
        std::tie(chunk, offsetInChunk) = Reserve(src.size());

        assert(chunk != nullptr);
        assert(offsetInChunk + src.size() <= chunk->capacityInBytes);

        // Map the upload heap and copy the source data into it at the specified offset
        void* uploadHeapData = nullptr;
        ORT_THROW_IF_FAILED(chunk->resource->Map(0, nullptr, &uploadHeapData));
        memcpy(static_cast<byte*>(uploadHeapData) + offsetInChunk, src.data(), src.size());
        chunk->resource->Unmap(0, nullptr);

        // Copy from the upload heap into the destination resource
        m_executionContext->CopyBufferRegion(
            dst,
            dstOffset,
            dstState,
            chunk->resource.Get(),
            offsetInChunk,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            src.size());

        GpuEvent doneEvent = m_executionContext->GetCurrentCompletionEvent();

        // Add an allocation entry to the chunk
        chunk->allocations.push_back(Allocation{ static_cast<size_t>(src.size()), offsetInChunk, doneEvent, false });

        return doneEvent;
    }

    GpuEvent PooledUploadHeap::BeginReusableUploadToGpu(
        ID3D12Resource* dst,
        uint64_t dstOffset,
        D3D12_RESOURCE_STATES dstState,
        gsl::span<const std::byte> src)
    {
        assert(!src.empty());
        assert(dst->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

        InvariantChecker checker(this);

        ReusableCopyKey key{};
        key.dstOffset = dstOffset;
        key.srcSizeInBytes = src.size();
        key.dstResource = dst;

        auto cacheIter = m_reusableCommandListsCache.find(key);

        // Unlock the least recently used command list before reclaiming the allocations
        if (cacheIter == m_reusableCommandListsCache.end() && m_reusableCommandLists.size() == maxReusableCommandLists)
        {
            m_executionContext->QueueReference(m_reusableCommandLists.front().commandList.Get());
            m_executionContext->QueueReference(m_reusableCommandLists.front().commandAllocator.Get());
            m_reusableCommandLists.front().allocation->locked = false;
            m_reusableCommandLists.pop_front();
            m_reusableCommandListsCache.erase(key);
        }

        ReclaimAllocations();

        if (cacheIter == m_reusableCommandListsCache.end())
        {
            // Allocate space from the upload heap
            Chunk* chunk = nullptr;
            size_t offsetInChunk = 0;
            std::tie(chunk, offsetInChunk) = Reserve(src.size());

            assert(chunk != nullptr);
            assert(offsetInChunk + src.size() <= chunk->capacityInBytes);

            ComPtr<ID3D12CommandAllocator> allocator;
            ORT_THROW_IF_FAILED(m_device->CreateCommandAllocator(
                m_executionContext->GetCommandListTypeForQueue(),
                IID_GRAPHICS_PPV_ARGS(allocator.ReleaseAndGetAddressOf())));

            ComPtr<ID3D12GraphicsCommandList> commandList;
            ORT_THROW_IF_FAILED(m_device->CreateCommandList(
                0,
                m_executionContext->GetCommandListTypeForQueue(),
                allocator.Get(),
                nullptr,
                IID_GRAPHICS_PPV_ARGS(commandList.ReleaseAndGetAddressOf())));

            // Map the upload heap and copy the source data into it at the specified offset
            void* uploadHeapData = nullptr;
            ORT_THROW_IF_FAILED(chunk->resource->Map(0, nullptr, &uploadHeapData));
            memcpy(static_cast<byte*>(uploadHeapData) + offsetInChunk, src.data(), src.size());
            chunk->resource->Unmap(0, nullptr);

            // Copy from the upload heap into the destination resource
            std::vector<D3D12_RESOURCE_BARRIER> barriers;

            if (!(dstState & D3D12_RESOURCE_STATE_COPY_DEST))
            {
                auto resourceBarrier = CD3DX12_RESOURCE_BARRIER::Transition(dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
                commandList->ResourceBarrier(1, &resourceBarrier);
            }

            commandList->CopyBufferRegion(dst, dstOffset, chunk->resource.Get(), offsetInChunk, src.size());

            if (!(dstState & D3D12_RESOURCE_STATE_COPY_DEST))
            {
                auto resourceBarrier = CD3DX12_RESOURCE_BARRIER::Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, dstState);
                commandList->ResourceBarrier(1, &resourceBarrier);
            }

            commandList->Close();

            GpuEvent doneEvent = m_executionContext->GetCurrentCompletionEvent();

            // Add an allocation entry to the chunk
            chunk->allocations.push_back(Allocation{ static_cast<size_t>(src.size()), offsetInChunk, doneEvent, true });

            static_assert(
                std::is_same_v<decltype(chunk->allocations), std::list<Allocation>>,
                "Make sure chunk->allocations uses a container that doesn't invalidate pointers when resizing if using &chunk->allocations.back()");

            ReusableCopyCommandListState commandListState{};
            commandListState.commandAllocator = std::move(allocator);
            commandListState.commandList = std::move(commandList);
            commandListState.allocation = &chunk->allocations.back();
            commandListState.chunkResource = chunk->resource.Get();

            ComPtr<ID3D12Fence> fence;
            uint64_t completionValue;
            m_executionContext->ExecuteCommandList(commandListState.commandList.Get(), &fence, &completionValue);

            // Add the command list to the cache
            m_reusableCommandLists.push_back(std::move(commandListState));
            m_reusableCommandListsCache.emplace(std::move(key), std::next(m_reusableCommandLists.end(), -1));
        }
        else
        {
            auto offsetInChunk = cacheIter->second->allocation->offsetInChunk;
            auto chunkResource = cacheIter->second->chunkResource;

            // Map the upload heap and copy the source data into it at the specified offset
            void* uploadHeapData = nullptr;
            ORT_THROW_IF_FAILED(chunkResource->Map(0, nullptr, &uploadHeapData));
            memcpy(static_cast<byte*>(uploadHeapData) + offsetInChunk, src.data(), src.size());
            chunkResource->Unmap(0, nullptr);

            ComPtr<ID3D12Fence> fence;
            uint64_t completionValue;
            m_executionContext->ExecuteCommandList(cacheIter->second->commandList.Get(), &fence, &completionValue);

            // Update the value that the allocation can be freed at
            cacheIter->second->allocation->doneEvent = m_executionContext->GetCurrentCompletionEvent();
        }

        GpuEvent doneEvent = m_executionContext->GetCurrentCompletionEvent();

        return doneEvent;
    }

    void PooledUploadHeap::Trim()
    {
        InvariantChecker checker(this);

        ReclaimAllocations();

        // Release any chunks which have no allocations
        auto it = std::remove_if(m_chunks.begin(), m_chunks.end(), [](const Chunk& c) {
            return c.allocations.empty();
        });
        m_chunks.erase(it, m_chunks.end());

        // Re-calculate total capacity
        m_totalCapacity = 0;
        for (const auto& chunk : m_chunks)
        {
            m_totalCapacity += chunk.capacityInBytes;
        }
    }

    void PooledUploadHeap::AssertInvariants()
    {
    #ifdef _DEBUG

        auto chunkCapacityComparer = [](const Chunk& lhs, const Chunk& rhs) {
            return lhs.capacityInBytes < rhs.capacityInBytes;
        };

        // Chunks should be sorted by ascending capacity
        assert(std::is_sorted(m_chunks.begin(), m_chunks.end(), chunkCapacityComparer));

        // Validate chunk properties
        for (const auto& chunk : m_chunks)
        {
            assert(chunk.resource != nullptr);
            assert(chunk.capacityInBytes == chunk.resource->GetDesc().Width);
        }

        // Validate allocation properties
        for (const auto& chunk : m_chunks)
        {
            for (const auto& alloc : chunk.allocations)
            {
                assert(alloc.offsetInChunk + alloc.sizeInBytes <= chunk.capacityInBytes);
                assert(alloc.offsetInChunk % c_allocationAlignment == 0); // Validate alignment
            }
        }

        // Validate no overlapping allocations
        for (const auto& chunk : m_chunks)
        {
            auto allocOffsetComparer = [](const Allocation& lhs, const Allocation& rhs) {
                return lhs.offsetInChunk < rhs.offsetInChunk;
            };

            std::vector<Allocation> allocationsSortedByOffset(chunk.allocations.begin(), chunk.allocations.end());
            std::sort(allocationsSortedByOffset.begin(), allocationsSortedByOffset.end(), allocOffsetComparer);

            for (size_t i = 1; i < allocationsSortedByOffset.size(); ++i)
            {
                const auto& alloc = allocationsSortedByOffset[i - 1];
                const auto& nextAlloc = allocationsSortedByOffset[i];
                assert(alloc.offsetInChunk + alloc.sizeInBytes <= nextAlloc.offsetInChunk);
            }
        }

        // Validate total capacity of pool
        size_t calculatedCapacity = 0;
        for (const auto& chunk : m_chunks)
        {
            calculatedCapacity += chunk.capacityInBytes;
        }
        assert(calculatedCapacity == m_totalCapacity);

    #endif // #ifdef _DEBUG
    }
} // namespace Dml
