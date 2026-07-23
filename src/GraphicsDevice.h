#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>

// Device, swap chain, command list and frame synchronisation.
class GraphicsDevice
{
public:
    static const uint32_t FrameCount = 3;

    bool Initialize(HWND window, uint32_t width, uint32_t height, bool enableDebugLayer);
    void Shutdown();

    void BeginFrame();
    void EndFrame();
    void WaitForGpu();
    void Resize(uint32_t width, uint32_t height);

    ID3D12Device* GetDevice() const { return m_device.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return m_commandQueue.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }

    ID3D12Resource* GetBackBuffer() const { return m_renderTargets[m_frameIndex].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferView() const;

    void TransitionBackBuffer(D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter);

    uint32_t GetFrameIndex() const { return m_frameIndex; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    bool CreateRenderTargets();

    Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocators[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValues[FrameCount] = {};

    uint32_t m_rtvDescriptorSize = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
