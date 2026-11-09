#include "Common.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <stdexcept>

namespace
{
    // The system description of an error code, without the trailing line break.
    std::string DescribeError(DWORD code)
    {
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            code,
            0,
            reinterpret_cast<wchar_t*>(&buffer),
            0,
            nullptr);

        if (length == 0)
        {
            return {};
        }

        std::wstring text(buffer, length);
        LocalFree(buffer);

        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
        {
            text.pop_back();
        }

        return ToUtf8(text);
    }
}

void ThrowIfFailed(HRESULT result, std::source_location location)
{
    if (SUCCEEDED(result))
    {
        return;
    }

    const std::string file = std::filesystem::path(location.file_name()).filename().string();
    const std::string description = DescribeError(static_cast<DWORD>(result));

    throw std::runtime_error(std::format(
        "A Direct3D call failed with 0x{:08X} at {}:{}.{}{}",
        static_cast<uint32_t>(result),
        file,
        location.line(),
        description.empty() ? "" : "\n\n",
        description));
}

void ThrowLastError(std::string_view call)
{
    const DWORD code = GetLastError();
    throw std::runtime_error(std::format("{} failed with error {}. {}", call, code, DescribeError(code)));
}

std::string ToUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int sourceLength = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0, nullptr, nullptr);

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength, result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring ToWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int sourceLength = static_cast<int>(text.size());
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0);

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, result.data(), length);
    return result;
}

void ShowErrorMessage(HWND owner, std::string_view text)
{
    MessageBoxW(owner, ToWide(text).c_str(), L"SmokeDX12", MB_OK | MB_ICONERROR);
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc, const wchar_t* name)
{
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;

    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        const std::string_view message = errors
            ? std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
            : std::string_view("unknown error");

        throw std::runtime_error(std::format("Invalid root signature: {}", message));
    }

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature)));

    rootSignature->SetName(name);
    return rootSignature;
}
