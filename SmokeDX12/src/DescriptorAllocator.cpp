#include "DescriptorAllocator.h"

#include <stdexcept>

DescriptorAllocator::DescriptorAllocator(ID3D12Device* device, uint32_t stagingCapacity, uint32_t ringCapacityPerFrame, uint32_t frameCount)
    : m_device(device)
    , m_descriptorSize(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV))
    , m_stagingCapacity(stagingCapacity)
    , m_ringCapacityPerFrame(ringCapacityPerFrame)
{
    const D3D12_DESCRIPTOR_HEAP_DESC stagingDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = stagingCapacity,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    ThrowIfFailed(device->CreateDescriptorHeap(&stagingDesc, IID_PPV_ARGS(&m_stagingHeap)));
    m_stagingHeap->SetName(L"Staging descriptors");
    m_stagingStart = m_stagingHeap->GetCPUDescriptorHandleForHeapStart();

    const D3D12_DESCRIPTOR_HEAP_DESC ringDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = ringCapacityPerFrame * frameCount,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    ThrowIfFailed(device->CreateDescriptorHeap(&ringDesc, IID_PPV_ARGS(&m_ringHeap)));
    m_ringHeap->SetName(L"Shader visible descriptor ring");
    m_ringCpuStart = m_ringHeap->GetCPUDescriptorHandleForHeapStart();
    m_ringGpuStart = m_ringHeap->GetGPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocator::AllocateStaging()
{
    if (m_stagingUsed == m_stagingCapacity)
    {
        throw std::length_error("The staging descriptor heap is full.");
    }

    return { m_stagingStart.ptr + static_cast<SIZE_T>(m_stagingUsed++) * m_descriptorSize };
}

void DescriptorAllocator::BeginFrame(uint32_t frameIndex) noexcept
{
    m_frameStart = frameIndex * m_ringCapacityPerFrame;
    m_frameUsed = 0;
}

DescriptorTable DescriptorAllocator::Upload(std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors)
{
    const auto count = static_cast<uint32_t>(descriptors.size());
    if (count > m_ringCapacityPerFrame - m_frameUsed)
    {
        throw std::length_error("The descriptor ring ran out of space for this frame.");
    }

    const uint32_t first = m_frameStart + m_frameUsed;
    m_frameUsed += count;

    const DescriptorTable table = {
        .cpu = { m_ringCpuStart.ptr + static_cast<SIZE_T>(first) * m_descriptorSize },
        .gpu = { m_ringGpuStart.ptr + static_cast<UINT64>(first) * m_descriptorSize },
    };

    // The sources are scattered over the staging heap, so they are copied one by one.
    for (uint32_t index = 0; index < count; ++index)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE destination = { table.cpu.ptr + static_cast<SIZE_T>(index) * m_descriptorSize };
        m_device->CopyDescriptorsSimple(1, destination, descriptors[index], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    return table;
}
