#include "GpuTexture3D.h"

#include "DescriptorAllocator.h"

namespace
{
    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc(DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture3D.MipLevels = 1;
        return desc;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc(DXGI_FORMAT format)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D.WSize = UINT(-1);
        return desc;
    }
}

void BarrierBatch::Add(const D3D12_RESOURCE_BARRIER& barrier)
{
    if (m_count == m_barriers.size())
    {
        Flush();
    }

    m_barriers[m_count++] = barrier;
}

void BarrierBatch::Flush()
{
    if (m_count > 0)
    {
        m_commandList->ResourceBarrier(m_count, m_barriers.data());
        m_count = 0;
    }
}

GpuTexture3D::GpuTexture3D(ID3D12Device* device, DescriptorAllocator& descriptors, UInt3 size, DXGI_FORMAT format, const wchar_t* name)
{
    const D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    const D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D,
        .Width = size.x,
        .Height = size.y,
        .DepthOrArraySize = static_cast<UINT16>(size.z),
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        m_state,
        nullptr,
        IID_PPV_ARGS(&m_resource)));
    m_resource->SetName(name);

    const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = SrvDesc(format);
    const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = UavDesc(format);

    m_views.srv = descriptors.AllocateStaging();
    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_views.srv);

    m_views.uav = descriptors.AllocateStaging();
    device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, m_views.uav);
}

TextureViews GpuTexture3D::CreateNullViews(ID3D12Device* device, DescriptorAllocator& descriptors, DXGI_FORMAT format)
{
    const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = SrvDesc(format);
    const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = UavDesc(format);

    const TextureViews views = {
        .srv = descriptors.AllocateStaging(),
        .uav = descriptors.AllocateStaging(),
    };

    device->CreateShaderResourceView(nullptr, &srvDesc, views.srv);
    device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, views.uav);
    return views;
}

void GpuTexture3D::Transition(BarrierBatch& barriers, D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_BARRIER barrier = {};

    if (state == m_state)
    {
        if (state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            return;
        }

        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_resource.Get();
    }
    else
    {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {
            .pResource = m_resource.Get(),
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = m_state,
            .StateAfter = state,
        };
        m_state = state;
    }

    barriers.Add(barrier);
}
