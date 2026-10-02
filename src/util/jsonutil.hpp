#pragma once

#include <picojson/picojson.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace srtgw::json
{
inline picojson::value parse(std::string const& text, std::string* err)
{
    picojson::value value;
    auto const message = picojson::parse(value, text);
    if (err != nullptr)
    {
        *err = message;
    }
    return value;
}

inline bool isObject(picojson::value const& value)
{
    return value.is<picojson::object>();
}

inline picojson::object const* objectPtr(picojson::value const& value)
{
    if (!value.is<picojson::object>())
    {
        return nullptr;
    }
    return &value.get<picojson::object>();
}

inline std::optional<picojson::value> field(picojson::value const& value, std::string const& key)
{
    auto const* obj = objectPtr(value);
    if (obj == nullptr)
    {
        return std::nullopt;
    }
    auto const it = obj->find(key);
    if (it == obj->end() || it->second.is<picojson::null>())
    {
        return std::nullopt;
    }
    return it->second;
}

inline bool has(picojson::value const& value, std::string const& key)
{
    auto const* obj = objectPtr(value);
    return obj != nullptr && obj->find(key) != obj->end();
}

inline std::string asString(picojson::value const& value, std::string const& fallback = {})
{
    if (value.is<std::string>())
    {
        return value.get<std::string>();
    }
    if (value.is<bool>())
    {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is<double>())
    {
        auto const n = value.get<double>();
        if (n == static_cast<double>(static_cast<long long>(n)))
        {
            return std::to_string(static_cast<long long>(n));
        }
        return std::to_string(n);
    }
    return fallback;
}

inline std::string fieldString(picojson::value const& value, std::string const& key, std::string const& fallback = {})
{
    auto const item = field(value, key);
    if (!item)
    {
        return fallback;
    }
    return asString(*item, fallback);
}

inline int fieldInt(picojson::value const& value, std::string const& key, int fallback)
{
    auto const item = field(value, key);
    if (!item)
    {
        return fallback;
    }
    if (item->is<double>())
    {
        return static_cast<int>(item->get<double>());
    }
    if (item->is<std::string>())
    {
        try
        {
            return std::stoi(item->get<std::string>());
        }
        catch (...)
        {
            return fallback;
        }
    }
    return fallback;
}

inline std::int64_t fieldInt64(picojson::value const& value, std::string const& key, std::int64_t fallback)
{
    auto const item = field(value, key);
    if (!item || !item->is<double>())
    {
        return fallback;
    }
    return static_cast<std::int64_t>(item->get<double>());
}

inline double fieldDouble(picojson::value const& value, std::string const& key, double fallback)
{
    auto const item = field(value, key);
    if (!item || !item->is<double>())
    {
        return fallback;
    }
    return item->get<double>();
}

inline bool fieldBool(picojson::value const& value, std::string const& key, bool fallback)
{
    auto const item = field(value, key);
    if (!item)
    {
        return fallback;
    }
    if (item->is<bool>())
    {
        return item->get<bool>();
    }
    if (item->is<std::string>())
    {
        auto const text = item->get<std::string>();
        if (text == "true" || text == "1")
        {
            return true;
        }
        if (text == "false" || text == "0")
        {
            return false;
        }
    }
    return fallback;
}

inline std::vector<std::string> fieldStringList(picojson::value const& value, std::string const& key)
{
    std::vector<std::string> out;
    auto const item = field(value, key);
    if (!item || !item->is<picojson::array>())
    {
        return out;
    }
    for (auto const& entry : item->get<picojson::array>())
    {
        out.push_back(asString(entry));
    }
    return out;
}

inline std::vector<int> fieldIntList(picojson::value const& value, std::string const& key)
{
    std::vector<int> out;
    auto const item = field(value, key);
    if (!item || !item->is<picojson::array>())
    {
        return out;
    }
    for (auto const& entry : item->get<picojson::array>())
    {
        if (entry.is<double>())
        {
            out.push_back(static_cast<int>(entry.get<double>()));
        }
    }
    return out;
}

inline picojson::value stringList(std::vector<std::string> const& values)
{
    picojson::array arr;
    for (auto const& value : values)
    {
        arr.emplace_back(value);
    }
    return picojson::value(arr);
}

inline picojson::value intList(std::vector<int> const& values)
{
    picojson::array arr;
    for (int value : values)
    {
        arr.emplace_back(static_cast<double>(value));
    }
    return picojson::value(arr);
}

inline std::string stringify(picojson::value const& value)
{
    return value.serialize();
}
} // namespace srtgw::json
