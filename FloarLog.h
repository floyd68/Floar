#pragma once

// FloarLog.h - optional sink-based logging for Floar internals.
// With no sink installed, every FLOAR_LOG_* call is a silent no-op.
// Host apps (e.g. FICture2) call Floar::Log::SetSink() once at startup.

#include <chrono>
#include <sstream>
#include <string>

namespace Floar
{
namespace Log
{
    enum class Level { Trace, Debug, Info, Warn, Error };

    using Sink = void(*)(Level level, const std::string& message);

    void SetSink(Sink sink);

    namespace Detail
    {
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

#define FLOAR_LOG_INFO(...)   ::Floar::Log::Detail::Dispatch(::Floar::Log::Level::Info,  ::Floar::Log::Detail::Format(__VA_ARGS__))
#define FLOAR_LOG_WARN(...)   ::Floar::Log::Detail::Dispatch(::Floar::Log::Level::Warn,  ::Floar::Log::Detail::Format(__VA_ARGS__))
#define FLOAR_LOG_ERROR(...)  ::Floar::Log::Detail::Dispatch(::Floar::Log::Level::Error, ::Floar::Log::Detail::Format(__VA_ARGS__))

#ifdef _DEBUG
#define FLOAR_LOG_TRACE(...)  ::Floar::Log::Detail::Dispatch(::Floar::Log::Level::Trace, ::Floar::Log::Detail::Format(__VA_ARGS__))
#define FLOAR_LOG_DEBUG(...)  ::Floar::Log::Detail::Dispatch(::Floar::Log::Level::Debug, ::Floar::Log::Detail::Format(__VA_ARGS__))
#else
#define FLOAR_LOG_TRACE(...)  do {} while (0)
#define FLOAR_LOG_DEBUG(...)  do {} while (0)
#endif

#define FLOAR_TIMER_START(var) \
    auto var = std::chrono::steady_clock::now()

#define FLOAR_ELAPSED_MS(var) \
    (std::chrono::duration_cast<std::chrono::milliseconds>( \
        std::chrono::steady_clock::now() - (var)).count())
