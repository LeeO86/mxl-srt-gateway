#include "mxl/domain.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace srtgw
{
namespace
{
std::string readText(std::filesystem::path const& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeText(std::filesystem::path const& path, std::string const& text)
{
    std::ofstream out(path, std::ios::trunc);
    if (out)
    {
        out << text;
    }
}
} // namespace

bool ensureOutputDomain(std::string const& path, std::string const& id, std::int64_t historyNs, std::string* error)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        if (error != nullptr)
        {
            *error = "cannot create MXL domain " + path;
        }
        return false;
    }
    auto const def = std::filesystem::path(path) / "domain_def.json";
    if (std::filesystem::exists(def))
    {
        std::string parseError;
        auto const existing = json::fieldString(json::parse(readText(def), &parseError), "id", "");
        if (existing != id)
        {
            if (error != nullptr)
            {
                *error = "domain " + path + " already has id " + existing + "; refusing to overwrite it with " + id;
            }
            log::error("mxl_domain_id_mismatch", {{"path", path}, {"existing", existing}, {"wanted", id}});
            return false;
        }
    }
    else
    {
        // BCP-007-03 requires id, label, description and tags.
        writeText(def, std::string("{\"id\":\"") + id + "\",\"label\":\"mxl-srt-gateway\",\"description\":\"Output domain of mxl-srt-gateway\",\"tags\":{}}\n");
    }
    auto const options = std::filesystem::path(path) / "options.json";
    if (!std::filesystem::exists(options))
    {
        writeText(options, std::string("{\"urn:x-mxl:option:history_duration/v1.0\":") + std::to_string(historyNs) + "}\n");
    }
    return true;
}

bool removeOwnDomain(std::string const& path, std::string const& id)
{
    if (path.empty() || path == "/" || id.empty())
    {
        return false;
    }
    auto const def = std::filesystem::path(path) / "domain_def.json";
    if (std::filesystem::exists(def))
    {
        std::string parseError;
        auto const existing = json::fieldString(json::parse(readText(def), &parseError), "id", "");
        if (existing != id)
        {
            log::error("mxl_domain_cleanup_refused", {{"path", path}, {"existing", existing}, {"wanted", id}});
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec)
    {
        log::error("mxl_domain_cleanup_failed", {{"path", path}, {"error", ec.message()}});
        return false;
    }
    log::info("mxl_domain_removed", {{"path", path}, {"id", id}});
    return true;
}
} // namespace srtgw
