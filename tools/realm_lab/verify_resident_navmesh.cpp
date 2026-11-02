// Read-only multi-tile native Detour validation for authored resident standing points.
// Compile with the matching repository Detour sources and DT_POLYREF64 ABI.
// Usage: verify_resident_navmesh DATA_ROOT POINTS_TSV ROUTES_TSV
// POINTS_TSV header: kind,id,role,x,y,z (tabs), in server coordinates.
// ROUTES_TSV header: from,to (tabs). No live world or physical movement claim.
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
        require(argc == 4, "Usage: verify_resident_navmesh DATA_ROOT POINTS_TSV ROUTES_TSV");
        Input input;
        std::map<std::string, std::size_t> indices;
        std::ifstream points(argv[2]);
        std::string line;
        require(bool(std::getline(points, line)) && line == "kind\tid\trole\tx\ty\tz", "Invalid point header");
        while (std::getline(points, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto parts = fields(line);
            require(parts.size() == 6, "Invalid point row");
            Point p{parts[0], parts[1], parts[2], {}};
            for (int axis=0; axis<3; ++axis) p.server[axis] = coordinate(parts[axis+3], input.points.size()+2);
            require(p.server[0] >= -533.334 && p.server[0] <= 533.334 && p.server[1] >= 0 && p.server[1] <= 1066.667, "Point outside map725 authored scope");
            require(indices.emplace(p.id, input.points.size()).second, "Duplicate point");
            input.points.push_back(p);
        }
        require(input.points.size() > 0 && input.points.size() <= 100, "Unexpected point count");
        std::filesystem::path root = std::filesystem::path(argv[1]) / "mmaps";
        auto mapBytes = readFile(root / "725.mmap");
        require(mapBytes.size() == sizeof(dtNavMeshParams), "Unexpected parameter ABI");
        dtNavMeshParams params{};
        std::memcpy(&params, mapBytes.data(), sizeof(params));
        std::uint32_t maxPolys = static_cast<std::uint32_t>(params.maxPolys);
        require(params.maxTiles > 0 && maxPolys > 0 && maxPolys <= (std::uint64_t(1) << DT_POLY_BITS), "Invalid mesh limits");
        std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)> mesh(dtAllocNavMesh(), dtFreeNavMesh);
        require(bool(mesh) && completeStatus(mesh->init(&params)), "Cannot initialize mesh");
        int tiles = 0;
        for (char const* name : {"7253130.mmtile", "7253131.mmtile", "7253230.mmtile", "7253231.mmtile"})
        {
            auto bytes = readFile(root / name);
            require(bytes.size() >= sizeof(MmapTileHeader), "Truncated tile");
            MmapTileHeader header;
            std::memcpy(&header, bytes.data(), sizeof(header));
            require(header.mmapMagic == MMAP_MAGIC && header.mmapVersion == MMAP_VERSION && header.dtVersion == DT_NAVMESH_VERSION, "Tile ABI mismatch");
            require(header.usesLiquids == 0 || header.usesLiquids == 1, "Invalid liquid byte");
            require(header.size >= sizeof(dtMeshHeader) && header.size <= unsigned(std::numeric_limits<int>::max()) && bytes.size() == sizeof(header)+header.size, "Invalid payload size");
            dtMeshHeader payloadHeader{};
            std::memcpy(&payloadHeader, bytes.data()+sizeof(header), sizeof(payloadHeader));
            validatePayload(payloadHeader, header.size, maxPolys);
            std::unique_ptr<unsigned char, decltype(&dtFree)> payload(static_cast<unsigned char*>(dtAlloc(header.size, DT_ALLOC_PERM)), dtFree);
            require(bool(payload), "Cannot allocate payload");
            std::memcpy(payload.get(), bytes.data()+sizeof(header), header.size);
            dtTileRef reference=0;
            require(completeStatus(mesh->addTile(payload.get(), int(header.size), DT_TILE_FREE_DATA, 0, &reference)) && reference, "Cannot add tile");
            payload.release(); ++tiles;
        }
        std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)> query(dtAllocNavMeshQuery(), dtFreeNavMeshQuery);
        require(bool(query) && completeStatus(query->init(mesh.get(), PathCapacity)), "Cannot initialize query");
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND);
        filter.setExcludeFlags(0xfffe);
        std::vector<Projection> projections(input.points.size());
        float extents[3] = {2, 4, 2};
        std::size_t valid=0;
        for (std::size_t i=0; i<input.points.size(); ++i)
        {
            auto const& server=input.points[i].server;
            float center[3] = {float(server[1]), float(server[2]), float(server[0])};
            auto& p=projections[i];
            p.status=query->findNearestPoly(center,extents,&filter,&p.ref,p.detour.data());
            p.finite=std::all_of(p.detour.begin(), p.detour.end(), [](float x){return std::isfinite(x);});
            if (p.ref && p.finite) {p.xyDelta=std::hypot(p.detour[0]-server[1],p.detour[2]-server[0]);p.zDelta=p.detour[1]-server[2];}
            p.valid=completeStatus(p.status) && p.ref && p.finite && p.xyDelta<=1 && std::abs(p.zDelta)<=2;
            valid += p.valid?1:0;
        }
        std::ifstream routeFile(argv[3]);
        require(bool(std::getline(routeFile,line)) && line=="from\tto", "Invalid routes header");
        std::vector<PathResult> routes;
        while (std::getline(routeFile,line))
        {
            if (!line.empty() && line.back()=='\r') line.pop_back();
            auto parts=fields(line);
            require(parts.size()==2 && indices.count(parts[0]) && indices.count(parts[1]), "Invalid route row");
            routes.push_back(findPath(*query,filter,projections,"resident",indices.at(parts[0]),indices.at(parts[1])));
        }
        std::size_t completed=0;
        for (auto& path:routes)
        {
            for (dtPolyRef ref:path.corridor)
            {
                unsigned short flags=0;
                require(completeStatus(mesh->getPolyFlags(ref,&flags)), "Cannot read corridor polygon flags");
                path.complete = path.complete && flags==NAV_GROUND;
            }
            completed+=path.complete?1:0;
        }
        bool ok=valid==input.points.size() && completed==routes.size() && !routes.empty();
        std::cout << "{\"ok\":" << (ok?"true":"false") << ",\"map_id\":725,\"poly_ref_bits\":64,\"tiles\":" << tiles
          << ",\"include_flags\":1,\"exclude_flags\":65534,\"projection_limits\":{\"xy\":1,\"z\":2},\"valid_projections\":" << valid
          << ",\"required_paths\":" << routes.size() << ",\"complete_paths\":" << completed << ",\"projections\":[";
        for(std::size_t i=0;i<input.points.size();++i)
        {
            if(i)std::cout<<',';auto const& p=projections[i];
            unsigned short polygonFlags=0; unsigned char polygonArea=0;
            require(p.ref && completeStatus(mesh->getPolyFlags(p.ref,&polygonFlags)) &&
                completeStatus(mesh->getPolyArea(p.ref,&polygonArea)), "Cannot inspect projected polygon");
            std::cout<<"{\"id\":"<<quoted(input.points[i].id)<<",\"input\":";emitPosition(input.points[i].server);
            std::cout<<",\"status\":";emitStatus(p.status);
            std::cout<<",\"ref\":"<<quoted(std::to_string(p.ref))<<",\"valid\":"<<(p.valid?"true":"false")<<",\"projected\":";
            if(p.ref&&p.finite)emitPosition({p.detour[2],p.detour[0],p.detour[1]});else std::cout<<"null";
            std::cout<<",\"xy_delta\":"<<p.xyDelta<<",\"z_delta\":"<<p.zDelta
                <<",\"flags\":"<<polygonFlags<<",\"area\":"<<unsigned(polygonArea)<<'}';
        }
        std::cout<<"],\"paths\":[";
        for(std::size_t i=0;i<routes.size();++i)
        {
            if(i)std::cout<<',';auto const& p=routes[i];
            std::cout<<"{\"from\":"<<quoted(input.points[p.from].id)<<",\"to\":"<<quoted(input.points[p.to].id)<<",\"complete\":"<<(p.complete?"true":"false")
              <<",\"attempted\":"<<(p.attempted?"true":"false")<<",\"polygons\":"<<p.corridor.size()<<",\"status\":";emitStatus(p.status);
            std::cout<<",\"straight_status\":";emitStatus(p.straightStatus);
            std::cout<<",\"endpoint_delta\":"<<p.endpointDelta<<",\"all_corridor_flags_ground_only\":"<<(p.complete?"true":"false")<<",\"straight_path\":[";
            for(std::size_t j=0;j<p.straight.size();++j){if(j)std::cout<<',';auto const& v=p.straight[j];emitPosition({v[2],v[0],v[1]});}
            std::cout<<"]}";
        }
        std::cout<<"]}\n";return ok?0:1;
    }
    catch(std::exception const& error){std::cout<<"{\"ok\":false,\"error\":"<<quoted(error.what())<<"}\n";return 1;}
}
