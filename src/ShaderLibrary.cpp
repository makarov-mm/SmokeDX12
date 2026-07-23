#include "ShaderLibrary.h"

#include <Windows.h>
#include <d3dcompiler.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    bool DirectoryExists(const std::wstring& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::wstring GetExecutableDirectory()
    {
        std::vector<wchar_t> buffer(MAX_PATH);
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        std::wstring path(buffer.data(), length);

        size_t separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
        {
            return L".\\";
        }

        return path.substr(0, separator + 1);
    }
}

bool ShaderLibrary::Initialize()
{
    // The shader folder is looked up next to the executable first, then a few
    // levels up so that running straight from the Visual Studio build folder works.
    const std::wstring executableDirectory = GetExecutableDirectory();

    const std::wstring candidates[] =
    {
        executableDirectory + L"shaders\\",
        executableDirectory + L"..\\shaders\\",
        executableDirectory + L"..\\..\\shaders\\",
        executableDirectory + L"..\\..\\..\\shaders\\",
        executableDirectory + L"..\\..\\..\\..\\shaders\\",
        L"shaders\\"
    };

    for (const std::wstring& candidate : candidates)
    {
        if (DirectoryExists(candidate))
        {
            m_directory = candidate;
            return true;
        }
    }

    m_lastError = "Could not locate the shaders directory.";
    return false;
}

ComPtr<ID3DBlob> ShaderLibrary::Compile(const wchar_t* fileName, const char* entryPoint, const char* target)
{
    const std::wstring fullPath = m_directory + fileName;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;

    HRESULT result = D3DCompileFromFile(
        fullPath.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint,
        target,
        flags,
        0,
        &code,
        &errors);

    if (FAILED(result))
    {
        if (errors)
        {
            m_lastError.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        else
        {
            m_lastError = "Shader file not found or could not be opened.";
        }

        OutputDebugStringA(m_lastError.c_str());
        OutputDebugStringA("\n");
        return nullptr;
    }

    m_lastError.clear();
    return code;
}
