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

    // Deliberately not a general JSON library - this project's actual
    // build environment (docker/trinitycore/Dockerfile.dev/.runtime, both
    // Ubuntu 22.04 + libboost-all-dev) is pinned to Boost 1.74, one
    // release short of Boost.JSON (added in 1.75). A first pass at 2.9B
    // reached for <boost/json/src.hpp> anyway, on the mistaken assumption
    // that whatever Boost this file's existing Beast/Asio usage needed
    // would also have JSON - it wouldn't have compiled on this project's
    // own image. Hand assembly stays: just this one fixed, versioned
    // schema (see DecisionRequest/AgentContext), all numeric/bool/
    // enum-ToString() fields with nothing that needs escaping (no
    // freeform strings ever cross this boundary).
    std::string BuildNeedsJson(NeedsState const& needs)
    {
        std::ostringstream body;
        body << "{\"health_pressure\":" << needs.HealthPressure
             << ",\"hunger\":" << needs.Hunger
             << ",\"fatigue\":" << needs.Fatigue
             << ",\"safety_pressure\":" << needs.SafetyPressure
             << ",\"resource_pressure\":" << needs.ResourcePressure
             << "}";
        return body.str();
    }

    std::string BuildActiveGoalJson(std::optional<ActiveGoal> const& goal)
    {
        if (!goal)
            return "null";

        std::ostringstream body;
        body << "{\"type\":\"" << ToString(goal->Type) << "\""
             << ",\"priority\":\"" << ToString(goal->Priority) << "\""
             << ",\"source\":\"" << ToString(goal->Source) << "\""
             << ",\"utility\":" << goal->Utility
             << ",\"started_at_ms\":" << goal->StartedAtMs
             << ",\"timeout_ms\":" << goal->TimeoutMs
             << "}";
        return body.str();
    }

    std::string BuildDecisionEntityJson(DecisionEntity const& entity)
    {
        std::ostringstream body;
        body << "{\"entry\":" << entity.Entry
             << ",\"agent_id\":" << entity.Agent.Value
             << "}";
        return body.str();
    }

    std::string BuildDecisionMemoryJson(DecisionMemory const& memory)
    {
        std::ostringstream body;
        body << "{\"tier\":\"" << ToString(memory.Tier) << "\""
             << ",\"memory_id\":" << memory.MemoryId
             << ",\"type\":\"" << ToString(memory.Type) << "\""
             << ",\"importance\":" << memory.Importance
             << ",\"relevance\":" << memory.Relevance
             << ",\"source_event_id\":" << memory.SourceEventId
             << ",\"source_occurred_at_ms\":" << memory.SourceOccurredAtMs
             << ",\"source_event_type\":" << (memory.SourceEventType ? (std::string("\"") + ToString(*memory.SourceEventType) + "\"") : "null")
             << ",\"first_observed_at_ms\":" << memory.FirstObservedAtMs
             << ",\"last_observed_at_ms\":" << memory.LastObservedAtMs
             << ",\"location\":{\"map_id\":" << memory.Location.MapId
             << ",\"x\":" << memory.Location.X << ",\"y\":" << memory.Location.Y << ",\"z\":" << memory.Location.Z << "}"
             << ",\"actor\":" << BuildDecisionEntityJson(memory.Actor)
             << ",\"target\":" << BuildDecisionEntityJson(memory.Target)
             << "}";
        return body.str();
    }

    std::string BuildAgentContextJson(AgentContext const& context)
    {
        AgentSnapshot const& self = context.Self;

        std::ostringstream body;
        body << "{\"agent_id\":" << self.Agent.Value
             << ",\"snapshot_sequence\":" << self.SnapshotSequence
             << ",\"spawn_id\":" << self.SpawnId
             << ",\"entry\":" << self.Entry
             << ",\"map_id\":" << self.MapId
             << ",\"position\":{\"x\":" << self.X << ",\"y\":" << self.Y << ",\"z\":" << self.Z << ",\"orientation\":" << self.Orientation << "}"
             << ",\"health\":" << self.Health
             << ",\"max_health\":" << self.MaxHealth
             << ",\"alive\":" << (self.Alive ? "true" : "false")
             << ",\"in_combat\":" << (self.InCombat ? "true" : "false")
             << ",\"needs\":" << BuildNeedsJson(context.Needs)
             << ",\"active_goal\":" << BuildActiveGoalJson(context.Goal)
             << ",\"relevant_memories\":[";

        for (std::size_t i = 0; i < context.RelevantMemories.size(); ++i)
        {
            if (i > 0)
                body << ",";
            body << BuildDecisionMemoryJson(context.RelevantMemories[i]);
        }

        body << "],\"available_actions\":[";

        for (std::size_t i = 0; i < context.AvailableActions.size(); ++i)
        {
            if (i > 0)
                body << ",";
            body << "\"" << ToString(context.AvailableActions[i]) << "\"";
        }

        body << "]}";
        return body.str();
    }

    // Builds the versioned /decision request body: protocol_version,
    // request_id, and the full agent_context - see DecisionRequest.
    std::string BuildDecisionRequestBody(AIRequest const& request)
    {
        std::ostringstream body;
        body << "{\"protocol_version\":" << ToUnderlying(request.Decision.Version)
             << ",\"request_id\":" << request.Decision.RequestId
             << ",\"agent_context\":" << BuildAgentContextJson(request.Decision.Context)
             << "}";
        return body.str();
    }

    // Finds the (whitespace-only-preceded) '{'...'}' object value of `key`
    // and returns the substring spanning both braces, tracking brace depth
    // so a nested object doesn't end the search early. Safe for this exact,
    // fully self-controlled response schema, where no string value between
    // the braces can itself contain a literal '{' or '}' (decision.type is
    // always one of a small fixed set of plain identifiers - see
    // ParseDecisionResponseBody) - not a general-purpose JSON object
    // finder. Returns false if the key is missing, its value isn't an
    // object, or the object is unterminated.
    bool FindObjectField(std::string const& body, std::string const& key, std::string& out)
    {
        std::string needle = "\"" + key + "\"";
        size_t keyPos = body.find(needle);
        if (keyPos == std::string::npos)
            return false;

        size_t colonPos = body.find(':', keyPos + needle.size());
        if (colonPos == std::string::npos)
            return false;

        size_t braceStart = colonPos + 1;
        while (braceStart < body.size() && std::isspace(static_cast<unsigned char>(body[braceStart])))
            ++braceStart;

        if (braceStart >= body.size() || body[braceStart] != '{')
            return false;

        int depth = 0;
        size_t i = braceStart;
        for (; i < body.size(); ++i)
        {
            if (body[i] == '{')
                ++depth;
            else if (body[i] == '}')
            {
                --depth;
                if (depth == 0)
                    break;
            }
        }
        if (depth != 0)
            return false;

        out = body.substr(braceStart, i - braceStart + 1);
        return true;
    }

    bool FindUintField(std::string const& body, std::string const& key, uint64& out)
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
    }

    bool FindStringField(std::string const& body, std::string const& key, std::string& out)
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
    }

    // Defensive, minimal extraction for the fixed
    // {"protocol_version":N,"agent_id":N,"request_id":N,"snapshot_sequence":N,"decision":{"type":"STR"}}
    // response shape - not a general JSON parser (see the comment above
    // BuildNeedsJson for why). Tolerant of key order, whitespace, and
    // unknown extra keys; never throws; returns false (never partial
    // output) if a required field is missing, malformed, or names an
    // intent type this build doesn't know. Callers must still check
    // protocolVersion/agentId/requestId/snapshotSequence against what was
    // actually sent - parsing successfully only means the body was
    // well-formed, not that it answers this request.
    bool ParseDecisionResponseBody(std::string const& body, uint64& protocolVersion, uint64& agentId, uint64& requestId, uint64& snapshotSequence, DecisionIntent& intent)
    {
        if (!FindUintField(body, "protocol_version", protocolVersion) ||
            !FindUintField(body, "agent_id", agentId) ||
            !FindUintField(body, "request_id", requestId) ||
            !FindUintField(body, "snapshot_sequence", snapshotSequence))
            return false;

        std::string decisionObject;
        if (!FindObjectField(body, "decision", decisionObject))
            return false;

        std::string typeString;
        if (!FindStringField(decisionObject, "type", typeString))
            return false;

        if (typeString == "NONE")
            intent.Type = DecisionIntentType::None;
        else if (typeString == "FLEE")
            intent.Type = DecisionIntentType::Flee;
        else if (typeString == "MOVE_TO")
            intent.Type = DecisionIntentType::MoveTo;
        else if (typeString == "EAT")
            intent.Type = DecisionIntentType::Eat;
        else
            return false; // unknown intent type - reject rather than guess

        return true;
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

                TC_LOG_INFO("ai.world", "AI decision request id={} version={} agent={} snapshot={} spawn={} submitted",
                    _request.RequestId, ToUnderlying(_request.Decision.Version), _request.Decision.Context.Self.Agent.Value,
                    _request.Decision.Context.Self.SnapshotSequence, _request.Decision.Context.Self.SpawnId);

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
                Complete(false, 0, beast::error::timeout, DecisionIntent(), std::string());
            }

            void OnResolve(beast::error_code ec, tcp::resolver::results_type results)
            {
                _resolveTimer.cancel();

                if (ec)
                    return Complete(false, 0, ec, DecisionIntent(), std::string());

                if (_completed.load(std::memory_order_acquire))
                    return; // resolve timeout already completed this request

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                _stream.async_connect(results,
                    beast::bind_front_handler(&DecisionSession::OnConnect, shared_from_this()));
            }

            void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
            {
                if (ec)
                    return Complete(false, 0, ec, DecisionIntent(), std::string());

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                http::async_write(_stream, _httpRequest,
                    beast::bind_front_handler(&DecisionSession::OnWrite, shared_from_this()));
            }

            void OnWrite(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec, DecisionIntent(), std::string());

                http::async_read(_stream, _buffer, _httpResponse,
                    beast::bind_front_handler(&DecisionSession::OnRead, shared_from_this()));
            }

            void OnRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec, DecisionIntent(), std::string());

                uint32 statusCode = _httpResponse.result_int();
                bool success = statusCode >= 200 && statusCode < 300;
                DecisionIntent intent;
                // Set only when statusCode was 2xx but the body itself made
                // the response unusable - lets Complete() tell that apart
                // from a genuine non-2xx status instead of mislabeling it.
                std::string rejectReason;
                if (success)
                {
                    uint64 responseProtocolVersion = 0;
                    uint64 responseAgentId = 0;
                    uint64 responseRequestId = 0;
                    uint64 responseSnapshotSequence = 0;
                    if (!ParseDecisionResponseBody(_httpResponse.body(), responseProtocolVersion, responseAgentId, responseRequestId, responseSnapshotSequence, intent))
                    {
                        success = false;
                        rejectReason = "parse failure";
                    }
                    // A well-formed body isn't enough on its own - verify it
                    // actually answers the request we sent, not just any
                    // request/response the queue happened to pull off HTTP
                    // pipelining, a proxy, or a future protocol bug - and,
                    // new in 2.9A, that it speaks the same protocol version
                    // this request was sent as.
                    else if (responseProtocolVersion != ToUnderlying(_request.Decision.Version) ||
                        responseAgentId != _request.Decision.Context.Self.Agent.Value || responseRequestId != _request.RequestId ||
                        responseSnapshotSequence != _request.Decision.Context.Self.SnapshotSequence)
                    {
                        success = false;
                        std::ostringstream detail;
                        detail << "protocol mismatch (server replied version=" << responseProtocolVersion
                               << " agent=" << responseAgentId << " id=" << responseRequestId
                               << " snapshot=" << responseSnapshotSequence << ")";
                        rejectReason = detail.str();
                    }
                }
                Complete(success, statusCode, ec, intent, rejectReason);
            }

            // Guarded by _completed - see HealthCheckSession::Complete().
            // rejectReason is only non-empty when statusCode was 2xx but the
            // body itself made the response unusable (see OnRead) - without
            // it, this would otherwise log a 2xx response as "non-2xx".
            void Complete(bool success, uint32 statusCode, beast::error_code ec, DecisionIntent const& intent, std::string const& rejectReason)
            {
                if (_completed.exchange(true, std::memory_order_acq_rel))
                    return;

                beast::error_code ignored;
                _stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
                _inFlightFlag->store(false, std::memory_order_release);

                uint32 latencyMs = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - _startTime).count());

                AgentId requestAgent = _request.Decision.Context.Self.Agent;
                uint64 requestSnapshotSequence = _request.Decision.Context.Self.SnapshotSequence;

                if (ec == beast::error::timeout)
                    TC_LOG_WARN("ai.world", "AI decision request id={} agent={} timed out after {}ms",
                        _request.RequestId, requestAgent.Value, _timeoutMs);
                else if (ec)
                    TC_LOG_WARN("ai.world", "AI decision request id={} agent={} failed: {}",
                        _request.RequestId, requestAgent.Value, ec.message());
                else if (!rejectReason.empty())
                    TC_LOG_WARN("ai.world", "AI decision response id={} agent={} {} status={} latency={}ms",
                        _request.RequestId, requestAgent.Value, rejectReason, statusCode, latencyMs);
                else if (!success)
                    TC_LOG_WARN("ai.world", "AI decision request id={} agent={} completed with non-2xx status={} latency={}ms",
                        _request.RequestId, requestAgent.Value, statusCode, latencyMs);
                else
                    TC_LOG_INFO("ai.world", "AI decision response id={} agent={} snapshot={} intent={} latency={}ms",
                        _request.RequestId, requestAgent.Value, requestSnapshotSequence, ToString(intent.Type), latencyMs);

                AIResponse* response = new AIResponse();
                response->RequestId = _request.RequestId;
                response->Type = AIRequestType::Decision;
                response->Agent = requestAgent;
                response->SnapshotSequence = requestSnapshotSequence;

                // Milestone 2.9C/2.9C P2 fix: echoed from the request's own
                // AgentContext, never from anything ai-server sent back -
                // see DecisionProvenance.h.
                if (_request.Decision.Context.Goal)
                {
                    response->Provenance.Goal = _request.Decision.Context.Goal->Type;
                    response->Provenance.GoalStartedAtMs = _request.Decision.Context.Goal->StartedAtMs;
                }
                response->Provenance.RuntimeGuid = _request.Decision.Context.Self.Guid;

                response->Success = success;
                response->StatusCode = statusCode;
                response->LatencyMs = latencyMs;

                if (success)
                {
                    DecisionResponse decision;
                    decision.Version = _request.Decision.Version;
                    decision.RequestId = _request.RequestId;
                    decision.Agent = requestAgent;
                    decision.SnapshotSequence = requestSnapshotSequence;
                    decision.Intent = intent;
                    response->Decision = std::move(decision);
                }

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
    request.Decision.RequestId = requestId;
    request.Decision.Version = CurrentProtocolVersion;

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
