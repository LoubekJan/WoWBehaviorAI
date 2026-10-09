// Standalone geometry check. Link against this checkout's libDetour.a.
// Usage: verify_navmesh DATA_ROOT (expects DATA_ROOT/mmaps/725*.mmap/mmtile).
// This checks navigation data, not physical NPC movement or AI recovery.
#include "MapDefines.h" // src/common/Collision/Maps: actual MmapTileHeader.
#include "DetourNavMeshQuery.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static_assert(sizeof(dtNavMeshParams) == 28, "Unexpected native mmap parameter layout");
static_assert(sizeof(MmapTileHeader) == 20, "Unexpected Trinity mmtile header layout");

namespace
{
void require(bool valid, std::string const& message)
{
    if (!valid)
        throw std::runtime_error(message);
}

std::string quoted(std::string const& value)
{
    std::string result = "\"";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\')
            result += '\\';
        if (c < 32)
            result += '?';
        else
            result += static_cast<char>(c);
    }
    return result + '"';
}

std::vector<unsigned char> readFile(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(input), "Cannot open " + path.string());
    auto length = input.tellg();
    require(length > 0 && length <= 128 * 1024 * 1024, "Invalid file size: " + path.string());
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(input), "Cannot read " + path.string());
    return bytes;
}

bool completeStatus(dtStatus status)
{
    return dtStatusSucceed(status) && !dtStatusFailed(status) &&
        !dtStatusDetail(status, DT_STATUS_DETAIL_MASK);
}

std::uint64_t alignedSize(std::uint64_t bytes)
{
    return (bytes + 3) & ~std::uint64_t(3);
}

// Detour addTile assumes the serialized arrays are complete. Validate their
// aggregate size using the exact structures/64-bit refs of this repository.
void validatePayload(dtMeshHeader const& header, std::size_t size, std::uint32_t maxPolys)
{
    require(header.magic == DT_NAVMESH_MAGIC && header.version == DT_NAVMESH_VERSION,
        "Wrong Detour payload magic/version");
    require(header.polyCount > 0 && static_cast<std::uint32_t>(header.polyCount) <= maxPolys && header.vertCount >= 3 &&
        header.maxLinkCount > 0 && header.detailMeshCount >= 0 && header.detailVertCount >= 0 &&
        header.detailTriCount >= 0 && header.bvNodeCount >= 0 && header.offMeshConCount >= 0 &&
        header.offMeshBase >= 0 && header.offMeshBase <= header.polyCount,
        "Invalid Detour array counts");
    auto section = [](int count, std::size_t itemSize)
    { return alignedSize(static_cast<std::uint64_t>(count) * itemSize); };
    std::uint64_t expected = alignedSize(sizeof(dtMeshHeader)) +
        section(header.vertCount, 3 * sizeof(float)) + section(header.polyCount, sizeof(dtPoly)) +
        section(header.maxLinkCount, sizeof(dtLink)) + section(header.detailMeshCount, sizeof(dtPolyDetail)) +
        section(header.detailVertCount, 3 * sizeof(float)) + section(header.detailTriCount, 4) +
        section(header.bvNodeCount, sizeof(dtBVNode)) + section(header.offMeshConCount, sizeof(dtOffMeshConnection));
    require(expected == size, "Detour payload size does not match its arrays");
}

struct Point
{
    char const* name;
    std::array<float, 3> server; // server X, Y, Z
};

struct Projection
{
    dtPolyRef ref = 0;
    std::array<float, 3> detour{};
};

struct PathResult
{
    int from;
    int to;
    int polygons;
    dtStatus status;
    bool complete;
};
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "Usage: verify_navmesh DATA_ROOT");
        std::uint32_t endian = 1;
        require(*reinterpret_cast<unsigned char*>(&endian) == 1, "Native mmap files require little endian");
        std::filesystem::path root = std::filesystem::path(argv[1]) / "mmaps";
        auto mapBytes = readFile(root / "725.mmap");
        require(mapBytes.size() == sizeof(dtNavMeshParams), "725.mmap must contain exactly 28 bytes");
        dtNavMeshParams params{};
        std::memcpy(&params, mapBytes.data(), sizeof(params));
        // Repo MapBuilder serializes int(1 << DT_POLY_BITS). With its 64-bit
        // Detour refs and 31 poly bits this is the native int bit pattern
        // 0x80000000, not a positive signed int. Preserve the bytes for init.
        std::uint32_t maxPolys = static_cast<std::uint32_t>(params.maxPolys);
        require(params.maxTiles > 0 && maxPolys > 0 && maxPolys <= (std::uint64_t(1) << DT_POLY_BITS) &&
            std::isfinite(params.tileWidth) && params.tileWidth > 0 &&
            std::isfinite(params.tileHeight) && params.tileHeight > 0 &&
            std::isfinite(params.orig[0]) && std::isfinite(params.orig[1]) && std::isfinite(params.orig[2]),
            "Invalid navigation mesh parameters");

        std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)> mesh(dtAllocNavMesh(), dtFreeNavMesh);
        require(static_cast<bool>(mesh), "Cannot allocate navigation mesh");
        require(completeStatus(mesh->init(&params)), "Cannot initialize navigation mesh");
        auto tileBytes = readFile(root / "7253130.mmtile");
        require(tileBytes.size() >= sizeof(MmapTileHeader), "Truncated Trinity tile header");
        MmapTileHeader tileHeader;
        std::memcpy(&tileHeader, tileBytes.data(), sizeof(tileHeader));
        require(tileHeader.mmapMagic == MMAP_MAGIC && tileHeader.mmapVersion == MMAP_VERSION &&
            tileHeader.dtVersion == DT_NAVMESH_VERSION, "Wrong Trinity tile magic/version");
        require(tileHeader.usesLiquids == 0 || tileHeader.usesLiquids == 1, "Invalid usesLiquids byte");
        require(tileHeader.size >= sizeof(dtMeshHeader) &&
            tileHeader.size <= static_cast<unsigned int>(std::numeric_limits<int>::max()) &&
            tileBytes.size() == sizeof(MmapTileHeader) + tileHeader.size, "Invalid Trinity tile payload size");
        dtMeshHeader payloadHeader{};
        std::memcpy(&payloadHeader, tileBytes.data() + sizeof(MmapTileHeader), sizeof(payloadHeader));
        validatePayload(payloadHeader, tileHeader.size, maxPolys);
        std::unique_ptr<unsigned char, decltype(&dtFree)> payload(
            static_cast<unsigned char*>(dtAlloc(tileHeader.size, DT_ALLOC_PERM)), dtFree);
        require(static_cast<bool>(payload), "Cannot allocate tile data");
        std::memcpy(payload.get(), tileBytes.data() + sizeof(MmapTileHeader), tileHeader.size);
        dtTileRef tileRef = 0;
        dtStatus added = mesh->addTile(payload.get(), static_cast<int>(tileHeader.size), DT_TILE_FREE_DATA, 0, &tileRef);
        require(completeStatus(added), "Cannot add tile: status " + std::to_string(added));
        payload.release(); // Mesh owns this dtAlloc buffer only after successful addTile.
        require(tileRef != 0, "Tile has no reference");
        std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)> query(dtAllocNavMeshQuery(), dtFreeNavMeshQuery);
        require(static_cast<bool>(query), "Cannot allocate navigation query");
        require(completeStatus(query->init(mesh.get(), 8192)), "Cannot initialize navigation query");

        std::array<Point, 5> points = {{
            {"home", {266.667f, 800.0f, 0.0f}},
            {"x_plus40", {306.667f, 800.0f, 0.0f}},
            {"x_minus40", {226.667f, 800.0f, 0.0f}},
            {"y_plus40", {266.667f, 840.0f, 0.0f}},
            {"y_minus40", {266.667f, 760.0f, 0.0f}}
        }};
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND);
        filter.setExcludeFlags(0);
        std::array<Projection, 5> projections{};
        float extents[3] = {2.0f, 4.0f, 2.0f}; // Detour horizontal, vertical, horizontal.
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            auto const& position = points[i].server;
            float center[3] = {position[1], position[2], position[0]}; // Same Y,Z,X conversion as PathGenerator.
            dtStatus status = query->findNearestPoly(center, extents, &filter,
                &projections[i].ref, projections[i].detour.data());
            require(completeStatus(status) && projections[i].ref != 0,
                std::string("No ground polygon for ") + points[i].name + ": status " + std::to_string(status));
            auto const& projected = projections[i].detour;
            require(std::isfinite(projected[0]) && std::isfinite(projected[1]) && std::isfinite(projected[2]) &&
                std::hypot(projected[0] - center[0], projected[2] - center[2]) <= 1.0f &&
                std::abs(projected[1] - center[1]) <= extents[1],
                std::string("Ground projection too far from ") + points[i].name);
        }

        std::vector<PathResult> paths;
        int completed = 0;
        for (int point = 1; point < static_cast<int>(points.size()); ++point)
            for (int direction = 0; direction < 2; ++direction)
            {
                int from = direction == 0 ? 0 : point;
                int to = direction == 0 ? point : 0;
                std::array<dtPolyRef, 512> corridor{};
                int count = 0;
                dtStatus status = query->findPath(projections[from].ref, projections[to].ref,
                    projections[from].detour.data(), projections[to].detour.data(), &filter,
                    corridor.data(), &count, static_cast<int>(corridor.size()));
                bool complete = completeStatus(status) && !dtStatusDetail(status, DT_PARTIAL_RESULT) &&
                    count > 0 && count <= static_cast<int>(corridor.size()) &&
                    corridor[0] == projections[from].ref && corridor[count - 1] == projections[to].ref;
                paths.push_back({from, to, count, status, complete});
                completed += complete ? 1 : 0;
            }
        std::cout << "{\"ok\":" << (completed == 8 ? "true" : "false")
                  << ",\"scope\":\"navmesh_only\",\"physical_movement_verified\":false,\"map_id\":725"
                  << ",\"tile\":\"7253130.mmtile\",\"projected_points\":5,\"complete_paths\":" << completed
                  << ",\"required_paths\":8,\"paths\":[";
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            auto const& path = paths[i];
            if (i)
                std::cout << ',';
            std::cout << "{\"from\":" << quoted(points[path.from].name) << ",\"to\":" << quoted(points[path.to].name)
                      << ",\"polygons\":" << path.polygons << ",\"status\":" << path.status
                      << ",\"complete\":" << (path.complete ? "true" : "false") << '}';
        }
        std::cout << "]}\n";
        return completed == 8 ? 0 : 1;
    }
    catch (std::exception const& error)
    {
        std::cout << "{\"ok\":false,\"scope\":\"navmesh_only\",\"error\":" << quoted(error.what()) << "}\n";
        return 1;
    }
}
