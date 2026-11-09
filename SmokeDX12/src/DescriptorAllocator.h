#pragma once

#include "Common.h"

#include <d3d12.h>

#include <cstdint>
#include <span>

// Location of a block of descriptors in the shader visible heap.
struct DescriptorTable
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};

// Owns two CBV/SRV/UAV descriptor heaps.
//
// The staging heap is never bound to the pipeline. Every view is created there
// once and stays for the lifetime of the allocator.
//
// The shader visible heap is a linear ring split into one segment per frame in
// flight. Right before a draw, a dispatch or a clear, the descriptors it needs
// are copied into the current segment as one contiguous block, so a root
// descriptor table can point at them. A segment is reused only after the fence
// has shown that the GPU is done with the frame that filled it.
class DescriptorAllocator
{
public:
    DescriptorAllocator(ID3D12Device* device, uint32_t stagingCapacity, uint32_t ringCapacityPerFrame, uint32_t frameCount);

    D3D12_CPU_DESCRIPTOR_HANDLE AllocateStaging();

    void BeginFrame(uint32_t frameIndex) noexcept;
    DescriptorTable Upload(std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors);

    ID3D12DescriptorHeap* ShaderVisibleHeap() const noexcept { return m_ringHeap.Get(); }

private:
    ID3D12Device* m_device;
    uint32_t m_descriptorSize;

    ComPtr<ID3D12DescriptorHeap> m_stagingHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_stagingStart = {};
    uint32_t m_stagingCapacity;
    uint32_t m_stagingUsed = 0;

    ComPtr<ID3D12DescriptorHeap> m_ringHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_ringCpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_ringGpuStart = {};
    uint32_t m_ringCapacityPerFrame;
    uint32_t m_frameStart = 0;
    uint32_t m_frameUsed = 0;
};
