#pragma once

// FloarLog.h - optional sink-based logging for Floar internals.
// With no sink installed, every FLOAR_LOG_* call is a silent no-op: the
// arguments are not evaluated and nothing is formatted.
// Host apps (e.g. FICture2) call Floar::Log::SetSink() once at startup.
// Messages are UTF-8. Pass paths and other wide strings through Log::ToUtf8
// (never std::filesystem::path::string(), which uses the active code page and
// throws for characters outside it). A logging failure (formatting, conversion,
// or a throwing sink) is swallowed so it can never escape an archive operation.

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

namespace Floar
{
namespace Log
{
    enum class Level { Trace, Debug, Info, Warn, Error };

    using Sink = void(*)(Level level, const std::string& message);

    void SetSink(Sink sink);

    // Non-throwing UTF-16 -> UTF-8 conversion. Unpaired surrogates become U+FFFD.
    std::string ToUtf8(std::wstring_view text) noexcept;
    std::string ToUtf8(const std::filesystem::path& path) noexcept;
    // Exact-match overloads: std::wstring and wide literals convert to both
    // std::wstring_view and std::filesystem::path, which would be ambiguous.
    inline std::string ToUtf8(const std::wstring& text) noexcept { return ToUtf8(std::wstring_view(text)); }
    inline std::string ToUtf8(const wchar_t* text) noexcept
    {
        return text ? ToUtf8(std::wstring_view(text)) : std::string();
    }
    // Non-throwing conversion of narrow text in `codePage` (for example a
    // library diagnostic) to UTF-8. Bytes that do not decode become U+FFFD (or
    // '?' if the code page is unavailable), so the result is always valid UTF-8.
    std::string CodePageToUtf8(std::string_view text, unsigned codePage) noexcept;

    namespace Detail
    {
        bool HasSink() noexcept;
        void Dispatch(Level level, std::string message);

        template <typename T>
        void FormatArg(std::ostringstream& out, const T& value)
        {
            out << value;
        }

        inline void FormatArg(std::ostringstream& out, bool value)
        {
            out << (value ? "true" : "false");
        }

        template <typename... Args>
        std::string Format(const char* fmt, const Args&... args)
        {
            std::ostringstream out;
            const char* p = fmt;
            auto emitOne = [&](const auto& value)
            {
                while (*p != '\0' && !(p[0] == '{' && p[1] == '}'))
                {
                    out << *p++;
                }
                if (*p != '\0')
                {
                    p += 2;
                }
                FormatArg(out, value);
            };
            (emitOne(args), ...);
            out << p;
            return out.str();
        }
    }
}
}

// Arguments are evaluated and formatted only when a sink is installed.
#define FLOAR_LOG_AT(level, ...) \
    do { \
        if (::Floar::Log::Detail::HasSink()) \
        { \
            try \
            { \
                ::Floar::Log::Detail::Dispatch((level), ::Floar::Log::Detail::Format(__VA_ARGS__)); \
            } \
            catch (...) \
            { \
            } \
        } \
    } while (0)

#define FLOAR_LOG_INFO(...)   FLOAR_LOG_AT(::Floar::Log::Level::Info,  __VA_ARGS__)
#define FLOAR_LOG_WARN(...)   FLOAR_LOG_AT(::Floar::Log::Level::Warn,  __VA_ARGS__)
#define FLOAR_LOG_ERROR(...)  FLOAR_LOG_AT(::Floar::Log::Level::Error, __VA_ARGS__)

#ifdef _DEBUG
#define FLOAR_LOG_TRACE(...)  FLOAR_LOG_AT(::Floar::Log::Level::Trace, __VA_ARGS__)
#define FLOAR_LOG_DEBUG(...)  FLOAR_LOG_AT(::Floar::Log::Level::Debug, __VA_ARGS__)
#else
#define FLOAR_LOG_TRACE(...)  do {} while (0)
#define FLOAR_LOG_DEBUG(...)  do {} while (0)
#endif

#define FLOAR_TIMER_START(var) \
    auto var = std::chrono::steady_clock::now()

#define FLOAR_ELAPSED_MS(var) \
    (std::chrono::duration_cast<std::chrono::milliseconds>( \
        std::chrono::steady_clock::now() - (var)).count())
