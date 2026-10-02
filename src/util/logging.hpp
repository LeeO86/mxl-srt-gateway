#pragma once

#include <string>
#include <utility>
#include <vector>

namespace srtgw
{
enum class LogLevel
{
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
};

class log
{
public:
    static void setLevel(std::string const& name);
    static void setLevel(LogLevel level);
    static LogLevel level();

    using Field = std::pair<std::string, std::string>;
    static void error(std::string const& event, std::vector<Field> const& fields = {});
    static void warn(std::string const& event, std::vector<Field> const& fields = {});
    static void info(std::string const& event, std::vector<Field> const& fields = {});
    static void debug(std::string const& event, std::vector<Field> const& fields = {});

    static std::string jsonEscape(std::string const& text);
    static std::string redact(std::string const& text);
};
} // namespace srtgw
