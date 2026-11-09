#pragma once

#include "Common.h"
#include "Math.h"

#include <d3d12.h>

#include <array>
#include <cstdint>

class DescriptorAllocator;

// The state of every texture that is being read. It covers compute and pixel
// shaders at once, so the renderer can read what the solver wrote last without
// another transition, and the solver can read it again on the next frame.
inline constexpr D3D12_RESOURCE_STATES ShaderReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct TextureViews
{
    D3D12_CPU_DESCRIPTOR_HANDLE srv;
    D3D12_CPU_DESCRIPTOR_HANDLE uav;
};

// Collects resource barriers so that they reach the command list as a single
// ResourceBarrier call.
class BarrierBatch
{
public:
    explicit BarrierBatch(ID3D12GraphicsCommandList* commandList) noexcept : m_commandList(commandList) {}

    void Add(const D3D12_RESOURCE_BARRIER& barrier);
    void Flush();

private:
    ID3D12GraphicsCommandList* m_commandList;
    std::array<D3D12_RESOURCE_BARRIER, 16> m_barriers = {};
    uint32_t m_count = 0;
};

// A 3D texture with an SRV, a UAV and a tracked resource state.
class GpuTexture3D
{
public:
    GpuTexture3D(ID3D12Device* device, DescriptorAllocator& descriptors, UInt3 size, DXGI_FORMAT format, const wchar_t* name);

    // Views of no resource at all, for descriptor slots a shader does not use.
    // Reads through them return zero and writes are dropped.
    static TextureViews CreateNullViews(ID3D12Device* device, DescriptorAllocator& descriptors, DXGI_FORMAT format);

    ID3D12Resource* Resource() const noexcept { return m_resource.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE Srv() const noexcept { return m_views.srv; }
    D3D12_CPU_DESCRIPTOR_HANDLE Uav() const noexcept { return m_views.uav; }

    // Adds the barrier that moves the texture into `state`. A texture that is
    // already in the unordered access state gets a UAV barrier instead, because
    // two writes in a row, say a clear and a dispatch, must not overlap.
    void Transition(BarrierBatch& barriers, D3D12_RESOURCE_STATES state);

private:
    ComPtr<ID3D12Resource> m_resource;
    TextureViews m_views = {};
    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
