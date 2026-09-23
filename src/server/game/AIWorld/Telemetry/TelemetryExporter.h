/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef AIWORLD_TELEMETRYEXPORTER_H
#define AIWORLD_TELEMETRYEXPORTER_H

#include "AgentTelemetrySnapshot.h"
#include "MemoryTelemetry.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Trinity::Asio { class IoContext; }

// Shared only by the exporter session and world-thread capture. Contains no
// engine state and remains valid if an HTTP completion arrives during shutdown.
class TelemetryMemoryRequestSlot
{
public:
    void Store(std::optional<MemoryPageRequest> request)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _request = std::move(request);
    }
    std::optional<MemoryPageRequest> Take()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto request = std::move(_request);
        _request.reset();
        return request;
    }
private:
    std::mutex _mutex;
    std::optional<MemoryPageRequest> _request;
};

// One bounded asynchronous HTTP request at a time. Submit() is called only
// on the world thread and never performs DNS, socket I/O, or serialization.
class TelemetryExporter
{
public:
    TelemetryExporter(Trinity::Asio::IoContext& ioContext, std::string host, std::string port, std::string token);
    bool Busy() const { return _inFlight->load(std::memory_order_acquire); }
    std::optional<MemoryPageRequest> TakeMemoryRequest() { return _memoryRequest->Take(); }
    void Submit(std::vector<AgentTelemetrySnapshot> snapshots, uint64 capturedAtMs,
        std::optional<MemoryPageTelemetry> memoryPage = std::nullopt);

private:
    Trinity::Asio::IoContext& _ioContext;
    std::string _host;
    std::string _port;
    std::string _token;
    // Shared with a self-owned session so a late completion cannot access a
    // destroyed exporter during shutdown.
    std::shared_ptr<std::atomic<bool>> _inFlight = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<TelemetryMemoryRequestSlot> _memoryRequest = std::make_shared<TelemetryMemoryRequestSlot>();
};

#endif // AIWORLD_TELEMETRYEXPORTER_H
