#include "GraphicsDevice.h"

#include <format>
#include <stdexcept>

namespace
{
    // The first hardware adapter that runs Direct3D 12 at feature level 11_0.
    // DXGI is asked for the high performance GPU first, so that a laptop with
    // hybrid graphics does not run the simulation on its integrated GPU.
    ComPtr<IDXGIAdapter1> FindAdapter(IDXGIFactory4* factory)
    {
        ComPtr<IDXGIFactory6> factory6;
        factory->QueryInterface(IID_PPV_ARGS(&factory6));

        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory6
                ? factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
                : factory->EnumAdapters1(index, &adapter);

            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            ThrowIfFailed(result);

            DXGI_ADAPTER_DESC1 description = {};
            ThrowIfFailed(adapter->GetDesc1(&description));
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                continue;
            }

            // Without an output pointer this only checks that the device could be created.
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            {
                return adapter;
            }
        }

        throw std::runtime_error("No GPU with Direct3D 12 support at feature level 11_0 was found.");
    }
}

GraphicsDevice::GraphicsDevice(HWND window, uint32_t width, uint32_t height)
    : m_width(width)
    , m_height(height)
{
    UINT factoryFlags = 0;

#if defined(_DEBUG)
    // The debug layer comes with the optional Graphics Tools feature of
    // Windows. Without it the Debug build simply runs unvalidated.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)));

    const ComPtr<IDXGIAdapter1> adapter = FindAdapter(factory.Get());
    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));

    const D3D12_COMMAND_QUEUE_DESC queueDesc = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)));

    const DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {
        .Width = width,
        .Height = height,
        .Format = BackBufferFormat,
        .SampleDesc = { .Count = 1 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = FrameCount,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
    };

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(m_queue.Get(), window, &swapChainDesc, nullptr, nullptr, &swapChain));
    ThrowIfFailed(swapChain.As(&m_swapChain));

    // Alt+Enter would switch to exclusive fullscreen, which is not supported.
    ThrowIfFailed(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));

    const D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = FrameCount,
    };
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CreateRenderTargets();

    for (ComPtr<ID3D12CommandAllocator>& allocator : m_commandAllocators)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    ThrowIfFailed(m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[m_frameIndex].Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close());

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValues[m_frameIndex] = 1;

    m_fenceEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!m_fenceEvent)
    {
        ThrowLastError("CreateEventW");
    }
}

GraphicsDevice::~GraphicsDevice()
{
    WaitForIdle();
}

ID3D12GraphicsCommandList* GraphicsDevice::BeginFrame()
{
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    TransitionBackBuffer(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE renderTarget = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    renderTarget.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1, &renderTarget, FALSE, nullptr);

    return m_commandList.Get();
}

void GraphicsDevice::EndFrame()
{
    TransitionBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* const lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);

    const HRESULT presented = m_swapChain->Present(1, 0);
    if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET)
    {
        // Usually Windows resetting a GPU that ran one dispatch for too long.
        throw std::runtime_error(std::format(
            "The GPU stopped responding and was reset by Windows (device removed reason 0x{:08X}).",
            static_cast<uint32_t>(m_device->GetDeviceRemovedReason())));
    }
    ThrowIfFailed(presented);

    const uint64_t submitted = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_queue->Signal(m_fence.Get(), submitted));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    WaitForFence(m_fenceValues[m_frameIndex]);
    m_fenceValues[m_frameIndex] = submitted + 1;
}

void GraphicsDevice::WaitForIdle() noexcept
{
    const uint64_t value = m_fenceValues[m_frameIndex];
    if (SUCCEEDED(m_queue->Signal(m_fence.Get(), value)))
    {
        WaitForFence(value);
        ++m_fenceValues[m_frameIndex];
    }
}

void GraphicsDevice::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (width == m_width && height == m_height))
    {
        return;
    }

    // ResizeBuffers fails while anything still references the old buffers.
    WaitForIdle();
    for (ComPtr<ID3D12Resource>& renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
    m_fenceValues.fill(m_fenceValues[m_frameIndex]);

    ThrowIfFailed(m_swapChain->ResizeBuffers(FrameCount, width, height, BackBufferFormat, 0));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_width = width;
    m_height = height;
    CreateRenderTargets();
}

void GraphicsDevice::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t index = 0; index < FrameCount; ++index)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_renderTargets[index])));
        m_device->CreateRenderTargetView(m_renderTargets[index].Get(), nullptr, handle);
        handle.ptr += m_rtvDescriptorSize;
    }
}

void GraphicsDevice::TransitionBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {
        .pResource = m_renderTargets[m_frameIndex].Get(),
        .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        .StateBefore = before,
        .StateAfter = after,
    };

    m_commandList->ResourceBarrier(1, &barrier);
}

void GraphicsDevice::WaitForFence(uint64_t value) noexcept
{
    if (m_fence->GetCompletedValue() >= value)
    {
        return;
    }

    if (SUCCEEDED(m_fence->SetEventOnCompletion(value, m_fenceEvent.get())))
    {
        WaitForSingleObjectEx(m_fenceEvent.get(), INFINITE, FALSE);
    }
}
