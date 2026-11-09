#include "ShaderLibrary.h"

#include <d3dcompiler.h>

#include <cstdint>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>

namespace
{
    std::filesystem::path ExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');

        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0)
            {
                ThrowLastError("GetModuleFileNameW");
            }

            if (length < path.size())
            {
                path.resize(length);
                return std::filesystem::path(path).parent_path();
            }

            // The path was truncated, which happens with long path support enabled.
            path.resize(path.size() * 2);
        }
    }

    std::optional<std::string> ReadFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return std::nullopt;
        }

        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // Compiler output is a null terminated string inside the blob.
    std::string BlobText(ID3DBlob* blob)
    {
        std::string_view text(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
        while (!text.empty() && text.back() == '\0')
        {
            text.remove_suffix(1);
        }
        return std::string(text);
    }

    // Resolves #include against the shaders folder through std::filesystem.
    // FXC's standard handler derives the folder from the ANSI source name
    // instead, which cannot represent every path a Windows user can have.
    // The handler keeps no state, so concurrent compiles can share it.
    class IncludeHandler final : public ID3DInclude
    {
    public:
        explicit IncludeHandler(const std::filesystem::path& directory) noexcept
            : m_directory(directory)
        {
        }

        HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID, LPCVOID* data, UINT* bytes) override
        {
            try
            {
                const std::optional<std::string> text = ReadFile(m_directory / fileName);
                if (!text)
                {
                    return E_FAIL;
                }

                auto buffer = std::make_unique<char[]>(text->size());
                text->copy(buffer.get(), text->size());

                *data = buffer.release();
                *bytes = static_cast<UINT>(text->size());
                return S_OK;
            }
            catch (...)
            {
                return E_FAIL;
            }
        }

        HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override
        {
            delete[] static_cast<const char*>(data);
            return S_OK;
        }

    private:
        const std::filesystem::path& m_directory;
    };
}

ShaderLibrary::ShaderLibrary()
{
    std::error_code error;
    std::filesystem::path nearest;

    for (std::filesystem::path directory = ExecutableDirectory();; directory = directory.parent_path())
    {
        const std::filesystem::path candidate = directory / L"shaders";
        if (std::filesystem::is_directory(candidate, error))
        {
            // Inside the project's build tree the project's own folder wins over
            // the copy the post build step puts next to the executable, so that
            // hot reloading compiles the files that are actually being edited.
            if (std::filesystem::exists(directory / L"SmokeDX12.vcxproj", error))
            {
                m_directory = candidate;
                return;
            }

            if (nearest.empty())
            {
                nearest = candidate;
            }
        }

        if (directory == directory.parent_path())
        {
            break;
        }
    }

    if (nearest.empty())
    {
        throw std::runtime_error("Could not find the shaders folder next to the executable or in any folder above it.");
    }

    m_directory = nearest;
}

std::vector<ComPtr<ID3DBlob>> ShaderLibrary::Compile(std::wstring_view fileName, std::span<const ShaderEntryPoint> entryPoints) const
{
    const std::filesystem::path path = m_directory / fileName;
    const std::optional<std::string> source = ReadFile(path);
    if (!source)
    {
        throw ShaderCompileError(std::format("Could not read {}.", ToUtf8(path.native())));
    }

    // Only used in messages, which then name the file that has to be fixed.
    const std::string sourceName = ToUtf8(path.native());

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    IncludeHandler includes(m_directory);

    struct Output
    {
        ComPtr<ID3DBlob> code;
        std::string messages;
    };

    // FXC is the slowest part of start up and of every hot reload, and
    // D3DCompile may run on several threads at once. A blob only changes
    // threads by being moved out of its future: FXC blobs count references
    // without atomics, so they must never be shared between threads.
    std::vector<std::future<Output>> jobs;
    jobs.reserve(entryPoints.size());

    for (const ShaderEntryPoint& entryPoint : entryPoints)
    {
        jobs.push_back(std::async(std::launch::async, [&, entryPoint]
        {
            Output output;
            ComPtr<ID3DBlob> messages;

            const HRESULT result = D3DCompile(
                source->data(),
                source->size(),
                sourceName.c_str(),
                nullptr,
                &includes,
                entryPoint.name,
                entryPoint.target,
                flags,
                0,
                &output.code,
                &messages);

            if (messages)
            {
                output.messages = BlobText(messages.Get());
            }

            if (FAILED(result))
            {
                output.code.Reset();
                if (output.messages.empty())
                {
                    output.messages = std::format("{}: {} failed with 0x{:08X}.", sourceName, entryPoint.name, static_cast<uint32_t>(result));
                }
            }

            return output;
        }));
    }

    std::vector<ComPtr<ID3DBlob>> blobs;
    std::string errors;

    for (std::future<Output>& job : jobs)
    {
        Output output = job.get();

        if (output.code)
        {
            // Warnings of a successful compile only go to the debugger.
            if (!output.messages.empty())
            {
                OutputDebugStringA((output.messages + '\n').c_str());
            }
            blobs.push_back(std::move(output.code));
        }
        else if (errors.find(output.messages) == std::string::npos)
        {
            // Entry points that share the broken code report the same errors.
            errors += output.messages;
            errors += '\n';
        }
    }

    if (!errors.empty())
    {
        throw ShaderCompileError(errors);
    }

    return blobs;
}
