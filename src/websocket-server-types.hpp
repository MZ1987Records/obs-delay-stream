#pragma once

#include <cstdint>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using WsServer   = websocketpp::server<websocketpp::config::asio>;
using ConnHandle = websocketpp::connection_hdl;
using Clock      = std::chrono::steady_clock;
using Ms         = std::chrono::duration<double, std::milli>;

inline constexpr uint32_t MAGIC_AUDI = 0x41554449u; // "AUDI"
inline constexpr uint32_t MAGIC_OPUS = 0x4F505553u; // "OPUS"

struct LatencyResult {
    bool   valid       = false;
    double avg_rtt_ms  = 0.0;
    double avg_one_way = 0.0;
    double min_rtt_ms  = 0.0;
    double max_rtt_ms  = 0.0;
    int    samples     = 0;
};

struct ConnInfo {
    std::string stream_id;
    int         ch = -1;
    bool        force_pcm = false;
};

struct ChannelState {
    std::set<ConnHandle, std::owner_less<ConnHandle>> conns;

    std::map<int, Clock::time_point> ping_times;
    std::vector<double>              rtt_samples;
    std::atomic<bool>                measuring{false};
    LatencyResult                    last_result;

    double                           last_applied_delay{-1.0};
    std::string                      last_applied_reason;

    std::function<void(const std::string& stream_id, int ch, LatencyResult)> on_result;
};
