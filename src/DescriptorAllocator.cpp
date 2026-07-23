#include "DescriptorAllocator.h"

using Microsoft::WRL::ComPtr;

bool DescriptorAllocator::Initialize(ID3D12Device* device, uint32_t cpuCapacity, uint32_t gpuCapacityPerFrame, uint32_t frameCount)
{
    m_device = device;
    m_cpuCapacity = cpuCapacity;
    m_gpuCapacityPerFrame = gpuCapacityPerFrame;
    m_frameCount = frameCount;

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC cpuDesc = {};
    cpuDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cpuDesc.NumDescriptors = cpuCapacity;
    cpuDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(device->CreateDescriptorHeap(&cpuDesc, IID_PPV_ARGS(&m_cpuHeap))))
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC gpuDesc = {};
    gpuDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gpuDesc.NumDescriptors = gpuCapacityPerFrame * frameCount;
    gpuDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&gpuDesc, IID_PPV_ARGS(&m_gpuHeap))))
    {
        return false;
    }

    m_cpuHeap->SetName(L"CpuDescriptorHeap");
    m_gpuHeap->SetName(L"ShaderVisibleDescriptorHeap");
    return true;
}

void DescriptorAllocator::Shutdown()
{
    m_cpuHeap.Reset();
    m_gpuHeap.Reset();
    m_device.Reset();
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocator::AllocatePersistent()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_cpuUsed) * m_descriptorSize;
    ++m_cpuUsed;
    return handle;
}

void DescriptorAllocator::BeginFrame(uint32_t frameIndex)
{
    m_frameIndex = frameIndex;
    m_gpuUsed = 0;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorAllocator::CopyToShaderVisibleHeap(const D3D12_CPU_DESCRIPTOR_HANDLE* handles, uint32_t count)
{
    // Descriptors are appended into the segment reserved for the current frame.
    // The segment is only reused once the GPU has finished with that frame.
    const uint32_t base = m_frameIndex * m_gpuCapacityPerFrame + m_gpuUsed;
    m_gpuUsed += count;

    D3D12_CPU_DESCRIPTOR_HANDLE destinationCpu = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
    destinationCpu.ptr += static_cast<SIZE_T>(base) * m_descriptorSize;

    UINT destinationCount = count;
    static const UINT sourceCounts[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };

    m_device->CopyDescriptors(
        1,
        &destinationCpu,
        &destinationCount,
        count,
        handles,
        sourceCounts,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE destinationGpu = m_gpuHeap->GetGPUDescriptorHandleForHeapStart();
    destinationGpu.ptr += static_cast<UINT64>(base) * m_descriptorSize;
    return destinationGpu;
}
