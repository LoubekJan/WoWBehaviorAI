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
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <sstream>

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
                uint32 timeoutMs, uint64 requestId, MPSCQueue<AIResponse>* responseQueue, std::atomic<bool>* inFlightFlag)
                : _resolver(net::make_strand(ioContext)), _stream(net::make_strand(ioContext)),
                  _resolveTimer(_resolver.get_executor()),
                  _host(host), _port(port), _timeoutMs(timeoutMs), _requestId(requestId),
                  _responseQueue(responseQueue), _inFlightFlag(inFlightFlag),
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

                // tcp_stream's expires_after() only covers operations done
                // through the stream itself (connect/write/read below) - the
                // resolver is a separate object with no built-in deadline, so
                // a stuck/unresponsive DNS server needs its own timer.
                _resolveTimer.expires_after(std::chrono::milliseconds(_timeoutMs));
                _resolveTimer.async_wait(
                    beast::bind_front_handler(&HealthCheckSession::OnResolveTimeout, shared_from_this()));

                _resolver.async_resolve(_host, _port,
                    beast::bind_front_handler(&HealthCheckSession::OnResolve, shared_from_this()));
            }

        private:
            void OnResolveTimeout(beast::error_code ec)
            {
                if (ec == net::error::operation_aborted)
                    return; // resolve finished first and cancelled this timer

                _resolver.cancel();
                Complete(false, 0, beast::error::timeout);
            }

            void OnResolve(beast::error_code ec, tcp::resolver::results_type results)
            {
                _resolveTimer.cancel();

                if (ec)
                    return Complete(false, 0, ec);

                if (_completed.load(std::memory_order_acquire))
                    return; // resolve timeout already completed this request

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

                uint32 statusCode = _response.result_int();
                bool success = statusCode >= 200 && statusCode < 300;
                Complete(success, statusCode, ec);
            }

            // Guarded by _completed so exactly one AIResponse is ever queued
            // per request, even when the resolve-timeout timer and a
            // just-completed resolve race each other (they run on the same
            // strand, but the timer can already be queued to fire).
            void Complete(bool success, uint32 statusCode, beast::error_code ec)
            {
                if (_completed.exchange(true, std::memory_order_acq_rel))
                    return;

                beast::error_code ignored;
                _stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
                _inFlightFlag->store(false, std::memory_order_release);

                uint32 latencyMs = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - _startTime).count());

                if (ec == beast::error::timeout)
                    TC_LOG_WARN("ai.world", "AI request id={} timed out after {}ms", _requestId, _timeoutMs);
                else if (ec)
                    TC_LOG_WARN("ai.world", "AI request id={} failed: {}", _requestId, ec.message());
                else if (!success)
                    TC_LOG_WARN("ai.world", "AI request id={} completed with non-2xx status={} latency={}ms", _requestId, statusCode, latencyMs);
                else
                    TC_LOG_INFO("ai.world", "AI response id={} status={} latency={}ms", _requestId, statusCode, latencyMs);

                AIResponse* response = new AIResponse();
                response->RequestId = _requestId;
                response->Type = AIRequestType::Health;
                response->Success = success;
                response->StatusCode = statusCode;
                response->LatencyMs = latencyMs;

                _responseQueue->Enqueue(response);
            }

            tcp::resolver _resolver;
            beast::tcp_stream _stream;
            net::steady_timer _resolveTimer;
            beast::flat_buffer _buffer;
            http::request<http::empty_body> _request;
            http::response<http::string_body> _response;

            std::string _host;
            std::string _port;
            uint32 _timeoutMs;
            uint64 _requestId;
            MPSCQueue<AIResponse>* _responseQueue;
            std::atomic<bool>* _inFlightFlag;
            std::chrono::steady_clock::time_point _startTime;
            std::atomic<bool> _completed { false };
    };

    // Builds the fixed-shape /decision request body. Deliberately not a
    // general JSON serializer - just this one flat schema, all
    // numeric/bool fields with nothing that needs escaping.
    std::string BuildDecisionRequestBody(AIRequest const& request)
    {
        std::ostringstream body;
        body << "{\"request_id\":" << request.RequestId
             << ",\"snapshot_sequence\":" << request.SnapshotSequence
             << ",\"spawn_id\":" << request.SpawnId
             << ",\"entry\":" << request.Entry
             << ",\"health\":" << request.Health
             << ",\"max_health\":" << request.MaxHealth
             << ",\"alive\":" << (request.Alive ? "true" : "false")
             << ",\"in_combat\":" << (request.InCombat ? "true" : "false")
             << ",\"map_id\":" << request.MapId
             << ",\"position\":{\"x\":" << request.X
             << ",\"y\":" << request.Y
             << ",\"z\":" << request.Z
             << "}}";
        return body.str();
    }

    // Defensive, minimal extraction for the fixed
    // {"request_id":N,"snapshot_sequence":N,"action":"STR"} response shape -
    // not a general JSON parser. Tolerant of key order, whitespace, and
    // unknown extra keys; never throws; returns false (never partial output)
    // if a required field is missing or malformed. Callers must still check
    // requestId/snapshotSequence against what was actually sent - parsing
    // successfully only means the body was well-formed, not that it answers
    // this request.
    bool ParseDecisionResponseBody(std::string const& body, uint64& requestId, uint64& snapshotSequence, std::string& action)
    {
        auto findUintField = [&body](std::string const& key, uint64& out) -> bool
        {
            std::string needle = "\"" + key + "\"";
            size_t keyPos = body.find(needle);
            if (keyPos == std::string::npos)
                return false;

            size_t colonPos = body.find(':', keyPos + needle.size());
            if (colonPos == std::string::npos)
                return false;

            size_t valueStart = colonPos + 1;
            while (valueStart < body.size() && std::isspace(static_cast<unsigned char>(body[valueStart])))
                ++valueStart;

            size_t valueEnd = valueStart;
            while (valueEnd < body.size() && std::isdigit(static_cast<unsigned char>(body[valueEnd])))
                ++valueEnd;

            if (valueEnd == valueStart)
                return false;

            try
            {
                out = std::stoull(body.substr(valueStart, valueEnd - valueStart));
            }
            catch (std::exception const&)
            {
                return false;
            }
            return true;
        };

        auto findStringField = [&body](std::string const& key, std::string& out) -> bool
        {
            std::string needle = "\"" + key + "\"";
            size_t keyPos = body.find(needle);
            if (keyPos == std::string::npos)
                return false;

            size_t colonPos = body.find(':', keyPos + needle.size());
            if (colonPos == std::string::npos)
                return false;

            size_t quoteStart = body.find('"', colonPos + 1);
            if (quoteStart == std::string::npos)
                return false;

            size_t quoteEnd = body.find('"', quoteStart + 1);
            if (quoteEnd == std::string::npos)
                return false;

            out = body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            return true;
        };

        return findUintField("request_id", requestId) &&
               findUintField("snapshot_sequence", snapshotSequence) &&
               findStringField("action", action);
    }

    // Same resolve/connect/write/read/timeout shape as HealthCheckSession
    // (see its comments for why each piece is there), POSTing a JSON body
    // to /decision and parsing a JSON body back instead of GETting /health.
    class DecisionSession : public std::enable_shared_from_this<DecisionSession>
    {
        public:
            DecisionSession(net::io_context& ioContext, std::string const& host, std::string const& port,
                uint32 timeoutMs, AIRequest const& request, MPSCQueue<AIResponse>* responseQueue, std::atomic<bool>* inFlightFlag)
                : _resolver(net::make_strand(ioContext)), _stream(net::make_strand(ioContext)),
                  _resolveTimer(_resolver.get_executor()),
                  _host(host), _port(port), _timeoutMs(timeoutMs), _request(request),
                  _responseQueue(responseQueue), _inFlightFlag(inFlightFlag),
                  _startTime(std::chrono::steady_clock::now())
            {
            }

            void Run()
            {
                _httpRequest.version(11);
                _httpRequest.method(http::verb::post);
                _httpRequest.target("/decision");
                _httpRequest.set(http::field::host, _host + ":" + _port);
                _httpRequest.set(http::field::user_agent, "TrinityCore-AIWorld");
                _httpRequest.set(http::field::content_type, "application/json");
                _httpRequest.body() = BuildDecisionRequestBody(_request);
                _httpRequest.prepare_payload();

                TC_LOG_INFO("ai.world", "AI decision request id={} snapshot={} spawn={} submitted",
                    _request.RequestId, _request.SnapshotSequence, _request.SpawnId);

                _resolveTimer.expires_after(std::chrono::milliseconds(_timeoutMs));
                _resolveTimer.async_wait(
                    beast::bind_front_handler(&DecisionSession::OnResolveTimeout, shared_from_this()));

                _resolver.async_resolve(_host, _port,
                    beast::bind_front_handler(&DecisionSession::OnResolve, shared_from_this()));
            }

        private:
            void OnResolveTimeout(beast::error_code ec)
            {
                if (ec == net::error::operation_aborted)
                    return; // resolve finished first and cancelled this timer

                _resolver.cancel();
                Complete(false, 0, beast::error::timeout, std::string());
            }

            void OnResolve(beast::error_code ec, tcp::resolver::results_type results)
            {
                _resolveTimer.cancel();

                if (ec)
                    return Complete(false, 0, ec, std::string());

                if (_completed.load(std::memory_order_acquire))
                    return; // resolve timeout already completed this request

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                _stream.async_connect(results,
                    beast::bind_front_handler(&DecisionSession::OnConnect, shared_from_this()));
            }

            void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
            {
                if (ec)
                    return Complete(false, 0, ec, std::string());

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                http::async_write(_stream, _httpRequest,
                    beast::bind_front_handler(&DecisionSession::OnWrite, shared_from_this()));
            }

            void OnWrite(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec, std::string());

                http::async_read(_stream, _buffer, _httpResponse,
                    beast::bind_front_handler(&DecisionSession::OnRead, shared_from_this()));
            }

            void OnRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec, std::string());

                uint32 statusCode = _httpResponse.result_int();
                bool success = statusCode >= 200 && statusCode < 300;
                std::string action;
                if (success)
                {
                    uint64 responseRequestId = 0;
                    uint64 responseSnapshotSequence = 0;
                    if (!ParseDecisionResponseBody(_httpResponse.body(), responseRequestId, responseSnapshotSequence, action))
                    {
                        TC_LOG_WARN("ai.world", "AI decision response id={} could not be parsed, treating as failed", _request.RequestId);
                        success = false;
                    }
                    // A well-formed body isn't enough on its own - verify it
                    // actually answers the request we sent, not just any
                    // request/response the queue happened to pull off HTTP
                    // pipelining, a proxy, or a future protocol bug.
                    else if (responseRequestId != _request.RequestId || responseSnapshotSequence != _request.SnapshotSequence)
                    {
                        TC_LOG_WARN("ai.world", "AI decision request id={} snapshot={} got mismatched response id={} snapshot={}, discarding",
                            _request.RequestId, _request.SnapshotSequence, responseRequestId, responseSnapshotSequence);
                        success = false;
                    }
                }
                Complete(success, statusCode, ec, action);
            }

            // Guarded by _completed - see HealthCheckSession::Complete().
            void Complete(bool success, uint32 statusCode, beast::error_code ec, std::string const& action)
            {
                if (_completed.exchange(true, std::memory_order_acq_rel))
                    return;

                beast::error_code ignored;
                _stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
                _inFlightFlag->store(false, std::memory_order_release);

                uint32 latencyMs = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - _startTime).count());

                if (ec == beast::error::timeout)
                    TC_LOG_WARN("ai.world", "AI decision request id={} timed out after {}ms", _request.RequestId, _timeoutMs);
                else if (ec)
                    TC_LOG_WARN("ai.world", "AI decision request id={} failed: {}", _request.RequestId, ec.message());
                else if (!success)
                    TC_LOG_WARN("ai.world", "AI decision request id={} completed with non-2xx status={} latency={}ms",
                        _request.RequestId, statusCode, latencyMs);
                else
                    TC_LOG_INFO("ai.world", "AI decision response id={} snapshot={} action={} latency={}ms",
                        _request.RequestId, _request.SnapshotSequence, action, latencyMs);

                AIResponse* response = new AIResponse();
                response->RequestId = _request.RequestId;
                response->Type = AIRequestType::Decision;
                response->SnapshotSequence = _request.SnapshotSequence;
                response->Success = success;
                response->StatusCode = statusCode;
                response->LatencyMs = latencyMs;
                response->Action = success ? action : std::string();

                _responseQueue->Enqueue(response);
            }

            tcp::resolver _resolver;
            beast::tcp_stream _stream;
            net::steady_timer _resolveTimer;
            beast::flat_buffer _buffer;
            http::request<http::string_body> _httpRequest;
            http::response<http::string_body> _httpResponse;

            std::string _host;
            std::string _port;
            uint32 _timeoutMs;
            AIRequest _request;
            MPSCQueue<AIResponse>* _responseQueue;
            std::atomic<bool>* _inFlightFlag;
            std::chrono::steady_clock::time_point _startTime;
            std::atomic<bool> _completed { false };
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

    // Guards against a health check pileup when RequestTimeoutMs is set
    // higher than HealthIntervalMs (e.g. 30000/1000): without this, every
    // Update() tick would spawn another in-flight session against a slow or
    // unresponsive ai-server instead of waiting for the previous one.
    std::atomic<bool> HealthCheckInFlight { false };

    // Same guard, for decisions: SnapshotIntervalMs is the decision cadence,
    // and nothing stops it being configured shorter than RequestTimeoutMs.
    std::atomic<bool> DecisionInFlight { false };
};

AIClient::AIClient(Trinity::Asio::IoContext& ioContext, std::string host, std::string port, uint32 requestTimeoutMs)
    : _impl(std::make_unique<Impl>(ioContext, std::move(host), std::move(port), requestTimeoutMs))
{
}

AIClient::~AIClient() = default;

uint64 AIClient::SubmitHealthCheck()
{
    if (_impl->HealthCheckInFlight.exchange(true, std::memory_order_acq_rel))
    {
        TC_LOG_DEBUG("ai.world", "AI health check skipped: previous request still in flight");
        return 0;
    }

    uint64 requestId = _impl->NextRequestId.fetch_add(1, std::memory_order_relaxed);

    net::io_context& rawIoContext = _impl->IoContextRef;
    std::string const& host = _impl->Host;
    std::string const& port = _impl->Port;
    uint32 timeoutMs = _impl->RequestTimeoutMs;
    MPSCQueue<AIResponse>* responseQueue = &_impl->ResponseQueue;
    std::atomic<bool>* inFlightFlag = &_impl->HealthCheckInFlight;

    // Hand the actual work off to the io_context's worker threads. Nothing
    // below this point ever runs on the calling (world update) thread.
    net::post(rawIoContext, [&rawIoContext, host, port, timeoutMs, requestId, responseQueue, inFlightFlag]()
    {
        std::make_shared<HealthCheckSession>(rawIoContext, host, port, timeoutMs, requestId, responseQueue, inFlightFlag)->Run();
    });

    return requestId;
}

uint64 AIClient::SubmitDecision(AIRequest request)
{
    if (_impl->DecisionInFlight.exchange(true, std::memory_order_acq_rel))
    {
        TC_LOG_DEBUG("ai.world", "AI decision skipped: previous request still in flight");
        return 0;
    }

    uint64 requestId = _impl->NextRequestId.fetch_add(1, std::memory_order_relaxed);
    request.RequestId = requestId;
    request.Type = AIRequestType::Decision;

    net::io_context& rawIoContext = _impl->IoContextRef;
    std::string const& host = _impl->Host;
    std::string const& port = _impl->Port;
    uint32 timeoutMs = _impl->RequestTimeoutMs;
    MPSCQueue<AIResponse>* responseQueue = &_impl->ResponseQueue;
    std::atomic<bool>* inFlightFlag = &_impl->DecisionInFlight;

    net::post(rawIoContext, [&rawIoContext, host, port, timeoutMs, request, responseQueue, inFlightFlag]()
    {
        std::make_shared<DecisionSession>(rawIoContext, host, port, timeoutMs, request, responseQueue, inFlightFlag)->Run();
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
