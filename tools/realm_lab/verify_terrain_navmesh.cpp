// Standalone check for map 725's single terrain tile. Link against this
// checkout's libDetour.a; its DT_POLYREF64 ABI must match the extracted data.
// Usage: verify_terrain_navmesh DATA_ROOT POINTS_TSV
// POINTS_TSV is UTF-8/ASCII, with the exact tab-separated header:
// kind<TAB>id<TAB>role<TAB>x<TAB>y<TAB>z
// It contains 100 home rows (IDs 900725..900824, predator/prey, actual ground Z)
// and four test rows (IDs plus_x/minus_x/plus_y/minus_y, role "-").
// The return-test origin is home 900725. Coordinates are SERVER X,Y,Z.
// Input Z must come from terrain height data, not from this navmesh projection.
// This checks navigation geometry only, not physical movement or AI recovery.
#include "MapDefines.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static_assert(sizeof(dtNavMeshParams) == 28, "Unexpected native mmap parameter layout");
static_assert(sizeof(MmapTileHeader) == 20, "Unexpected Trinity mmtile header layout");
static_assert(sizeof(dtPolyRef) == 8, "Use this repository's 64-bit Detour refs");

namespace
{
constexpr float MaxProjectionXY = 1.0f;
constexpr float MaxProjectionZ = 2.0f;
constexpr double HuntRadiusXY = 25.0;
constexpr int PathCapacity = 8192;

void require(bool valid, std::string const& message)
{
    if (!valid)
        throw std::runtime_error(message);
}

std::string quoted(std::string const& value)
{
    std::ostringstream result;
    result << '"';
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\')
            result << '\\' << static_cast<char>(c);
        else if (c < 32)
            result << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(c) << std::dec;
        else
            result << static_cast<char>(c);
    }
    result << '"';
    return result.str();
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
    return dtStatusSucceed(status) && !dtStatusFailed(status) && !dtStatusInProgress(status) &&
        !dtStatusDetail(status, DT_STATUS_DETAIL_MASK);
}

std::uint64_t alignedSize(std::uint64_t bytes)
{
    return (bytes + 3) & ~std::uint64_t(3);
}

// addTile assumes complete serialized arrays. Check their aggregate size with
// the exact native structures used by this repository before calling it.
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
    std::string kind;
    std::string id;
    std::string role;
    std::array<double, 3> server{};
};

struct Input
{
    std::vector<Point> points;
    std::map<std::string, std::size_t> homes;
    std::map<std::string, std::size_t> tests;
    std::vector<std::size_t> predators;
    std::vector<std::size_t> prey;
};

std::vector<std::string> fields(std::string const& line)
{
    std::vector<std::string> result;
    std::size_t begin = 0;
    for (;;)
    {
        auto end = line.find('\t', begin);
        result.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos)
            return result;
        begin = end + 1;
    }
}

double coordinate(std::string const& value, std::size_t lineNumber)
{
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    double parsed = 0;
    input >> std::noskipws >> parsed;
    require(input && input.peek() == std::char_traits<char>::eof() && std::isfinite(parsed) &&
        std::abs(parsed) <= 10000.0, "Invalid coordinate on TSV line " + std::to_string(lineNumber));
    return parsed;
}

Input readPoints(std::filesystem::path const& path)
{
    std::ifstream stream(path);
    require(static_cast<bool>(stream), "Cannot open point input " + path.string());
    std::string line;
    require(static_cast<bool>(std::getline(stream, line)), "Empty point input");
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    require(line == "kind\tid\trole\tx\ty\tz", "Expected exact TSV header: kind,id,role,x,y,z (tabs)");
    Input input;
    std::size_t lineNumber = 1;
    while (std::getline(stream, line))
    {
        ++lineNumber;
        require(lineNumber <= 105 && line.size() <= 1024, "Point input exceeds 104 rows or line-size limit");
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto columns = fields(line);
        require(columns.size() == 6, "Expected six TSV columns on line " + std::to_string(lineNumber));
        Point point{columns[0], columns[1], columns[2], {}};
        for (int axis = 0; axis < 3; ++axis)
            point.server[axis] = coordinate(columns[axis + 3], lineNumber);
        require(point.server[0] >= 0.0f && point.server[0] <= 533.334f &&
            point.server[1] >= 533.333f && point.server[1] <= 1066.667f,
            "Point outside map725 tile30,31: " + point.id);
        std::size_t index = input.points.size();
        if (point.kind == "home")
        {
            require(point.role == "predator" || point.role == "prey", "Invalid role for home " + point.id);
            require(input.homes.emplace(point.id, index).second, "Duplicate home ID " + point.id);
            (point.role == "predator" ? input.predators : input.prey).push_back(index);
        }
        else
        {
            require(point.kind == "test" && point.role == "-", "Expected kind home/test and test role '-'");
            require(input.tests.emplace(point.id, index).second, "Duplicate test ID " + point.id);
        }
        input.points.push_back(std::move(point));
    }
    require(!stream.bad(), "Cannot read point input");
    require(input.homes.size() == 100 && input.predators.size() == 20 && input.prey.size() == 80,
        "Expected 100 homes: exactly 20 predators and 80 prey");
    for (int id = 900725; id <= 900824; ++id)
        require(input.homes.count(std::to_string(id)) == 1, "Missing home ID " + std::to_string(id));
    require(input.points[input.homes.at("900725")].role == "predator", "Home900725 must be a predator");
    require(input.tests.size() == 4, "Expected exactly four return-test points");
    auto const& home = input.points[input.homes.at("900725")].server;
    std::array<std::pair<char const*, std::array<double, 2>>, 4> expected{{
        {"plus_x", {home[0] + 40.0, home[1]}}, {"minus_x", {home[0] - 40.0, home[1]}},
        {"plus_y", {home[0], home[1] + 40.0}}, {"minus_y", {home[0], home[1] - 40.0}}
    }};
    for (auto const& test : expected)
    {
        require(input.tests.count(test.first) == 1, "Missing test point " + std::string(test.first));
        auto const& position = input.points[input.tests.at(test.first)].server;
        require(std::abs(position[0] - test.second[0]) <= 0.001f &&
            std::abs(position[1] - test.second[1]) <= 0.001f,
            "Return-test XY must be exactly 40 yards from home900725: " + std::string(test.first));
    }
    return input;
}

struct Projection
{
    dtPolyRef ref = 0;
    dtStatus status = 0;
    std::array<float, 3> detour{};
    bool finite = false;
    double xyDelta = 0;
    double zDelta = 0;
    bool valid = false;
};

struct PathResult
{
    std::string kind;
    std::size_t from = 0;
    std::size_t to = 0;
    bool attempted = false;
    dtStatus status = 0;
    dtStatus straightStatus = 0;
    bool complete = false;
    double endpointDelta = 0;
    std::vector<dtPolyRef> corridor;
    std::vector<std::array<float, 3>> straight;
    std::vector<unsigned char> straightFlags;
    std::vector<dtPolyRef> straightRefs;
};

PathResult findPath(dtNavMeshQuery& query, dtQueryFilter const& filter, std::vector<Projection> const& projections,
    std::string kind, std::size_t from, std::size_t to)
{
    PathResult result;
    result.kind = std::move(kind);
    result.from = from;
    result.to = to;
    auto const& start = projections[from];
    auto const& end = projections[to];
    if (!start.valid || !end.valid)
        return result;
    result.attempted = true;
    std::vector<dtPolyRef> corridor(PathCapacity);
    int count = 0;
    result.status = query.findPath(start.ref, end.ref, start.detour.data(), end.detour.data(), &filter,
        corridor.data(), &count, PathCapacity);
    require(count >= 0 && count <= PathCapacity, "Detour returned an invalid corridor length");
    corridor.resize(static_cast<std::size_t>(count));
    result.corridor = std::move(corridor);
    bool fullCorridor = completeStatus(result.status) && count > 0 &&
        result.corridor.front() == start.ref && result.corridor.back() == end.ref;
    if (!fullCorridor)
        return result;
    std::vector<float> straight(PathCapacity * 3);
    std::vector<unsigned char> flags(PathCapacity);
    std::vector<dtPolyRef> refs(PathCapacity);
    int straightCount = 0;
    result.straightStatus = query.findStraightPath(start.detour.data(), end.detour.data(),
        result.corridor.data(), count, straight.data(), flags.data(), refs.data(), &straightCount, PathCapacity);
    require(straightCount >= 0 && straightCount <= PathCapacity, "Detour returned an invalid straight-path length");
    for (int point = 0; point < straightCount; ++point)
    {
        result.straight.push_back({straight[3 * point], straight[3 * point + 1], straight[3 * point + 2]});
        result.straightFlags.push_back(flags[point]);
        result.straightRefs.push_back(refs[point]);
    }
    if (straightCount > 0)
    {
        auto const& last = result.straight.back();
        result.endpointDelta = std::sqrt(std::pow(last[0] - end.detour[0], 2) +
            std::pow(last[1] - end.detour[1], 2) + std::pow(last[2] - end.detour[2], 2));
        bool finite = std::all_of(result.straight.begin(), result.straight.end(), [](auto const& point)
        { return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]); });
        result.complete = completeStatus(result.straightStatus) && finite && result.endpointDelta <= 0.01 &&
            (result.straightFlags.back() & DT_STRAIGHTPATH_END) != 0;
    }
    return result;
}

void emitPosition(std::array<double, 3> const& server)
{
    std::cout << "{\"x\":" << server[0] << ",\"y\":" << server[1] << ",\"z\":" << server[2] << '}';
}

void emitStatus(dtStatus status)
{
    std::cout << "{\"value\":" << status << ",\"succeeded\":" << (dtStatusSucceed(status) ? "true" : "false")
        << ",\"failed\":" << (dtStatusFailed(status) ? "true" : "false")
        << ",\"detail_bits\":" << (status & DT_STATUS_DETAIL_MASK)
        << ",\"partial\":" << (dtStatusDetail(status, DT_PARTIAL_RESULT) ? "true" : "false")
        << ",\"buffer_too_small\":" << (dtStatusDetail(status, DT_BUFFER_TOO_SMALL) ? "true" : "false")
        << ",\"out_of_nodes\":" << (dtStatusDetail(status, DT_OUT_OF_NODES) ? "true" : "false") << '}';
}
}

int main(int argc, char** argv)
{
    try
    {
        std::cout.imbue(std::locale::classic());
        std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
        require(argc == 3, "Usage: verify_terrain_navmesh DATA_ROOT POINTS_TSV");
        Input input = readPoints(argv[2]);
        std::uint32_t endian = 1;
        require(*reinterpret_cast<unsigned char*>(&endian) == 1, "Native mmap files require little endian");
        std::filesystem::path root = std::filesystem::path(argv[1]) / "mmaps";
        auto mapBytes = readFile(root / "725.mmap");
        require(mapBytes.size() == sizeof(dtNavMeshParams), "725.mmap must contain exactly 28 bytes");
        dtNavMeshParams params{};
        std::memcpy(&params, mapBytes.data(), sizeof(params));
        // MapBuilder's 1<<31 poly count has the native 0x80000000 int pattern.
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
        payload.release();
        require(tileRef != 0, "Tile has no reference");
        std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)> query(dtAllocNavMeshQuery(), dtFreeNavMeshQuery);
        require(static_cast<bool>(query), "Cannot allocate navigation query");
        require(completeStatus(query->init(mesh.get(), PathCapacity)), "Cannot initialize navigation query");
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND);
        filter.setExcludeFlags(0);
        std::vector<Projection> projections(input.points.size());
        float extents[3] = {2.0f, 4.0f, 2.0f};
        int validProjections = 0;
        for (std::size_t point = 0; point < input.points.size(); ++point)
        {
            auto const& server = input.points[point].server;
            float center[3] = {static_cast<float>(server[1]), static_cast<float>(server[2]), static_cast<float>(server[0])};
            // PathGenerator's server Y,Z,X conversion. Keep original double
            // input coordinates for distance checks and deterministic prey selection.
            auto& projection = projections[point];
            projection.status = query->findNearestPoly(center, extents, &filter, &projection.ref, projection.detour.data());
            projection.finite = std::isfinite(projection.detour[0]) &&
                std::isfinite(projection.detour[1]) && std::isfinite(projection.detour[2]);
            if (projection.ref != 0 && projection.finite)
            {
                projection.xyDelta = std::hypot(projection.detour[0] - server[1], projection.detour[2] - server[0]);
                projection.zDelta = projection.detour[1] - server[2];
            }
            projection.valid = completeStatus(projection.status) && projection.ref != 0 && projection.finite &&
                projection.xyDelta <= MaxProjectionXY && std::abs(projection.zDelta) <= MaxProjectionZ;
            validProjections += projection.valid ? 1 : 0;
        }
        std::vector<PathResult> paths;
        auto appendPair = [&](std::string const& kind, std::size_t from, std::size_t to)
        {
            paths.push_back(findPath(*query, filter, projections, kind, from, to));
            paths.push_back(findPath(*query, filter, projections, kind, to, from));
        };
        auto originalHome = input.homes.at("900725");
        for (auto const& test : input.tests)
            appendPair("return", originalHome, test.second);
        struct HuntPair { std::size_t predator; std::size_t prey; double distance; };
        std::vector<HuntPair> huntPairs;
        std::vector<std::string> errors;
        for (auto predator : input.predators)
        {
            std::vector<std::pair<double, std::size_t>> candidates;
            auto const& home = input.points[predator].server;
            for (auto prey : input.prey)
            {
                auto const& target = input.points[prey].server;
                double distance = std::hypot(static_cast<double>(target[0]) - home[0], static_cast<double>(target[1]) - home[1]);
                if (distance <= HuntRadiusXY)
                    candidates.emplace_back(distance, prey);
            }
            std::sort(candidates.begin(), candidates.end(), [&](auto const& lhs, auto const& rhs)
            {
                if (lhs.first != rhs.first)
                    return lhs.first < rhs.first;
                return input.points[lhs.second].id < input.points[rhs.second].id;
            });
            if (candidates.size() < 2)
                errors.push_back("Predator " + input.points[predator].id + " has fewer than two prey homes within 25 yards XY");
            for (std::size_t candidate = 0; candidate < std::min<std::size_t>(2, candidates.size()); ++candidate)
            {
                huntPairs.push_back({predator, candidates[candidate].second, candidates[candidate].first});
                appendPair("hunt", predator, candidates[candidate].second);
            }
        }
        int completePaths = static_cast<int>(std::count_if(paths.begin(), paths.end(), [](auto const& path) { return path.complete; }));
        bool ok = validProjections == 104 && huntPairs.size() == 40 && completePaths == 88 && errors.empty();
        std::cout << "{\"schema_version\":1,\"ok\":" << (ok ? "true" : "false")
            << ",\"scope\":\"navmesh_only\",\"physical_movement_verified\":false,\"map_id\":725"
            << ",\"tile\":\"7253130.mmtile\",\"poly_ref_bits\":64,\"tile_ref\":" << quoted(std::to_string(tileRef))
            << ",\"ground_z_source\":\"input_terrain_heights\",\"input\":" << quoted(argv[2])
            << ",\"projection_limits\":{\"xy\":1,\"z\":2},\"projected_points\":" << input.points.size()
            << ",\"valid_projections\":" << validProjections << ",\"actor_points\":100,\"homes\":100,\"predators\":20,\"prey\":80"
            << ",\"required_return_paths\":8,\"required_hunt_paths\":80,\"required_paths\":88"
            << ",\"attempted_paths\":" << std::count_if(paths.begin(), paths.end(), [](auto const& path) { return path.attempted; })
            << ",\"complete_paths\":" << completePaths << ",\"projections\":[";
        for (std::size_t point = 0; point < input.points.size(); ++point)
        {
            if (point)
                std::cout << ',';
            auto const& source = input.points[point];
            auto const& projection = projections[point];
            std::cout << "{\"kind\":" << quoted(source.kind) << ",\"id\":" << quoted(source.id)
                << ",\"role\":" << quoted(source.role);
            if (source.kind == "home")
                std::cout << ",\"spawn_id\":" << source.id;
            else
                std::cout << ",\"name\":" << quoted(source.id);
            std::cout << ",\"x\":" << source.server[0] << ",\"y\":" << source.server[1]
                << ",\"z\":" << source.server[2] << ",\"input\":";
            emitPosition(source.server);
            std::cout << ",\"status\":";
            emitStatus(projection.status);
            std::cout << ",\"ref\":" << quoted(std::to_string(projection.ref)) << ",\"projected\":";
            if (projection.ref != 0 && projection.finite)
            {
                emitPosition({projection.detour[2], projection.detour[0], projection.detour[1]});
                std::cout << ",\"projected_x\":" << projection.detour[2]
                    << ",\"projected_y\":" << projection.detour[0] << ",\"projected_z\":" << projection.detour[1];
            }
            else
                std::cout << "null,\"projected_x\":null,\"projected_y\":null,\"projected_z\":null";
            std::cout << ",\"xy_delta\":";
            if (projection.ref != 0 && projection.finite)
                std::cout << projection.xyDelta << ",\"z_delta\":" << projection.zDelta;
            else
                std::cout << "null,\"z_delta\":null";
            std::cout << ",\"valid\":" << (projection.valid ? "true" : "false") << '}';
        }
        std::cout << "],\"hunt_pairs\":[";
        for (std::size_t pair = 0; pair < huntPairs.size(); ++pair)
        {
            if (pair)
                std::cout << ',';
            auto const& target = huntPairs[pair];
            std::cout << "{\"predator\":" << quoted(input.points[target.predator].id)
                << ",\"prey\":" << quoted(input.points[target.prey].id) << ",\"home_xy_distance\":" << target.distance << '}';
        }
        std::cout << "],\"paths\":[";
        for (std::size_t pathIndex = 0; pathIndex < paths.size(); ++pathIndex)
        {
            if (pathIndex)
                std::cout << ',';
            auto const& path = paths[pathIndex];
            std::cout << "{\"kind\":" << quoted(path.kind) << ",\"from\":" << quoted(input.points[path.from].id)
                << ",\"to\":" << quoted(input.points[path.to].id);
            if (input.points[path.from].kind == "home")
                std::cout << ",\"from_spawn_id\":" << input.points[path.from].id;
            if (input.points[path.to].kind == "home")
                std::cout << ",\"to_spawn_id\":" << input.points[path.to].id;
            std::cout << ",\"start_ref\":" << quoted(std::to_string(projections[path.from].ref))
                << ",\"end_ref\":" << quoted(std::to_string(projections[path.to].ref))
                << ",\"attempted\":" << (path.attempted ? "true" : "false") << ",\"status\":";
            emitStatus(path.status);
            std::cout << ",\"straight_status\":";
            emitStatus(path.straightStatus);
            std::cout << ",\"polygons\":" << path.corridor.size() << ",\"complete\":" << (path.complete ? "true" : "false")
                << ",\"endpoint_delta\":";
            if (path.straight.empty() || !std::isfinite(path.endpointDelta))
                std::cout << "null";
            else
                std::cout << path.endpointDelta;
            std::cout << ",\"corridor_refs\":[";
            for (std::size_t polygon = 0; polygon < path.corridor.size(); ++polygon)
            {
                if (polygon)
                    std::cout << ',';
                std::cout << quoted(std::to_string(path.corridor[polygon]));
            }
            std::cout << "],\"straight_path\":[";
            for (std::size_t point = 0; point < path.straight.size(); ++point)
            {
                if (point)
                    std::cout << ',';
                auto const& detour = path.straight[point];
                std::cout << "{\"position\":";
                if (std::isfinite(detour[0]) && std::isfinite(detour[1]) && std::isfinite(detour[2]))
                    emitPosition({detour[2], detour[0], detour[1]});
                else
                    std::cout << "null";
                std::cout << ",\"flags\":" << static_cast<unsigned int>(path.straightFlags[point])
                    << ",\"ref\":" << quoted(std::to_string(path.straightRefs[point])) << '}';
            }
            std::cout << "]}";
        }
        std::cout << "],\"errors\":[";
        for (std::size_t error = 0; error < errors.size(); ++error)
        {
            if (error)
                std::cout << ',';
            std::cout << quoted(errors[error]);
        }
        std::cout << "]}\n";
        return ok ? 0 : 1;
    }
    catch (std::exception const& error)
    {
        std::cout << "{\"schema_version\":1,\"ok\":false,\"scope\":\"navmesh_only\",\"physical_movement_verified\":false,\"error\":"
            << quoted(error.what()) << "}\n";
        return 1;
    }
}
