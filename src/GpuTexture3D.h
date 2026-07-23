#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

class DescriptorAllocator;

// A 3D texture with a shader resource view, an unordered access view and a tracked resource state.
class GpuTexture3D
{
public:
    bool Create(
        ID3D12Device* device,
        DescriptorAllocator& allocator,
        uint32_t width,
        uint32_t height,
        uint32_t depth,
        DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState,
        const wchar_t* debugName);

    void Transition(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState);
    void InsertUavBarrier(ID3D12GraphicsCommandList* commandList);

    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrv() const { return m_srv; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetUav() const { return m_uav; }
    D3D12_RESOURCE_STATES GetState() const { return m_state; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE m_srv = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_uav = {};
    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
};
