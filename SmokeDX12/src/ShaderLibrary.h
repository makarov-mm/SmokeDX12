#pragma once

#include "Common.h"

#include <d3dcommon.h>

#include <filesystem>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

// A shader that did not compile. The message is the compiler output.
class ShaderCompileError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct ShaderEntryPoint
{
    const char* name;
    const char* target;
};

// Compiles HLSL from the shaders folder at run time with FXC.
//
// Shader model 5.1 covers everything this project does, and d3dcompiler_47.dll
// is part of Windows, so there is no compiler to ship next to the executable.
// Compiling at run time is also what makes hot reloading possible.
class ShaderLibrary
{
public:
    // Uses the project's own shaders folder when the executable runs inside the
    // project's build tree, and otherwise the nearest shaders folder next to the
    // executable or above it.
    ShaderLibrary();

    // Compiles the entry points of one file in parallel. Throws ShaderCompileError
    // with the output of every entry point that failed.
    std::vector<ComPtr<ID3DBlob>> Compile(std::wstring_view fileName, std::span<const ShaderEntryPoint> entryPoints) const;

private:
    std::filesystem::path m_directory;
};
