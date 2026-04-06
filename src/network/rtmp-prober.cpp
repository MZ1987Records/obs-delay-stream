/*
 * rtmp-prober.cpp
 *
 * 計測ステップ:
 *   1. TCP SYN → SYN-ACK (TCP RTT)
 *   2. RTMP C0+C1 送信 → S0+S1 受信 (RTMP handshake RTT)
 *   3. 両者の平均を「ネットワーク往復レイテンシ」とする
 *   4. ÷2 で片道レイテンシを推定
 *   5. これを N回繰り返して平均・最大・最小を算出
 *
 * Windows (Winsock2) のみ対応。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <vector>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <cmath>

#include "network/rtmp-prober.hpp"
#include "core/constants.hpp"

using ProbeClk = std::chrono::steady_clock;
using ProbeMs  = std::chrono::duration<double, std::milli>;

// ---- RAII guards (implementation-local) ----

struct AddrInfoGuard {
    addrinfo* p = nullptr;
    ~AddrInfoGuard() { if (p) freeaddrinfo(p); }
};

struct SocketGuard {
    SOCKET s = INVALID_SOCKET;
    ~SocketGuard() { if (s != INVALID_SOCKET) closesocket(s); }
};

// ============================================================
// RtmpProber
// ============================================================

RtmpProber::RtmpProber() {
    // Winsock初期化（多重呼び出し安全）
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

RtmpProber::~RtmpProber() {
    cancel();
}

bool RtmpProber::start(const std::string& rtmp_url,
                       int num_probes,
                       int interval_ms)
{
    if (running_) return false;

    if (!parse_url(rtmp_url, host_, port_)) {
        RtmpProbeResult r;
        r.error_msg = "URLの解析に失敗しました: " + rtmp_url;
        if (on_result) on_result(r);
        return false;
    }

    running_     = true;
    num_probes_  = num_probes;
    interval_ms_ = interval_ms;

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this]() { probe_loop(); });
    return true;
}

void RtmpProber::cancel() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

bool RtmpProber::is_running() const {
    return running_;
}

RtmpProbeResult RtmpProber::last_result() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_result_;
}

std::string RtmpProber::host() const { return host_; }
int         RtmpProber::port() const { return port_; }

// ============================================================
// RTMP URL → host, port 解析
// 対応形式:
//   rtmp://host:port/app/key
//   rtmps://host:port/...
//   host:port
//   host  (port=1935)
// ============================================================
bool RtmpProber::parse_url(const std::string& url, std::string& host, int& port) {
    std::string s = url;

    // スキーム除去
    for (auto prefix : {"rtmps://", "rtmp://"}) {
        if (s.rfind(prefix, 0) == 0) {
            s = s.substr(std::strlen(prefix));
            break;
        }
    }

    // パス除去 (最初の '/' まで)
    auto slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);

    // host:port 分割
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        port = std::atoi(s.c_str() + colon + 1);
        if (port <= 0 || port > 65535) port = 1935;
    } else {
        host = s;
        port = 1935;
    }

    return !host.empty();
}

// ============================================================
// 1回分の計測: TCP接続 + RTMP C0C1 送受信
// 戻り値: RTT (ms) または -1.0 (失敗)
// ============================================================
double RtmpProber::probe_once() {
    // --- アドレス解決 ---
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port_);
    AddrInfoGuard ai;
    if (getaddrinfo(host_.c_str(), port_str, &hints, &ai.p) != 0 || !ai.p)
        return -1.0;

    SocketGuard sg;
    sg.s = socket(ai.p->ai_family, ai.p->ai_socktype, ai.p->ai_protocol);
    if (sg.s == INVALID_SOCKET) return -1.0;

    // タイムアウト設定
    DWORD timeout_ms = SOCKET_TIMEOUT_MS;
    setsockopt(sg.s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(sg.s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    // --- TCP接続（ここがTCP RTTを含む） ---
    auto t0 = ProbeClk::now();
    if (connect(sg.s, ai.p->ai_addr, (int)ai.p->ai_addrlen) != 0)
        return -1.0;

    // --- RTMP C0 + C1 送信 (1 + 1536 bytes) ---
    uint8_t c0c1[1 + 1536] = {};
    c0c1[0] = 3; // RTMP version

    if (send(sg.s, reinterpret_cast<const char*>(c0c1), sizeof(c0c1), 0)
        != sizeof(c0c1))
        return -1.0;

    // --- S0 + S1 + S2 受信 (1 + 1536 + 1536 bytes) ---
    // 最低 S0(1) + S1(1536) = 1537 bytes 来れば計測成立
    uint8_t s_buf[1 + 1536 + 1536];
    int received = 0;
    int target   = 1 + 1536; // S0+S1
    while (received < target) {
        int n = recv(sg.s, reinterpret_cast<char*>(s_buf + received),
                     target - received, 0);
        if (n <= 0) break;
        received += n;
    }

    auto t1 = ProbeClk::now();
    if (received < target) return -1.0;

    return ProbeMs(t1 - t0).count();
}

// ============================================================
// 計測ループ
// ============================================================
void RtmpProber::probe_loop() {
    std::vector<double> samples;
    int failed = 0;

    for (int i = 0; i < num_probes_ && running_; ++i) {
        double rtt = probe_once();
        if (rtt > 0.0)
            samples.push_back(rtt);
        else
            ++failed;

        // 最後の1回以外はインターバル待機
        if (i < num_probes_ - 1) {
            for (int t = 0; t < interval_ms_ && running_; ++t)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    running_ = false;

    // --- 結果集計 ---
    RtmpProbeResult r;
    r.failed  = failed;
    r.samples = (int)samples.size();

    if (samples.empty()) {
        r.valid     = false;
        r.error_msg = "全ての計測が失敗しました。\n"
                      "RTMPサーバーのアドレスとポートを確認してください。";
    } else {
        r.valid       = true;
        r.min_rtt_ms  = *std::min_element(samples.begin(), samples.end());
        r.max_rtt_ms  = *std::max_element(samples.begin(), samples.end());
        r.avg_rtt_ms  = std::accumulate(samples.begin(), samples.end(), 0.0)
                        / r.samples;
        r.avg_latency_ms = r.avg_rtt_ms / 2.0;

        // ジッター (標準偏差)
        double sq_sum = 0;
        for (double v : samples)
            sq_sum += (v - r.avg_rtt_ms) * (v - r.avg_rtt_ms);
        r.jitter_ms = std::sqrt(sq_sum / r.samples);
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        last_result_ = r;
    }

    if (on_result) on_result(r);
}
