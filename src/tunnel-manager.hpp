#pragma once
/*
 * tunnel-manager.hpp  v1.7.0
 *
 * ngrok または cloudflared を子プロセスとして起動し、
 * 取得したパブリックURL (wss://) を返す。
 *
 * ngrok:
 *   起動: ngrok.exe tcp 19000 --authtoken <token>
 *         (または事前に `ngrok config add-authtoken` 済みなら tokenなしでも可)
 *   URL取得: http://127.0.0.1:4040/api/tunnels をポーリング
 *   URL形式: tcp://X.tcp.ngrok.io:NNNNN → ws://X.tcp.ngrok.io:NNNNN に変換
 *   ※ ngrokのTCPトンネルはTLS非対応のためws://になる
 *     (有料プランでTLS対応ドメインを使えばwss://も可能)
 *
 * cloudflared:
 *   起動: cloudflared.exe tunnel --url http://localhost:19000
 *   URL取得: stderr に出力される "https://xxxx.trycloudflare.com" を捕捉
 *   URL形式: https:// → wss:// に変換
 *   ※ 無料・認証不要・固定でない（起動毎にURLが変わる）
 */

// Windows headers in correct order (winsock2 before windows)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <vector>
#include <chrono>
#include <algorithm>

// ============================================================
// TunnelType
// ============================================================
enum class TunnelType { Ngrok, Cloudflared };

// ============================================================
// TunnelState
// ============================================================
enum class TunnelState {
    Idle,       // 未起動
    Starting,   // 起動中 (URL取得待ち)
    Running,    // 動作中
    Stopped,    // 停止済み
    Error,      // エラー
};

// ============================================================
// TunnelManager
// ============================================================
class TunnelManager {
public:
    // URL取得完了 / エラー コールバック
    std::function<void(const std::string& url)>   on_url_ready;
    std::function<void(const std::string& reason)> on_error;
    std::function<void()>                          on_stopped;

    TunnelManager()  { WSADATA w{}; WSAStartup(MAKEWORD(2,2), &w); }
    ~TunnelManager() { stop(); }

    // ----- 起動 -----
    // exe_path: ngrok.exe または cloudflared.exe のフルパス
    // token   : ngrokのみ使用 (空文字列なら設定ファイルのトークンを使用)
    bool start(TunnelType type,
               const std::string& exe_path,
               const std::string& token = "",
               int ws_port = 19000)
    {
        if (state_ == TunnelState::Running ||
            state_ == TunnelState::Starting) return false;

        type_     = type;
        exe_path_ = exe_path;
        token_    = token;
        ws_port_  = ws_port;
        url_      = "";
        error_    = "";
        state_    = TunnelState::Starting;

        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this]() { run_loop(); });
        return true;
    }

    // ----- 停止 -----
    void stop() {
        stop_requested_ = true;
        kill_child();
        if (worker_.joinable()) worker_.join();
        state_ = TunnelState::Stopped;
        stop_requested_ = false;
        if (on_stopped) on_stopped();
    }

    // ----- 状態アクセス -----
    TunnelState state() const { return state_; }
    std::string url()   const { std::lock_guard<std::mutex> lk(mtx_); return url_; }
    std::string error() const { std::lock_guard<std::mutex> lk(mtx_); return error_; }

    // 各chの受信URL生成
    // ngrok TCPはパスルーティング対応: ws://host:port/stream_id/ch
    std::string make_ch_url(const std::string& stream_id, int ch_1idx) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (url_.empty() || stream_id.empty()) return "";
        char buf[256];
        snprintf(buf, sizeof(buf), "%s/%s/%d",
                 url_.c_str(), stream_id.c_str(), ch_1idx);
        return buf;
    }

private:
    // ============================================================
    // 子プロセス起動
    // ============================================================
    // exe_path: 実行ファイルのフルパス
    // args: コマンドライン引数（exeパス部分は含まない）
    bool launch_process(const std::string& exe_path, const std::string& args) {
        // ファイルリダイレクト方式（パイプバッファ詰まりを完全回避）
        char tmp_path[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmp_path);
        if (tmp_path[0] == 0) strcpy(tmp_path, "C:\\Temp\\");
        // Ensure directory exists
        CreateDirectoryA(tmp_path, nullptr);
        log_file_path_ = std::string(tmp_path) + "obs_tunnel_out.txt";
        blog(LOG_INFO, "[obs-delay-stream] tunnel log file: %s", log_file_path_.c_str());

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE hFile = CreateFileA(
            log_file_path_.c_str(),
            GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            blog(LOG_ERROR, "[obs-delay-stream] Failed to create log file: %s", log_file_path_.c_str());
            return false;
        }

        STARTUPINFOA si{};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = hFile;
        si.hStdError  = hFile;
        si.hStdInput  = INVALID_HANDLE_VALUE;

        std::string cmdline = "\"" + exe_path + "\" " + args;
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessA(
            exe_path.c_str(),
            cmdline.data(),
            nullptr, nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr, nullptr,
            &si, &pi);
        CloseHandle(hFile); // 親側のハンドルを閉じる

        if (!ok) {
            DWORD err = GetLastError();
            char errmsg[256] = {};
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, err, 0, errmsg, sizeof(errmsg)-1, nullptr);
            blog(LOG_ERROR, "[obs-delay-stream] CreateProcess failed: err=%lu [%s] exe=[%s]",
                 err, errmsg, exe_path.c_str());
            return false;
        }
        proc_handle_   = pi.hProcess;
        thread_handle_ = pi.hThread;
        blog(LOG_INFO, "[obs-delay-stream] Process started, log: %s", log_file_path_.c_str());
        return true;
    }

    void kill_child() {
        if (proc_handle_ != INVALID_HANDLE_VALUE) {
            TerminateProcess(proc_handle_, 0);
            WaitForSingleObject(proc_handle_, 2000);
            CloseHandle(proc_handle_);
            CloseHandle(thread_handle_);
            proc_handle_   = INVALID_HANDLE_VALUE;
            thread_handle_ = INVALID_HANDLE_VALUE;
        }
        // ログファイルを削除
        if (!log_file_path_.empty()) {
            DeleteFileA(log_file_path_.c_str());
            log_file_path_.clear();
        }
    }

    // ============================================================
    // メインループ
    // ============================================================
    void run_loop() {
        bool ok = (type_ == TunnelType::Ngrok)
            ? run_ngrok()
            : run_cloudflared();

        if (!ok && !stop_requested_) {
            state_ = TunnelState::Error;
        }
        kill_child();
    }

    // ---- ngrok ----
    bool run_ngrok() {
        // コマンドライン構築
        char args[512];
        if (!token_.empty())
            snprintf(args, sizeof(args),
                "tcp %d --authtoken %s --log stdout",
                ws_port_, token_.c_str());
        else
            snprintf(args, sizeof(args),
                "tcp %d --log stdout", ws_port_);

        if (!launch_process(exe_path_, args)) {
            set_error("ngrok.exe launch failed. Check path and token.");
            return false;
        }

        // ngrok local API からURLを取得 (最大30秒ポーリング)
        std::string tunnel_url;
        for (int retry = 0; retry < 60 && !stop_requested_; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            tunnel_url = fetch_ngrok_url();
            if (!tunnel_url.empty()) break;
        }
        if (tunnel_url.empty()) {
            set_error("ngrok トンネルURLの取得がタイムアウトしました。\n"
                      "トークンとネットワーク接続を確認してください。");
            return false;
        }

        // tcp:// → ws:// に変換
        if (tunnel_url.rfind("tcp://", 0) == 0)
            tunnel_url = "ws://" + tunnel_url.substr(6);

        set_url(tunnel_url);
        state_ = TunnelState::Running;
        if (on_url_ready) on_url_ready(tunnel_url);

        // プロセス終了を監視
        while (!stop_requested_) {
            DWORD exit_code = STILL_ACTIVE;
            GetExitCodeProcess(proc_handle_, &exit_code);
            if (exit_code != STILL_ACTIVE) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return true;
    }

    // ngrok local API (http://127.0.0.1:4040/api/tunnels) からURLを取得
    std::string fetch_ngrok_url() {
        // 簡易HTTP GETをWinsockで実装
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return "";

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(4040);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        // タイムアウト設定 (1秒)
        DWORD to = 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof(to));

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(s); return "";
        }
        const char* req =
            "GET /api/tunnels HTTP/1.0\r\n"
            "Host: 127.0.0.1:4040\r\n\r\n";
        send(s, req, (int)strlen(req), 0);

        std::string resp;
        char buf[4096];
        int n;
        while ((n = recv(s, buf, sizeof(buf)-1, 0)) > 0) {
            buf[n] = 0; resp += buf;
        }
        closesocket(s);

        // "public_url":"tcp://..." を抜き出す
        auto pos = resp.find("\"public_url\":\"");
        if (pos == std::string::npos) return "";
        pos += 14;
        auto end = resp.find("\"", pos);
        if (end == std::string::npos) return "";
        return resp.substr(pos, end - pos);
    }

    // ---- cloudflared ----
    bool run_cloudflared() {
        char args[256];
        snprintf(args, sizeof(args),
            "tunnel --url http://localhost:%d", ws_port_);

        if (!launch_process(exe_path_, args)) {
            set_error("cloudflared.exe launch failed. Check path and permissions.");
            return false;
        }

        // ファイルからURLを読み取る
        std::string tunnel_url;
        std::string accum;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

        while (!stop_requested_ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            // ファイルから読み取り
            HANDLE hRead = CreateFileA(
                log_file_path_.c_str(),
                GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hRead == INVALID_HANDLE_VALUE) continue;
            // ファイルサイズ確認
            DWORD fsize = GetFileSize(hRead, nullptr);
            if (fsize == 0 || fsize == INVALID_FILE_SIZE) { CloseHandle(hRead); continue; }
            std::string fbuf(fsize, '\0');
            DWORD bytes_read = 0;
            BOOL r = ReadFile(hRead, &fbuf[0], fsize, &bytes_read, nullptr);
            CloseHandle(hRead);
            if (!r || bytes_read == 0) continue;
            fbuf.resize(bytes_read);
            accum = fbuf; // ファイル全体を処理

            // 行単位で解析
            size_t pos = 0;
            while (true) {
                auto nl = accum.find('\n', pos);
                std::string line = accum.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                if (nl == std::string::npos) {
                    // 改行なし: 残りをバッファに残す
                    accum = line;
                    break;
                }
                pos = nl + 1;

                // trycloudflare.com を含む行のみ対象
                if (line.find("trycloudflare.com") == std::string::npos) continue;

                // https:// で始まるURLを探す
                auto hp = line.find("https://");
                if (hp == std::string::npos) continue;

                // URLの終端: スペース・タブ・|・改行・引用符
                auto ep = line.find_first_of(" \t|\r\n\"'", hp);
                std::string candidate = line.substr(hp,
                    ep == std::string::npos ? std::string::npos : ep - hp);

                // trycloudflare.com を含むか再確認
                if (candidate.find("trycloudflare.com") != std::string::npos
                    && candidate.length() > 25) {
                    tunnel_url = candidate;
                    blog(LOG_INFO, "[obs-delay-stream] cloudflared URL found: %s", candidate.c_str());
                    break;
                }
            }
            if (!tunnel_url.empty()) break;
        }

        if (tunnel_url.empty()) {
            set_error("cloudflared トンネルURLの取得がタイムアウトしました。\n"
                      "ネットワーク接続を確認してください。");
            return false;
        }

        // https:// → wss://  /  http:// → ws:// に変換
        if (tunnel_url.rfind("https://", 0) == 0)
            tunnel_url = "wss://" + tunnel_url.substr(8);
        else if (tunnel_url.rfind("http://", 0) == 0)
            tunnel_url = "ws://" + tunnel_url.substr(7);

        set_url(tunnel_url);
        state_ = TunnelState::Running;
        if (on_url_ready) on_url_ready(tunnel_url);

        // プロセス終了監視
        while (!stop_requested_) {
            DWORD exit_code = STILL_ACTIVE;
            GetExitCodeProcess(proc_handle_, &exit_code);
            if (exit_code != STILL_ACTIVE) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return true;
    }

    void set_url(const std::string& u) {
        std::lock_guard<std::mutex> lk(mtx_);
        url_ = u;
    }
    void set_error(const std::string& e) {
        std::lock_guard<std::mutex> lk(mtx_);
        error_ = e;
        state_ = TunnelState::Error;
        if (on_error) on_error(e);
    }

    std::string  log_file_path_;
    TunnelType   type_          = TunnelType::Ngrok;
    std::string  exe_path_;
    std::string  token_;
    int          ws_port_       = 19000;

    std::string  url_;
    std::string  error_;
    mutable std::mutex mtx_;

    std::atomic<TunnelState> state_{TunnelState::Idle};
    std::atomic<bool>        stop_requested_{false};
    std::thread              worker_;

    HANDLE proc_handle_   = INVALID_HANDLE_VALUE;
    HANDLE thread_handle_ = INVALID_HANDLE_VALUE;
    HANDLE pipe_read_     = INVALID_HANDLE_VALUE;
    HANDLE pipe_write_    = INVALID_HANDLE_VALUE;
};
