#include "util/logging.hpp"

#include <cstdio>
#include <iostream>
#include <mutex>

namespace srtgw
{
namespace
{
std::mutex gMu;
LogLevel gLevel = LogLevel::Info;

void write(LogLevel level, char const* name, std::string const& event, std::vector<log::Field> const& fields)
{
    if (static_cast<int>(level) > static_cast<int>(log::level()))
    {
        return;
    }
    std::string line = std::string("{\"level\":\"") + name + "\",\"event\":\"" + log::jsonEscape(event) + "\"";
    for (auto const& field : fields)
    {
        line += ",\"";
        line += log::jsonEscape(field.first);
        line += "\":\"";
        bool const secret = field.first.find("pass") != std::string::npos;
        line += log::jsonEscape(secret ? "***" : field.second);
        line += "\"";
    }
    line += "}";
    std::lock_guard const lock{gMu};
    std::cerr << line << '\n';
}
} // namespace

void log::setLevel(std::string const& name)
{
    if (name == "error")
    {
        setLevel(LogLevel::Error);
    }
    else if (name == "warn" || name == "warning")
    {
        setLevel(LogLevel::Warn);
    }
    else if (name == "debug" || name == "trace")
    {
        setLevel(LogLevel::Debug);
    }
    else
    {
        setLevel(LogLevel::Info);
    }
}

void log::setLevel(LogLevel level)
{
    std::lock_guard const lock{gMu};
    gLevel = level;
}

LogLevel log::level()
{
    std::lock_guard const lock{gMu};
    return gLevel;
}

void log::error(std::string const& event, std::vector<Field> const& fields)
{
    write(LogLevel::Error, "error", event, fields);
}

void log::warn(std::string const& event, std::vector<Field> const& fields)
{
    write(LogLevel::Warn, "warn", event, fields);
}

void log::info(std::string const& event, std::vector<Field> const& fields)
{
    write(LogLevel::Info, "info", event, fields);
}

void log::debug(std::string const& event, std::vector<Field> const& fields)
{
    write(LogLevel::Debug, "debug", event, fields);
}

std::string log::jsonEscape(std::string const& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string log::redact(std::string const& text)
{
    std::string out = text;
    std::string const key = "passphrase=";
    auto pos = out.find(key);
    while (pos != std::string::npos)
    {
        auto end = out.find_first_of(" &\n", pos + key.size());
        if (end == std::string::npos)
        {
            end = out.size();
        }
        out.replace(pos + key.size(), end - (pos + key.size()), "***");
        pos = out.find(key, pos + key.size() + 3);
    }
    return out;
}
} // namespace srtgw
