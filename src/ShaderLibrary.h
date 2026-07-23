#pragma once

#include <d3dcommon.h>
#include <wrl/client.h>
#include <string>

// Compiles HLSL from disk at run time with the legacy FXC compiler.
// Shader model 5.1 is enough for everything this project does and it avoids
// shipping a separate DXC compiler alongside the executable.
class ShaderLibrary
{
public:
    bool Initialize();

    const std::wstring& GetDirectory() const { return m_directory; }

    Microsoft::WRL::ComPtr<ID3DBlob> Compile(
        const wchar_t* fileName,
        const char* entryPoint,
        const char* target);

    const std::string& GetLastError() const { return m_lastError; }

private:
    std::wstring m_directory;
    std::string m_lastError;
};
