/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AIClient.h"
#include "IoContext.h"
#include "Log.h"
#include "MPSCQueue.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <chrono>

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace
{
    // Self-owned (shared_from_this) async session: one instance per request,
    // torn down as soon as it completes, fails, or times out. Mirrors the
    // usual Boost.Beast async HTTP client shape - resolve -> connect ->
    // write -> read, each step chained through its own completion handler,
    // never blocking the thread that runs it.
    class HealthCheckSession : public std::enable_shared_from_this<HealthCheckSession>
    {
        public:
            HealthCheckSession(net::io_context& ioContext, std::string const& host, std::string const& port,
                uint32 timeoutMs, uint64 requestId, MPSCQueue<AIResponse>* responseQueue)
                : _resolver(net::make_strand(ioContext)), _stream(net::make_strand(ioContext)),
                  _host(host), _port(port), _timeoutMs(timeoutMs), _requestId(requestId), _responseQueue(responseQueue),
                  _startTime(std::chrono::steady_clock::now())
            {
            }

            void Run()
            {
                _request.version(11);
                _request.method(http::verb::get);
                _request.target("/health");
                _request.set(http::field::host, _host + ":" + _port);
                _request.set(http::field::user_agent, "TrinityCore-AIWorld");

                TC_LOG_INFO("ai.world", "AI request id={} type=health submitted", _requestId);

                _resolver.async_resolve(_host, _port,
                    beast::bind_front_handler(&HealthCheckSession::OnResolve, shared_from_this()));
            }

        private:
            void OnResolve(beast::error_code ec, tcp::resolver::results_type results)
            {
                if (ec)
                    return Complete(false, 0, ec);

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                _stream.async_connect(results,
                    beast::bind_front_handler(&HealthCheckSession::OnConnect, shared_from_this()));
            }

            void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
            {
                if (ec)
                    return Complete(false, 0, ec);

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                http::async_write(_stream, _request,
                    beast::bind_front_handler(&HealthCheckSession::OnWrite, shared_from_this()));
            }

            void OnWrite(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec);

                http::async_read(_stream, _buffer, _response,
                    beast::bind_front_handler(&HealthCheckSession::OnRead, shared_from_this()));
            }

            void OnRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec);

                Complete(true, _response.result_int(), ec);
            }

            void Complete(bool success, uint32 statusCode, beast::error_code ec)
            {
                beast::error_code ignored;
                _stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

                uint32 latencyMs = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - _startTime).count());

                if (!success && ec == beast::error::timeout)
                    TC_LOG_WARN("ai.world", "AI request id={} timed out after {}ms", _requestId, _timeoutMs);
                else if (!success)
                    TC_LOG_WARN("ai.world", "AI request id={} failed: {}", _requestId, ec.message());
                else
                    TC_LOG_INFO("ai.world", "AI response id={} status={} latency={}ms", _requestId, statusCode, latencyMs);

                AIResponse* response = new AIResponse();
                response->RequestId = _requestId;
                response->Success = success;
                response->StatusCode = statusCode;
                response->LatencyMs = latencyMs;

                _responseQueue->Enqueue(response);
            }

            tcp::resolver _resolver;
            beast::tcp_stream _stream;
            beast::flat_buffer _buffer;
            http::request<http::empty_body> _request;
            http::response<http::string_body> _response;

            std::string _host;
            std::string _port;
            uint32 _timeoutMs;
            uint64 _requestId;
            MPSCQueue<AIResponse>* _responseQueue;
            std::chrono::steady_clock::time_point _startTime;
    };
}

struct AIClient::Impl
{
    Impl(Trinity::Asio::IoContext& ioContext, std::string host, std::string port, uint32 requestTimeoutMs)
        : IoContextRef(ioContext), Host(std::move(host)), Port(std::move(port)), RequestTimeoutMs(requestTimeoutMs)
    {
    }

    Trinity::Asio::IoContext& IoContextRef;
    std::string Host;
    std::string Port;
    uint32 RequestTimeoutMs;
    std::atomic<uint64> NextRequestId { 1 };
    MPSCQueue<AIResponse> ResponseQueue;
};

AIClient::AIClient(Trinity::Asio::IoContext& ioContext, std::string host, std::string port, uint32 requestTimeoutMs)
    : _impl(std::make_unique<Impl>(ioContext, std::move(host), std::move(port), requestTimeoutMs))
{
}

AIClient::~AIClient() = default;

uint64 AIClient::SubmitHealthCheck()
{
    uint64 requestId = _impl->NextRequestId.fetch_add(1, std::memory_order_relaxed);

    net::io_context& rawIoContext = _impl->IoContextRef;
    std::string const& host = _impl->Host;
    std::string const& port = _impl->Port;
    uint32 timeoutMs = _impl->RequestTimeoutMs;
    MPSCQueue<AIResponse>* responseQueue = &_impl->ResponseQueue;

    // Hand the actual work off to the io_context's worker threads. Nothing
    // below this point ever runs on the calling (world update) thread.
    net::post(rawIoContext, [&rawIoContext, host, port, timeoutMs, requestId, responseQueue]()
    {
        std::make_shared<HealthCheckSession>(rawIoContext, host, port, timeoutMs, requestId, responseQueue)->Run();
    });

    return requestId;
}

bool AIClient::TryPopResponse(AIResponse& response)
{
    AIResponse* raw = nullptr;
    if (!_impl->ResponseQueue.Dequeue(raw))
        return false;

    response = *raw;
    delete raw;
    return true;
}
