#include "GraphicsDevice.h"

#include <cstdio>

using Microsoft::WRL::ComPtr;

bool GraphicsDevice::Initialize(HWND window, uint32_t width, uint32_t height, bool enableDebugLayer)
{
    m_width = width;
    m_height = height;

    UINT factoryFlags = 0;

    if (enableDebugLayer)
    {
        // Requires the optional Graphics Tools feature of Windows.
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory))))
    {
        return false;
    }

    // Pick the first adapter that supports feature level 11_0.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; m_factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
    {
        DXGI_ADAPTER_DESC1 description = {};
        adapter->GetDesc1(&description);

        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
        {
            break;
        }
    }

    if (!m_device)
    {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue))))
    {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), window, &swapChainDesc, nullptr, nullptr, &swapChain)))
    {
        return false;
    }

    m_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    if (FAILED(swapChain.As(&m_swapChain)))
    {
        return false;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap))))
    {
        return false;
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!CreateRenderTargets())
    {
        return false;
    }

    for (uint32_t index = 0; index < FrameCount; ++index)
    {
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[index]))))
        {
            return false;
        }
    }

    if (FAILED(m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[m_frameIndex].Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList))))
    {
        return false;
    }

    m_commandList->Close();

    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
    {
        return false;
    }

    for (uint32_t index = 0; index < FrameCount; ++index)
    {
        m_fenceValues[index] = 0;
    }
    m_fenceValues[m_frameIndex] = 1;

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr)
    {
        return false;
    }

    return true;
}

bool GraphicsDevice::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t index = 0; index < FrameCount; ++index)
    {
        if (FAILED(m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_renderTargets[index]))))
        {
            return false;
        }

        m_device->CreateRenderTargetView(m_renderTargets[index].Get(), nullptr, handle);
        handle.ptr += m_rtvDescriptorSize;
    }

    return true;
}

void GraphicsDevice::Shutdown()
{
    WaitForGpu();

    if (m_fenceEvent != nullptr)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void GraphicsDevice::BeginFrame()
{
    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);
}

void GraphicsDevice::EndFrame()
{
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    m_swapChain->Present(1, 0);

    const uint64_t currentFenceValue = m_fenceValues[m_frameIndex];
    m_commandQueue->Signal(m_fence.Get(), currentFenceValue);

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
    {
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

void GraphicsDevice::WaitForGpu()
{
    if (!m_commandQueue || !m_fence || m_fenceEvent == nullptr)
    {
        return;
    }

    const uint64_t value = m_fenceValues[m_frameIndex];
    m_commandQueue->Signal(m_fence.Get(), value);

    if (m_fence->GetCompletedValue() < value)
    {
        m_fence->SetEventOnCompletion(value, m_fenceEvent);
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    ++m_fenceValues[m_frameIndex];
}

void GraphicsDevice::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (width == m_width && height == m_height))
    {
        return;
    }

    WaitForGpu();

    for (uint32_t index = 0; index < FrameCount; ++index)
    {
        m_renderTargets[index].Reset();
        m_fenceValues[index] = m_fenceValues[m_frameIndex];
    }

    m_swapChain->ResizeBuffers(FrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    m_width = width;
    m_height = height;

    CreateRenderTargets();
}

D3D12_CPU_DESCRIPTOR_HANDLE GraphicsDevice::GetBackBufferView() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    return handle;
}

void GraphicsDevice::TransitionBackBuffer(D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = stateBefore;
    barrier.Transition.StateAfter = stateAfter;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(1, &barrier);
}
