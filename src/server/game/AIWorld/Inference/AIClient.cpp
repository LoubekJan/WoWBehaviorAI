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
#include "DynamicTaskJsonCodec.h"
#include "IoContext.h"
#include "Log.h"
#include "Metric.h"
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
            // Milestone 2.10A: inFlightCount is a bounded counter, not a
            // single bool - multiple DecisionSessions for different agents
            // can be alive at once, up to AIClient's own maxDecisionsInFlight.
            // Complete() decrements it exactly once per session, the same
            // "guarded by _completed" contract as everything else it does.
            DecisionSession(net::io_context& ioContext, std::string const& host, std::string const& port,
                uint32 timeoutMs, AIRequest const& request, MPSCQueue<AIResponse>* responseQueue, std::atomic<uint32>* inFlightCount)
                : _resolver(net::make_strand(ioContext)), _stream(net::make_strand(ioContext)),
                  _resolveTimer(_resolver.get_executor()),
                  _host(host), _port(port), _timeoutMs(timeoutMs), _request(request),
                  _responseQueue(responseQueue), _inFlightCount(inFlightCount),
                  _startTime(std::chrono::steady_clock::now())
            {
                // Milestone 2.9D: how long this request actually sat
                // between AIClient::SubmitDecision() posting it and this
                // session beginning to run on an asio worker thread - both
                // construction and Run() happen here, on the worker, once
                // the posted task is actually picked up. Deliberately not
                // HTTP/session latency (that's _startTime, used for
                // LatencyMs below) - a healthy queue_ms with a high
                // latency_ms means slow ai-server/network, not a
                // backed-up transport.
                _queueMs = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(_startTime - _request.SubmittedAt).count());
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
                Complete(false, 0, beast::error::timeout, DecisionIntent(), std::string(), false);
            }

            void OnResolve(beast::error_code ec, tcp::resolver::results_type results)
            {
                _resolveTimer.cancel();

                if (ec)
                    return Complete(false, 0, ec, DecisionIntent(), std::string(), false);

                if (_completed.load(std::memory_order_acquire))
                    return; // resolve timeout already completed this request

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                _stream.async_connect(results,
                    beast::bind_front_handler(&DecisionSession::OnConnect, shared_from_this()));
            }

            void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
            {
                if (ec)
                    return Complete(false, 0, ec, DecisionIntent(), std::string(), false);

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                http::async_write(_stream, _httpRequest,
                    beast::bind_front_handler(&DecisionSession::OnWrite, shared_from_this()));
            }

            void OnWrite(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec, DecisionIntent(), std::string(), false);

                http::async_read(_stream, _buffer, _httpResponse,
                    beast::bind_front_handler(&DecisionSession::OnRead, shared_from_this()));
            }

            void OnRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec, DecisionIntent(), std::string(), false);

                uint32 statusCode = _httpResponse.result_int();
                bool success = statusCode >= 200 && statusCode < 300;
                DecisionIntent intent;
                // Set only when statusCode was 2xx but the body itself made
                // the response unusable - lets Complete() tell that apart
                // from a genuine non-2xx status instead of mislabeling it.
                std::string rejectReason;
                // Milestone 2.9D: distinguishes the two ways rejectReason
                // can end up non-empty, for the ai.world.decision.result
                // metric's result tag - malformed_response vs
                // protocol_mismatch are different failure modes (a broken
                // response body vs a well-formed one that answers the
                // wrong request) worth telling apart in metrics, even
                // though today's TC_LOG line below treats them the same.
                bool protocolMismatch = false;
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
                        protocolMismatch = true;
                        std::ostringstream detail;
                        detail << "protocol mismatch (server replied version=" << responseProtocolVersion
                               << " agent=" << responseAgentId << " id=" << responseRequestId
                               << " snapshot=" << responseSnapshotSequence << ")";
                        rejectReason = detail.str();
                    }
                }
                Complete(success, statusCode, ec, intent, rejectReason, protocolMismatch);
            }

            // Guarded by _completed - see HealthCheckSession::Complete().
            // rejectReason is only non-empty when statusCode was 2xx but the
            // body itself made the response unusable (see OnRead) - without
            // it, this would otherwise log a 2xx response as "non-2xx".
            // protocolMismatch (2.9D) only ever distinguishes the metric's
            // result tag between two rejectReason causes; it doesn't change
            // the TC_LOG branch taken below, which treats both as the same
            // "response ... {rejectReason}" case.
            void Complete(bool success, uint32 statusCode, beast::error_code ec, DecisionIntent const& intent, std::string const& rejectReason, bool protocolMismatch)
            {
                if (_completed.exchange(true, std::memory_order_acq_rel))
                    return;

                beast::error_code ignored;
                _stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
                _inFlightCount->fetch_sub(1, std::memory_order_acq_rel);

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

                // Milestone 2.9D: one value per completed decision request,
                // tagged by exactly how it turned out - timeout rate,
                // transport-error rate, malformed-response rate, and
                // protocol-mismatch rate are then just
                // count(result=X)/count(total) on this one series, no
                // separate hand-maintained counters needed. result and
                // outcome-derived tags only (no request_id/agent_id/
                // spawn_id/RuntimeGuid - those belong in the TC_LOG lines
                // above, not in low-cardinality metric tags).
                char const* resultTag;
                bool malformedResponse = false;
                if (ec == beast::error::timeout)
                    resultTag = "timeout";
                else if (ec)
                    resultTag = "transport_error";
                else if (protocolMismatch)
                    resultTag = "protocol_mismatch";
                else if (!rejectReason.empty())
                {
                    resultTag = "malformed_response";
                    malformedResponse = true;
                }
                else if (!success)
                    resultTag = "http_error";
                else
                    resultTag = "success";

                TC_METRIC_VALUE("ai.world.decision.result", uint64(1), TC_METRIC_TAG("result", resultTag));
                TC_METRIC_VALUE("ai.world.decision.queue_ms", _queueMs, TC_METRIC_TAG("result", resultTag));
                TC_METRIC_VALUE("ai.world.decision.latency_ms", latencyMs, TC_METRIC_TAG("result", resultTag));

                // 2.9D P2 fix: ai.world.decision.result's "success" only
                // means transport/protocol succeeded, not that the
                // decision itself was usable - and a timeout/transport_error/
                // http_error/protocol_mismatch means there was no decision
                // content to judge the quality of at all. malformed_response
                // is different: the world thread DID receive something
                // claiming to be a decision, it just wasn't a usable one -
                // that's the one transport-layer outcome that also belongs
                // in ai.world.decision.validity's invalid count (see
                // AIWorldMgr::ValidateDecisionIntent() for the rest of that
                // series - NONE/FLEE ALLOWED are valid, unsupported remote
                // intents and FLEE REJECTED are invalid, and none of the
                // stale/discard cases count in either direction).
                if (malformedResponse)
                    TC_METRIC_VALUE("ai.world.decision.validity", uint64(1),
                        TC_METRIC_TAG("validity", "invalid"), TC_METRIC_TAG("reason", "malformed_response"));

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
            std::atomic<uint32>* _inFlightCount;
            std::chrono::steady_clock::time_point _startTime;
            uint32 _queueMs = 0;
            std::atomic<bool> _completed { false };
    };

    // Milestone 2.13A2's own /dynamic-task response cap
    // (AI_TASK_MODEL_MAX_RESPONSE_BYTES, default 16384) - mirrored here so
    // a misbehaving or malicious ai-server/model backend can't make this
    // side buffer an unbounded response either. DecisionSession/
    // HealthCheckSession read through a plain http::response<string_body>
    // (Beast's own default body limit applies); DynamicTaskSession uses an
    // explicit response_parser with this limit set instead, per review.
    constexpr std::size_t DynamicTaskResponseBodyLimit = 16384;

    // Same resolve/connect/write/read/timeout shape as DecisionSession
    // (see its comments for why each piece is there), POSTing to
    // /dynamic-task and using the permanent, escape-aware
    // DynamicTaskJsonCodec instead of AIClient.cpp's own /decision-only
    // hand-rolled Build*Json()/Find*Field() helpers - see
    // DynamicTaskJsonCodec.h for why those aren't reused here (this
    // schema carries genuine free text, /decision's never did).
    class DynamicTaskSession : public std::enable_shared_from_this<DynamicTaskSession>
    {
        public:
            DynamicTaskSession(net::io_context& ioContext, std::string const& host, std::string const& port,
                uint32 timeoutMs, AIRequest const& request, MPSCQueue<AIResponse>* responseQueue, std::atomic<uint32>* inFlightCount)
                : _resolver(net::make_strand(ioContext)), _stream(net::make_strand(ioContext)),
                  _resolveTimer(_resolver.get_executor()),
                  _host(host), _port(port), _timeoutMs(timeoutMs), _request(request),
                  _responseQueue(responseQueue), _inFlightCount(inFlightCount),
                  _startTime(std::chrono::steady_clock::now())
            {
                _parser.body_limit(DynamicTaskResponseBodyLimit);
            }

            void Run()
            {
                _httpRequest.version(11);
                _httpRequest.method(http::verb::post);
                _httpRequest.target(DynamicTaskEndpoint);
                _httpRequest.set(http::field::host, _host + ":" + _port);
                _httpRequest.set(http::field::user_agent, "TrinityCore-AIWorld");
                _httpRequest.set(http::field::content_type, "application/json");
                _httpRequest.body() = SerializeDynamicTaskRequest(_request.DynamicTask);
                _httpRequest.prepare_payload();

                TC_LOG_INFO("ai.world", "AI dynamic-task request id={} version={} agent={} snapshot={} submitted",
                    _request.RequestId, ToUnderlying(_request.DynamicTask.Version),
                    _request.DynamicTask.Context.Agent.Value, _request.DynamicTask.Context.SnapshotSequence);

                _resolveTimer.expires_after(std::chrono::milliseconds(_timeoutMs));
                _resolveTimer.async_wait(
                    beast::bind_front_handler(&DynamicTaskSession::OnResolveTimeout, shared_from_this()));

                _resolver.async_resolve(_host, _port,
                    beast::bind_front_handler(&DynamicTaskSession::OnResolve, shared_from_this()));
            }

        private:
            void OnResolveTimeout(beast::error_code ec)
            {
                if (ec == net::error::operation_aborted)
                    return; // resolve finished first and cancelled this timer

                _resolver.cancel();
                Complete(false, 0, beast::error::timeout, std::nullopt, std::string(), false);
            }

            void OnResolve(beast::error_code ec, tcp::resolver::results_type results)
            {
                _resolveTimer.cancel();

                if (ec)
                    return Complete(false, 0, ec, std::nullopt, std::string(), false);

                if (_completed.load(std::memory_order_acquire))
                    return; // resolve timeout already completed this request

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                _stream.async_connect(results,
                    beast::bind_front_handler(&DynamicTaskSession::OnConnect, shared_from_this()));
            }

            void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
            {
                if (ec)
                    return Complete(false, 0, ec, std::nullopt, std::string(), false);

                _stream.expires_after(std::chrono::milliseconds(_timeoutMs));
                http::async_write(_stream, _httpRequest,
                    beast::bind_front_handler(&DynamicTaskSession::OnWrite, shared_from_this()));
            }

            void OnWrite(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec, std::nullopt, std::string(), false);

                http::async_read(_stream, _buffer, _parser,
                    beast::bind_front_handler(&DynamicTaskSession::OnRead, shared_from_this()));
            }

            void OnRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
            {
                if (ec)
                    return Complete(false, 0, ec, std::nullopt, std::string(), false);

                http::response<http::string_body> const& httpResponse = _parser.get();
                uint32 statusCode = httpResponse.result_int();
                bool success = statusCode >= 200 && statusCode < 300;

                std::optional<DynamicTaskResponse> parsed;
                std::string rejectReason;
                bool protocolMismatch = false;

                if (success)
                {
                    DynamicTaskResponse candidate;
                    if (!ParseDynamicTaskResponse(httpResponse.body(), candidate))
                    {
                        success = false;
                        rejectReason = "parse failure";
                    }
                    // A well-formed, in-contract body isn't enough on its
                    // own - verify it actually answers the exact request
                    // this session sent, the same envelope check
                    // DecisionSession's OnRead() already does for
                    // /decision. Any mismatch is a hard reject, never a
                    // "best effort" partial acceptance.
                    else if (candidate.Version != _request.DynamicTask.Version ||
                        candidate.RequestId != _request.RequestId ||
                        candidate.Agent.Value != _request.DynamicTask.Context.Agent.Value ||
                        candidate.SnapshotSequence != _request.DynamicTask.Context.SnapshotSequence)
                    {
                        success = false;
                        protocolMismatch = true;
                        std::ostringstream detail;
                        detail << "protocol mismatch (server replied version=" << ToUnderlying(candidate.Version)
                               << " agent=" << candidate.Agent.Value << " id=" << candidate.RequestId
                               << " snapshot=" << candidate.SnapshotSequence << ")";
                        rejectReason = detail.str();
                    }
                    else
                        parsed = std::move(candidate);
                }

                Complete(success, statusCode, ec, std::move(parsed), rejectReason, protocolMismatch);
            }

            // Guarded by _completed - see DecisionSession::Complete().
            // rejectReason/protocolMismatch follow the exact same meaning
            // as there. `parsed` is only set once success, well-formed
            // parse, AND envelope match all held - see OnRead().
            void Complete(bool success, uint32 statusCode, beast::error_code ec, std::optional<DynamicTaskResponse> parsed, std::string const& rejectReason, bool protocolMismatch)
            {
                if (_completed.exchange(true, std::memory_order_acq_rel))
                    return;

                beast::error_code ignored;
                _stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
                _inFlightCount->fetch_sub(1, std::memory_order_acq_rel);

                uint32 latencyMs = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - _startTime).count());

                AgentId requestAgent = _request.DynamicTask.Context.Agent;

                if (ec == beast::error::timeout)
                    TC_LOG_WARN("ai.world", "AI dynamic-task request id={} agent={} timed out after {}ms",
                        _request.RequestId, requestAgent.Value, _timeoutMs);
                else if (ec)
                    TC_LOG_WARN("ai.world", "AI dynamic-task request id={} agent={} failed: {}",
                        _request.RequestId, requestAgent.Value, ec.message());
                else if (!rejectReason.empty())
                    TC_LOG_WARN("ai.world", "AI dynamic-task response id={} agent={} {} status={} latency={}ms",
                        _request.RequestId, requestAgent.Value, rejectReason, statusCode, latencyMs);
                else if (!success)
                    TC_LOG_WARN("ai.world", "AI dynamic-task request id={} agent={} completed with non-2xx status={} latency={}ms",
                        _request.RequestId, requestAgent.Value, statusCode, latencyMs);
                else
                    TC_LOG_INFO("ai.world", "AI dynamic-task response id={} agent={} snapshot={} latency={}ms",
                        _request.RequestId, requestAgent.Value, _request.DynamicTask.Context.SnapshotSequence, latencyMs);

                (void)protocolMismatch; // reserved for a future metrics pass, same as DecisionSession's own tag

                AIResponse* response = new AIResponse();
                response->RequestId = _request.RequestId;
                response->Type = AIRequestType::DynamicTask;
                response->Agent = requestAgent;
                response->SnapshotSequence = _request.DynamicTask.Context.SnapshotSequence;

                // Always the request's own echo, regardless of outcome -
                // see AIResponse.h.
                response->QuestProvenance = _request.QuestProvenance;

                response->Success = success;
                response->StatusCode = statusCode;
                response->LatencyMs = latencyMs;
                response->DynamicTask = std::move(parsed);

                _responseQueue->Enqueue(response);
            }

            tcp::resolver _resolver;
            beast::tcp_stream _stream;
            net::steady_timer _resolveTimer;
            beast::flat_buffer _buffer;
            http::request<http::string_body> _httpRequest;
            http::response_parser<http::string_body> _parser;

            std::string _host;
            std::string _port;
            uint32 _timeoutMs;
            AIRequest _request;
            MPSCQueue<AIResponse>* _responseQueue;
            std::atomic<uint32>* _inFlightCount;
            std::chrono::steady_clock::time_point _startTime;
            std::atomic<bool> _completed { false };
    };
}

struct AIClient::Impl
{
    Impl(Trinity::Asio::IoContext& ioContext, std::string host, std::string port, uint32 requestTimeoutMs, uint32 maxDecisionsInFlight, uint32 maxDynamicTasksInFlight)
        : IoContextRef(ioContext), Host(std::move(host)), Port(std::move(port)), RequestTimeoutMs(requestTimeoutMs),
          MaxDecisionsInFlight(maxDecisionsInFlight), MaxDynamicTasksInFlight(maxDynamicTasksInFlight)
    {
    }

    Trinity::Asio::IoContext& IoContextRef;
    std::string Host;
    std::string Port;
    uint32 RequestTimeoutMs;
    uint32 MaxDecisionsInFlight;
    uint32 MaxDynamicTasksInFlight;
    std::atomic<uint64> NextRequestId { 1 };
    MPSCQueue<AIResponse> ResponseQueue;

    // Guards against a health check pileup when RequestTimeoutMs is set
    // higher than HealthIntervalMs (e.g. 30000/1000): without this, every
    // Update() tick would spawn another in-flight session against a slow or
    // unresponsive ai-server instead of waiting for the previous one. Still
    // a single bool - health checks were never part of 2.10A's bounded
    // multi-agent admission scope.
    std::atomic<bool> HealthCheckInFlight { false };

    // Milestone 2.10A: bounded counter, not a single bool - up to
    // MaxDecisionsInFlight DecisionSessions for different agents can be
    // alive at once. SubmitDecision() only increments after confirming
    // there is room (see its own compare-and-increment loop); every
    // DecisionSession decrements exactly once, in Complete().
    std::atomic<uint32> DecisionsInFlight { 0 };

    // Milestone 2.13A3: the same bounded-counter pattern as
    // DecisionsInFlight, entirely separate from it - a burst of /decision
    // traffic can never starve dynamic-task admission, or vice versa.
    // Defaults to MaxDynamicTasksInFlight == 0 (see AIClient.h), so until
    // a caller explicitly configures a nonzero value, SubmitDynamicTask()
    // always reports "no slot available".
    std::atomic<uint32> DynamicTasksInFlight { 0 };
};

AIClient::AIClient(Trinity::Asio::IoContext& ioContext, std::string host, std::string port, uint32 requestTimeoutMs, uint32 maxDecisionsInFlight, uint32 maxDynamicTasksInFlight)
    : _impl(std::make_unique<Impl>(ioContext, std::move(host), std::move(port), requestTimeoutMs, maxDecisionsInFlight, maxDynamicTasksInFlight))
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
    // Milestone 2.10A: compare-and-increment rather than a single
    // exchange(true) - admits up to MaxDecisionsInFlight concurrently,
    // for however many different agents, instead of only ever one at a
    // time. Retries the compare_exchange_weak on spurious failure (the
    // loaded value is refreshed in place by the failed attempt); gives up
    // and skips only once the cap itself is actually reached.
    uint32 current = _impl->DecisionsInFlight.load(std::memory_order_acquire);
    for (;;)
    {
        if (current >= _impl->MaxDecisionsInFlight)
        {
            TC_LOG_DEBUG("ai.world", "AI decision skipped: {} decision(s) already in flight (max={})",
                current, _impl->MaxDecisionsInFlight);

            // Milestone 2.9D: without this, ai.world.decision.queue_ms
            // could look perfectly healthy while this cap was silently
            // dropping every decision beyond it - this is the baseline a
            // scheduler built on top of SubmitDecisions() needs to already
            // be visible against.
            TC_METRIC_VALUE("ai.world.decision.submit", uint64(1), TC_METRIC_TAG("result", "skipped_in_flight"));
            return 0;
        }

        if (_impl->DecisionsInFlight.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            break;
    }

    uint64 requestId = _impl->NextRequestId.fetch_add(1, std::memory_order_relaxed);
    request.RequestId = requestId;
    request.Type = AIRequestType::Decision;
    request.Decision.RequestId = requestId;
    request.Decision.Version = CurrentProtocolVersion;
    request.SubmittedAt = std::chrono::steady_clock::now();

    net::io_context& rawIoContext = _impl->IoContextRef;
    std::string const& host = _impl->Host;
    std::string const& port = _impl->Port;
    uint32 timeoutMs = _impl->RequestTimeoutMs;
    MPSCQueue<AIResponse>* responseQueue = &_impl->ResponseQueue;
    std::atomic<uint32>* inFlightCount = &_impl->DecisionsInFlight;

    net::post(rawIoContext, [&rawIoContext, host, port, timeoutMs, request, responseQueue, inFlightCount]()
    {
        std::make_shared<DecisionSession>(rawIoContext, host, port, timeoutMs, request, responseQueue, inFlightCount)->Run();
    });

    TC_METRIC_VALUE("ai.world.decision.submit", uint64(1), TC_METRIC_TAG("result", "submitted"));

    return requestId;
}

std::vector<DecisionSubmitResult> AIClient::SubmitDecisions(std::vector<AIRequest> requests)
{
    // Milestone 2.9E: pure delegation to the existing single-request
    // primitive, in order - no new wire format, no batching at the
    // transport level. Every request still goes through SubmitDecision()'s
    // own bounded-counter admission (Milestone 2.10A) and already-existing
    // ai.world.decision.submit metric individually; this only adds a
    // caller-facing shape on top, and a single aggregate debug log for
    // this one batch.
    std::vector<DecisionSubmitResult> results;
    results.reserve(requests.size());

    uint32 submitted = 0;
    uint32 skipped = 0;

    for (AIRequest& request : requests)
    {
        AgentId agent = request.Decision.Context.Self.Agent;
        uint64 requestId = SubmitDecision(std::move(request));

        DecisionSubmitStatus status = requestId != 0 ? DecisionSubmitStatus::Submitted : DecisionSubmitStatus::SkippedInFlight;
        if (status == DecisionSubmitStatus::Submitted)
            ++submitted;
        else
            ++skipped;

        results.push_back({ agent, requestId, status });
    }

    TC_LOG_DEBUG("ai.world", "AI decision batch submitted requested={} submitted={} skipped={}",
        requests.size(), submitted, skipped);

    return results;
}

uint64 AIClient::SubmitDynamicTask(AIRequest request)
{
    // Milestone 2.13A3: same bounded compare-and-increment admission as
    // SubmitDecision(), against its own separate DynamicTasksInFlight
    // counter - see AIClient.h/Impl for why these two budgets are never
    // allowed to share a slot pool.
    uint32 current = _impl->DynamicTasksInFlight.load(std::memory_order_acquire);
    for (;;)
    {
        if (current >= _impl->MaxDynamicTasksInFlight)
        {
            TC_LOG_DEBUG("ai.world", "AI dynamic-task skipped: {} dynamic-task(s) already in flight (max={})",
                current, _impl->MaxDynamicTasksInFlight);
            return 0;
        }

        if (_impl->DynamicTasksInFlight.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            break;
    }

    uint64 requestId = _impl->NextRequestId.fetch_add(1, std::memory_order_relaxed);
    request.RequestId = requestId;
    request.Type = AIRequestType::DynamicTask;
    request.DynamicTask.RequestId = requestId;
    request.DynamicTask.Version = CurrentDynamicTaskProtocolVersion;
    request.SubmittedAt = std::chrono::steady_clock::now();

    // Milestone 2.13A3: keep QuestProvenance's own Agent/SnapshotSequence
    // in lockstep with what actually goes out on the wire in
    // DynamicTask.Context - callers fill in both from the same snapshot,
    // but stamping it here too means a caller mistake there can never
    // desynchronize the two (see AIRequest.h).
    request.QuestProvenance.Agent = request.DynamicTask.Context.Agent;
    request.QuestProvenance.SnapshotSequence = request.DynamicTask.Context.SnapshotSequence;

    net::io_context& rawIoContext = _impl->IoContextRef;
    std::string const& host = _impl->Host;
    std::string const& port = _impl->Port;
    uint32 timeoutMs = _impl->RequestTimeoutMs;
    MPSCQueue<AIResponse>* responseQueue = &_impl->ResponseQueue;
    std::atomic<uint32>* inFlightCount = &_impl->DynamicTasksInFlight;

    net::post(rawIoContext, [&rawIoContext, host, port, timeoutMs, request, responseQueue, inFlightCount]()
    {
        std::make_shared<DynamicTaskSession>(rawIoContext, host, port, timeoutMs, request, responseQueue, inFlightCount)->Run();
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
