/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TelemetryExporter.h"
#include "TelemetryJsonCodec.h"
#include "IoContext.h"
#include "Log.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <utility>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

namespace
{
    class PostSession : public std::enable_shared_from_this<PostSession>
    {
    public:
        PostSession(net::io_context& io, std::string host, std::string port, std::string token,
            std::string body, std::shared_ptr<std::atomic<bool>> inFlight,
            std::shared_ptr<TelemetryMemoryRequestSlot> memoryRequest)
            : _resolver(net::make_strand(io)), _stream(_resolver.get_executor()),
              _resolveTimer(_resolver.get_executor()), _host(std::move(host)), _port(std::move(port)),
              _token(std::move(token)), _body(std::move(body)), _inFlight(std::move(inFlight)),
              _memoryRequest(std::move(memoryRequest)) { }

        void Run()
        {
            _request.version(11);
            _request.method(http::verb::post);
            _request.target("/internal/telemetry");
            _request.set(http::field::host, _host + ":" + _port);
            _request.set(http::field::content_type, "application/json");
            _request.set(http::field::authorization, "Bearer " + _token);
            _request.body() = std::move(_body);
            _request.prepare_payload();
            _resolveTimer.expires_after(std::chrono::seconds(3));
            _resolveTimer.async_wait(beast::bind_front_handler(&PostSession::OnResolveTimeout, shared_from_this()));
            _resolver.async_resolve(_host, _port,
                beast::bind_front_handler(&PostSession::OnResolve, shared_from_this()));
        }

    private:
        void OnResolveTimeout(beast::error_code ec)
        {
            if (ec == net::error::operation_aborted)
                return;
            _resolver.cancel();
            Finish(beast::error::timeout);
        }

        void OnResolve(beast::error_code ec, tcp::resolver::results_type results)
        {
            _resolveTimer.cancel();
            if (_finished || ec)
                return Finish(ec);
            _stream.expires_after(std::chrono::seconds(3));
            _stream.async_connect(results, beast::bind_front_handler(&PostSession::OnConnect, shared_from_this()));
        }

        void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
        {
            if (ec) return Finish(ec);
            _stream.expires_after(std::chrono::seconds(3));
            http::async_write(_stream, _request, beast::bind_front_handler(&PostSession::OnWrite, shared_from_this()));
        }

        void OnWrite(beast::error_code ec, std::size_t)
        {
            if (ec) return Finish(ec);
            _stream.expires_after(std::chrono::seconds(3));
            _response.body_limit(16384);
            http::async_read(_stream, _buffer, _response,
                beast::bind_front_handler(&PostSession::OnRead, shared_from_this()));
        }

        void OnRead(beast::error_code ec, std::size_t)
        {
            if (!ec && _response.get().result() == http::status::ok)
            {
                auto value = _response.get()["X-Observer-Memory-Request"];
                _memoryRequest->Store(ParseMemoryPageRequest(std::string_view(value.data(), value.size())));
            }
            if (!ec && _response.get().result() != http::status::ok)
                TC_LOG_DEBUG("ai.world", "Telemetry POST returned HTTP {}", _response.get().result_int());
            Finish(ec);
        }

        void Finish(beast::error_code ec)
        {
            if (_finished)
                return;
            _finished = true;
            _resolveTimer.cancel();
            if (ec)
                TC_LOG_DEBUG("ai.world", "Telemetry POST failed: {}", ec.message());
            beast::error_code ignored;
            _stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
            _inFlight->store(false, std::memory_order_release);
        }

        tcp::resolver _resolver;
        beast::tcp_stream _stream;
        net::steady_timer _resolveTimer;
        std::string _host;
        std::string _port;
        std::string _token;
        std::string _body;
        std::shared_ptr<std::atomic<bool>> _inFlight;
        std::shared_ptr<TelemetryMemoryRequestSlot> _memoryRequest;
        beast::flat_buffer _buffer;
        http::request<http::string_body> _request;
        http::response_parser<http::string_body> _response;
        bool _finished = false;
    };
}

TelemetryExporter::TelemetryExporter(Trinity::Asio::IoContext& ioContext, std::string host,
    std::string port, std::string token)
    : _ioContext(ioContext), _host(std::move(host)), _port(std::move(port)), _token(std::move(token)) { }

void TelemetryExporter::Submit(std::vector<AgentTelemetrySnapshot> snapshots, uint64 capturedAtMs,
    std::optional<MemoryPageTelemetry> memoryPage)
{
    if (_inFlight->exchange(true, std::memory_order_acq_rel))
        return;

    net::post(static_cast<net::io_context&>(_ioContext), [io = &_ioContext, host = _host, port = _port, token = _token,
        snapshots = std::move(snapshots), capturedAtMs, inFlight = _inFlight,
        memoryPage = std::move(memoryPage), memoryRequest = _memoryRequest]() mutable
    {
        std::string body = SerializeAgentTelemetry(snapshots, capturedAtMs, memoryPage);
        std::make_shared<PostSession>(static_cast<net::io_context&>(*io), std::move(host),
            std::move(port), std::move(token), std::move(body), std::move(inFlight), std::move(memoryRequest))->Run();
    });
}
