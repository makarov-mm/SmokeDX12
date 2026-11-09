#pragma once

#include "Common.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <cstdint>
#include <memory>

// Device, swap chain, command list and frame synchronisation.
//
// Every back buffer has its own command allocator and fence value. A frame
// waits for the GPU to finish the previous frame that used the same back
// buffer, so per frame data indexed by FrameIndex() is safe to overwrite.
class GraphicsDevice
{
public:
    static constexpr uint32_t FrameCount = 3;
    static constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    GraphicsDevice(HWND window, uint32_t width, uint32_t height);
    ~GraphicsDevice();

    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    // Resets the command list and binds the back buffer as the render target.
    ID3D12GraphicsCommandList* BeginFrame();
    // Submits the frame, presents it and waits until the next back buffer is free.
    void EndFrame();
    // Blocks until the GPU has finished everything submitted so far.
    void WaitForIdle() noexcept;
    void Resize(uint32_t width, uint32_t height);

    ID3D12Device* Device() const noexcept { return m_device.Get(); }
    uint32_t FrameIndex() const noexcept { return m_frameIndex; }
    uint32_t Width() const noexcept { return m_width; }
    uint32_t Height() const noexcept { return m_height; }

private:
    struct HandleCloser
    {
        void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
    };

    void CreateRenderTargets();
    void TransitionBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void WaitForFence(uint64_t value) noexcept;

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<IDXGISwapChain3> m_swapChain;

    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    std::array<ComPtr<ID3D12Resource>, FrameCount> m_renderTargets;
    std::array<ComPtr<ID3D12CommandAllocator>, FrameCount> m_commandAllocators;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    ComPtr<ID3D12Fence> m_fence;
    std::unique_ptr<void, HandleCloser> m_fenceEvent;
    std::array<uint64_t, FrameCount> m_fenceValues = {};

    uint32_t m_rtvDescriptorSize = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
