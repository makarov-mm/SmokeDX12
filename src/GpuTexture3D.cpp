#include "GpuTexture3D.h"

#include "DescriptorAllocator.h"

using Microsoft::WRL::ComPtr;

bool GpuTexture3D::Create(
    ID3D12Device* device,
    DescriptorAllocator& allocator,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    DXGI_FORMAT format,
    D3D12_RESOURCE_STATES initialState,
    const wchar_t* debugName)
{
    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = static_cast<UINT16>(depth);
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT result = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&m_resource));

    if (FAILED(result))
    {
        return false;
    }

    if (debugName != nullptr)
    {
        m_resource->SetName(debugName);
    }

    m_state = initialState;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture3D.MipLevels = 1;

    m_srv = allocator.AllocatePersistent();
    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srv);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uavDesc.Texture3D.WSize = depth;

    m_uav = allocator.AllocatePersistent();
    device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, m_uav);

    return true;
}

void GpuTexture3D::Transition(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState)
{
    if (m_state == newState)
    {
        if (newState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            InsertUavBarrier(commandList);
        }
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_resource.Get();
    barrier.Transition.StateBefore = m_state;
    barrier.Transition.StateAfter = newState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    commandList->ResourceBarrier(1, &barrier);
    m_state = newState;
}

void GpuTexture3D::InsertUavBarrier(ID3D12GraphicsCommandList* commandList)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_resource.Get();
    commandList->ResourceBarrier(1, &barrier);
}
