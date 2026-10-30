/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTCATALOGLOADING_H
#define AIWORLD_NPCAGENTCATALOGLOADING_H

#include "Define.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

struct NpcAgentCatalogIssue
{
    std::string Filename;
    uint64 Identity = 0;
    std::string Reason;
};
using NpcAgentCatalogIssues = std::vector<NpcAgentCatalogIssue>;

// Optional authoring files may be copied by hand. Keep diagnostics useful
// without logging authored text, filesystem paths or control characters.
namespace NpcAgentCatalogLoading
{
    inline std::string Filename(std::filesystem::path const& path)
    {
        std::string name;
        try { name = path.filename().string(); }
        catch (std::filesystem::filesystem_error const&) { return "<unrepresentable filename>"; }
        if (name.size() > 80) name.resize(80);
        for (char& c : name)
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126 || c == '|') c = '?';
        return name;
    }

    inline std::vector<std::filesystem::path> Files(std::string const& directory,
        std::size_t maxEntries, bool optional, NpcAgentCatalogIssues& issues)
    {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        bool exists = std::filesystem::exists(directory, error);
        if (error || (!exists && !optional))
        {
            issues.push_back({"<directory>", 0, "directory unavailable"});
            return files;
        }
        if (!exists) return files;
        auto entries = std::filesystem::directory_iterator(directory, error);
        if (error)
        {
            issues.push_back({"<directory>", 0, "directory unavailable"});
            return files;
        }
        std::size_t scanned = 0;
        for (auto const end = std::filesystem::directory_iterator(); entries != end; entries.increment(error))
        {
            if (error || ++scanned > maxEntries) break;
            if (entries->path().extension() == ".json") files.push_back(entries->path());
        }
        if (error || scanned > maxEntries)
        {
            issues.push_back({"<directory>", 0, error ? "directory read failed" : "directory entry limit exceeded"});
            files.clear();
            return files;
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    inline std::optional<std::string> Read(std::filesystem::path const& path,
        std::size_t maxBytes, NpcAgentCatalogIssues& issues)
    {
        auto fail = [&](char const* reason) -> std::optional<std::string>
        {
            issues.push_back({Filename(path), 0, reason});
            return std::nullopt;
        };
        std::error_code error;
        auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_regular_file(status)) return fail("file is not a regular file");
        auto size = std::filesystem::file_size(path, error);
        if (error || !size || size > maxBytes) return fail("file size invalid");
        std::ifstream input(path, std::ios::binary);
        if (!input) return fail("file unavailable");
        std::string body(maxBytes + 1, '\0');
        input.read(body.data(), std::streamsize(body.size()));
        std::size_t bytes = std::size_t(input.gcount());
        if (input.bad() || !bytes || bytes > maxBytes) return fail("file read failed");
        body.resize(bytes);
        return body;
    }
}

#endif // AIWORLD_NPCAGENTCATALOGLOADING_H
