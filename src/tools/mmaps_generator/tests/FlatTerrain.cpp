/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "TerrainBuilder.h"
#include "DetailMeshBounds.h"
#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

// Terrain-only fixtures have no liquid references; no client DBC is needed.
uint32 GetLiquidFlags(uint32) { return 0; }

namespace
{
    void Require(bool condition, char const* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void WriteMap(float height, bool constant = true)
    {
        // Original map v10: 44-byte map header, 16-byte height header and
        // optional float arrays. Empty hole data represents a complete floor.
        uint32 const heightBytes = 16 + (constant ? 0 :
            uint32((MMAP::V9_SIZE_SQ + MMAP::V8_SIZE_SQ) * sizeof(float)));
        std::array<uint32, 11> const header = {
            0x5350414D, 10, 12340, 0, 0, 44, heightBytes, 0, 0, 44 + heightBytes, 512 };
        std::array<uint32, 2> const heightHeader = { 0x5447484D, constant ? 1u : 0u };
        std::ofstream stream("maps/9002120.map", std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<char const*>(header.data()), sizeof(header));
        stream.write(reinterpret_cast<char const*>(heightHeader.data()), sizeof(heightHeader));
        stream.write(reinterpret_cast<char const*>(&height), sizeof(height));
        stream.write(reinterpret_cast<char const*>(&height), sizeof(height));
        if (!constant)
            for (int i = 0; i < MMAP::V9_SIZE_SQ + MMAP::V8_SIZE_SQ; ++i)
                stream.write(reinterpret_cast<char const*>(&height), sizeof(height));
        std::array<uint16, 256> const holes{};
        stream.write(reinterpret_cast<char const*>(holes.data()), sizeof(holes));
        Require(bool(stream), "Failed writing synthetic map fixture");
    }

    void CheckFloor(MMAP::MeshData const& mesh, float height)
    {
        Require(mesh.solidVerts.size() == (MMAP::V9_SIZE_SQ + MMAP::V8_SIZE_SQ) * 3,
            "Expected all V9/V8 terrain vertices");
        Require(mesh.solidTris.size() == MMAP::V8_SIZE_SQ * 4 * 3,
            "Expected all four triangles per terrain square");
        float minX = std::numeric_limits<float>::infinity(), minZ = minX;
        float maxX = -minX, maxZ = -minX;
        for (int i = 0; i < mesh.solidVerts.size(); i += 3)
        {
            float x = mesh.solidVerts[i], y = mesh.solidVerts[i + 1], z = mesh.solidVerts[i + 2];
            Require(std::isfinite(x) && std::isfinite(y) && std::isfinite(z), "Nonfinite mesh vertex");
            Require(y == height, "Constant floor height changed");
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
        }
        Require(std::abs(maxX - minX - MMAP::GRID_SIZE) < 0.01f &&
            std::abs(maxZ - minZ - MMAP::GRID_SIZE) < 0.01f, "Floor lost its two-dimensional extent");
        for (int i = 0; i < mesh.solidTris.size(); ++i)
            Require(mesh.solidTris[i] >= 0 && mesh.solidTris[i] < mesh.solidVerts.size() / 3,
                "Triangle index outside its vertices");
    }

    void CheckDetourBounds(bool includeFlatTerrain)
    {
        // Match the regression: input ground 0, quantized poly height ch,
        // detail height 2*ch. A collapsed header height hides its BV node.
        float const cell = 0.25f; // Exact binary step keeps BV quantization deterministic.
        unsigned short vertices[] = { 0, 1, 0, 0, 1, 4, 4, 1, 4, 4, 1, 0 };
        unsigned short polygons[] = { 0, 1, 2, 3, 0xffff, 0xffff, 0xffff, 0xffff };
        unsigned short flags[] = { 1 };
        unsigned char areas[] = { 1 };
        unsigned int detailMeshes[] = { 0, 4, 0, 2 };
        float detailVertices[] = {
            0, 2 * cell, 0, 0, 2 * cell, 4 * cell,
            4 * cell, 2 * cell, 4 * cell, 4 * cell, 2 * cell, 0 };
        unsigned char detailTriangles[] = { 0, 1, 2, 0, 0, 2, 3, 0 };
        dtNavMeshCreateParams params{};
        params.verts = vertices; params.vertCount = 4;
        params.polys = polygons; params.polyCount = 1; params.nvp = 4;
        params.polyFlags = flags; params.polyAreas = areas;
        params.detailMeshes = detailMeshes;
        params.detailVerts = detailVertices; params.detailVertsCount = 4;
        params.detailTris = detailTriangles; params.detailTriCount = 2;
        params.bmax[0] = params.bmax[2] = 4 * cell;
        params.cs = params.ch = cell;
        params.walkableHeight = 2; params.walkableRadius = 0.3f; params.walkableClimb = 1;
        params.buildBvTree = true;
        MMAP::IncludeDetailMeshHeightBounds(params, includeFlatTerrain);
        Require(params.bmin[1] == 0, "Bounds expansion changed the quantized vertex origin");
        Require(params.bmax[1] == (includeFlatTerrain ? 2 * cell : 0),
            "Header maximum must include detail heights only with opt-in");
        unsigned char* data = nullptr;
        int size = 0;
        Require(dtCreateNavMeshData(&params, &data, &size), "Failed building Detour regression tile");
        dtNavMesh nav;
        if (dtStatusFailed(nav.init(data, size, DT_TILE_FREE_DATA)))
        {
            dtFree(data);
            throw std::runtime_error("Failed initializing Detour regression tile");
        }
        dtNavMeshQuery query;
        Require(dtStatusSucceed(query.init(&nav, 64)), "Failed initializing Detour regression query");
        // The builder allocates 2 BV nodes per polygon, but a one-poly tree
        // uses one node. Keep this interior query away from the zero-filled
        // spare node; it must exercise the real polygon's elevated BV bounds.
        float const point[] = { 3 * cell, 0, 3 * cell };
        float const extent[] = { cell / 4, 2, cell / 4 };
        dtQueryFilter filter;
        filter.setIncludeFlags(1);
        dtPolyRef reference = 0;
        float nearest[3]{};
        Require(dtStatusSucceed(query.findNearestPoly(point, extent, &filter, &reference, nearest)),
            "Detour nearest-poly query failed");
        dtMeshTile const* tile = static_cast<dtNavMesh const&>(nav).getTile(0);
        std::cout << "Detour bounds: opt=" << includeFlatTerrain << " ref=" << reference
                  << " headerY=" << tile->header->bmin[1] << ',' << tile->header->bmax[1]
                  << " queryXZ=" << point[0] - extent[0] << ',' << point[0] + extent[0]
                  << " bvCount=" << tile->header->bvNodeCount;
        for (int i = 0; i < tile->header->bvNodeCount; ++i)
            std::cout << " node" << i << "=" << tile->bvTree[i].bmin[0] << ','
                      << tile->bvTree[i].bmin[1] << ',' << tile->bvTree[i].bmin[2]
                      << ".." << tile->bvTree[i].bmax[0] << ',' << tile->bvTree[i].bmax[1]
                      << ',' << tile->bvTree[i].bmax[2];
        std::cout << '\n';
        Require((reference != 0) == includeFlatTerrain,
            "Actual BV-tree query must expose opt-in terrain and retain the old miss by default");
        if (includeFlatTerrain)
            Require(std::abs(nearest[1] - cell) < 0.0001f,
                "Expanded maximum changed the polygon floor");
    }
}

int main()
{
    namespace fs = std::filesystem;
    fs::path const original = fs::current_path();
    fs::path const fixture = original / ("flat-terrain-fixture-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int result = 0;
    bool createdFixture = false;
    try
    {
        createdFixture = fs::create_directory(fixture);
        Require(createdFixture, "Fixture directory already exists");
        fs::create_directory(fixture / "maps");
        fs::current_path(fixture);
        WriteMap(7.25f);
        MMAP::MeshData oldDefault;
        MMAP::TerrainBuilder(false).loadMap(900, 20, 21, oldDefault);
        Require(oldDefault.solidVerts.size() == 0 && oldDefault.solidTris.size() == 0,
            "Default must retain legacy flat-terrain omission");
        MMAP::MeshData explicitOff;
        MMAP::TerrainBuilder(false, false).loadMap(900, 20, 21, explicitOff);
        Require(explicitOff.solidVerts.size() == 0 && explicitOff.solidTris.size() == 0,
            "Explicit false must retain legacy flat-terrain omission");
        MMAP::MeshData enabled;
        MMAP::TerrainBuilder(false, true).loadMap(900, 20, 21, enabled);
        CheckFloor(enabled, 7.25f);
        WriteMap(std::numeric_limits<float>::quiet_NaN());
        MMAP::MeshData invalid;
        MMAP::TerrainBuilder(false, true).loadMap(900, 20, 21, invalid);
        Require(invalid.solidVerts.size() == 0 && invalid.solidTris.size() == 0, "Nonfinite floor must be refused");
        WriteMap(7.25f, false);
        MMAP::MeshData ordinary;
        MMAP::TerrainBuilder(false).loadMap(900, 20, 21, ordinary);
        CheckFloor(ordinary, 7.25f);
        CheckDetourBounds(false);
        CheckDetourBounds(true);
        std::cout << "Flat-terrain regression passed: default/off/on/nonfinite/ordinary/Detour-BV-bounds\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    fs::current_path(original);
    // Remove only the three exact paths created for this synthetic fixture.
    if (createdFixture)
    {
        fs::remove(fixture / "maps/9002120.map");
        fs::remove(fixture / "maps");
        fs::remove(fixture);
    }
    return result;
}
