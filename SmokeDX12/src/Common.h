#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <source_location>
#include <string>
#include <string_view>

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Throws std::runtime_error naming the call site when `result` is a failure code.
void ThrowIfFailed(HRESULT result, std::source_location location = std::source_location::current());

// Throws std::runtime_error for a failed Win32 call, with the text of GetLastError.
[[noreturn]] void ThrowLastError(std::string_view call);

// Messages are UTF-8 throughout, Win32 wants UTF-16.
std::string ToUtf8(std::wstring_view text);
std::wstring ToWide(std::string_view text);

// Shows a modal error box with UTF-8 text.
void ShowErrorMessage(HWND owner, std::string_view text);

// Serialises a version 1.0 root signature and creates it, throwing with the
// serialiser's own message when the description is invalid.
ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc, const wchar_t* name);
