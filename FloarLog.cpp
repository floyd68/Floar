#include "FloarLog.h"

#include <windows.h>

#include <atomic>

namespace Floar
{
namespace Log
{
    namespace
    {
        std::atomic<Sink> s_sink { nullptr };
    }

    void SetSink(Sink sink)
    {
        s_sink.store(sink, std::memory_order_relaxed);
    }

    std::string ToUtf8(std::wstring_view text) noexcept
    {
        if (text.empty())
        {
            return {};
        }

        try
        {
            // Without WC_ERR_INVALID_CHARS, unpaired surrogates are replaced with
            // U+FFFD instead of failing the whole conversion.
            const int length = static_cast<int>(text.size());
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
            if (bytes <= 0)
            {
                return "<invalid UTF-16>";
            }

            std::string out(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), bytes, nullptr, nullptr);
            return out;
        }
        catch (...)
        {
            return {};
        }
    }

    std::string ToUtf8(const std::filesystem::path& path) noexcept
    {
        return ToUtf8(std::wstring_view(path.native()));
    }

    std::string CodePageToUtf8(std::string_view text, unsigned codePage) noexcept
    {
        bool ascii = true;
        for (const char c : text)
        {
            if (static_cast<unsigned char>(c) >= 0x80)
            {
                ascii = false;
                break;
            }
        }
        if (ascii)
        {
            try
            {
                return std::string(text);
            }
            catch (...)
            {
                return {};
            }
        }

        try
        {
            const int length = static_cast<int>(text.size());
            const int chars = MultiByteToWideChar(codePage, 0, text.data(), length, nullptr, 0);
            if (chars > 0)
            {
                std::wstring wide(static_cast<size_t>(chars), L'\0');
                MultiByteToWideChar(codePage, 0, text.data(), length, wide.data(), chars);
                return ToUtf8(std::wstring_view(wide));
            }

            // Code page not installed: keep ASCII and mark everything else.
            std::string out(text);
            for (char& c : out)
            {
                if (static_cast<unsigned char>(c) >= 0x80)
                {
                    c = '?';
                }
            }
            return out;
        }
        catch (...)
        {
            return {};
        }
    }

    namespace Detail
    {
        bool HasSink() noexcept
        {
            return s_sink.load(std::memory_order_relaxed) != nullptr;
        }

        void Dispatch(Level level, std::string message)
        {
            if (Sink sink = s_sink.load(std::memory_order_relaxed))
            {
                sink(level, message);
            }
        }
    }
}
}
