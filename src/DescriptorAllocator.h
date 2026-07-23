#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

// Owns two descriptor heaps.
// The CPU heap holds one persistent descriptor per view and is never bound to the pipeline.
// The GPU heap is a per frame linear ring, descriptors are copied into it right before a draw
// or a dispatch so that root descriptor tables always point at a contiguous block.
class DescriptorAllocator
{
public:
    bool Initialize(ID3D12Device* device, uint32_t cpuCapacity, uint32_t gpuCapacityPerFrame, uint32_t frameCount);
    void Shutdown();

    D3D12_CPU_DESCRIPTOR_HANDLE AllocatePersistent();

    void BeginFrame(uint32_t frameIndex);
    D3D12_GPU_DESCRIPTOR_HANDLE CopyToShaderVisibleHeap(const D3D12_CPU_DESCRIPTOR_HANDLE* handles, uint32_t count);

    ID3D12DescriptorHeap* GetShaderVisibleHeap() const { return m_gpuHeap.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cpuHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gpuHeap;

    uint32_t m_descriptorSize = 0;
    uint32_t m_cpuCapacity = 0;
    uint32_t m_cpuUsed = 0;

    uint32_t m_gpuCapacityPerFrame = 0;
    uint32_t m_frameCount = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_gpuUsed = 0;
};
