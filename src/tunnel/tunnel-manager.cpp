/*
 * tunnel-manager.cpp
 *
 * cloudflared を子プロセスとして起動し、
 * 取得したパブリックURL (wss://) を返す。
 *
 * cloudflared:
 *   起動: cloudflared.exe tunnel --url http://localhost:<WS_PORT>
 *   URL取得: stderr に出力される "https://xxxx.trycloudflare.com" を捕捉
 *   URL形式: https:// → wss:// に変換
 *   ※ 無料・認証不要・固定でない（起動毎にURLが変わる）
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ShlObj.h>
#include <urlmon.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Urlmon.lib")

#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>
#include <chrono>
#include <algorithm>

#include <obs-module.h>

#include "tunnel/tunnel-manager.hpp"

// ============================================================
// Destructor / lifecycle
// ============================================================

TunnelManager::~TunnelManager() {
    stop();
}

bool TunnelManager::start(const std::string& exe_path, int ws_port) {
    if (state_.load() == TunnelState::Running ||
        state_.load() == TunnelState::Starting) return false;

    ws_port_  = ws_port;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        url_   = "";
        error_ = "";
    }

    requested_exe_path_ = exe_path;
    bool is_auto = exe_path.empty() || _stricmp(exe_path.c_str(), "auto") == 0;
    if (!is_auto) {
        std::string expanded = expand_allowed_env_vars(exe_path);
        std::string resolved;
        std::string err;
        if (!resolve_cloudflared_path(expanded, resolved, err)) {
            set_error(err);
            return false;
        }
        exe_path_ = resolved;
        set_cloudflared_downloading(false);
    } else {
        std::string auto_path = get_default_cloudflared_path();
        if (file_exists(auto_path)) {
            exe_path_ = auto_path;
            set_cloudflared_downloading(false);
        } else {
            exe_path_.clear();
            set_cloudflared_downloading(true);
        }
    }

    state_ = TunnelState::Starting;

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this]() { run_loop(); });
    return true;
}

void TunnelManager::stop() {
    stop_requested_ = true;
    kill_child();
    if (worker_.joinable()) worker_.join();
    state_ = TunnelState::Stopped;
    set_cloudflared_downloading(false);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        url_.clear();
        error_.clear();
    }
    stop_requested_ = false;
    // コールバックは呼ばない（ds_destroy時にnull化されている可能性がある）
    auto cb = on_stopped;
    if (cb) cb();
}

TunnelState TunnelManager::state() const { return state_; }
std::string TunnelManager::url()   const { std::lock_guard<std::mutex> lk(mtx_); return url_; }
std::string TunnelManager::error() const { std::lock_guard<std::mutex> lk(mtx_); return error_; }

std::string TunnelManager::make_ch_url(const std::string& stream_id, int ch_1idx) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (url_.empty() || stream_id.empty()) return "";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/%s/%d",
             url_.c_str(), stream_id.c_str(), ch_1idx);
    return buf;
}

bool TunnelManager::cloudflared_downloading() const {
    return downloading_cloudflared_.load();
}

// ============================================================
// Static helpers (path / env resolution)
// ============================================================

std::string TunnelManager::get_local_appdata_dir() {
    char base[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) {
        return std::string(base);
    }
    return "";
}

std::string TunnelManager::get_temp_dir() {
    char tmp_path[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, tmp_path);
    if (n > 0 && n < MAX_PATH) return std::string(tmp_path);
    return "";
}

std::string TunnelManager::get_allowed_env_value(const std::string& name) {
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(),
        [](unsigned char c){ return (char)std::toupper(c); });
    if (upper == "LOCALAPPDATA") return get_local_appdata_dir();
    if (upper == "TEMP" || upper == "TMP") return get_temp_dir();
    return "";
}

std::string TunnelManager::expand_allowed_env_vars(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    size_t i = 0;
    while (i < path.size()) {
        size_t p = path.find('%', i);
        if (p == std::string::npos || p + 1 >= path.size()) {
            out.append(path.substr(i));
            break;
        }
        size_t q = path.find('%', p + 1);
        if (q == std::string::npos) {
            out.append(path.substr(i));
            break;
        }
        out.append(path.substr(i, p - i));
        std::string var = path.substr(p + 1, q - p - 1);
        std::string val = get_allowed_env_value(var);
        if (!val.empty()) {
            out.append(val);
        } else {
            out.append(path.substr(p, q - p + 1));
        }
        i = q + 1;
    }
    return out;
}

bool TunnelManager::file_exists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string TunnelManager::get_default_cloudflared_path() {
    std::string base = get_local_appdata_dir();
    if (!base.empty()) {
        std::string dir = base + "\\obs-delay-stream\\bin";
        SHCreateDirectoryExA(nullptr, dir.c_str(), nullptr);
        return dir + "\\cloudflared.exe";
    }
    std::string tmp_path = get_temp_dir();
    if (tmp_path.empty()) tmp_path = "C:\\Temp\\";
    std::string dir = tmp_path + "obs-delay-stream\\bin";
    SHCreateDirectoryExA(nullptr, dir.c_str(), nullptr);
    return dir + "\\cloudflared.exe";
}

bool TunnelManager::get_auto_cloudflared_path_if_exists(std::string& out) {
    out = get_default_cloudflared_path();
    return file_exists(out);
}

std::string TunnelManager::to_localappdata_env_path(const std::string& path) {
    std::string base = get_local_appdata_dir();
    if (base.empty()) return path;
    std::string base_norm = base;
    if (!base_norm.empty() && base_norm.back() != '\\' && base_norm.back() != '/') {
        base_norm.push_back('\\');
    }
    if (path.size() >= base_norm.size() &&
        _strnicmp(path.c_str(), base_norm.c_str(), base_norm.size()) == 0) {
        return std::string("%LocalAppData%\\") + path.substr(base_norm.size());
    }
    return path;
}

bool TunnelManager::download_cloudflared(const std::string& path, std::string& err) {
    static const char* kUrl =
        "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe";
    HRESULT hr = URLDownloadToFileA(nullptr, kUrl, path.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        char buf[64] = {};
        snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)hr);
        err = std::string("cloudflared の自動ダウンロードに失敗しました (") + buf +
              ")。ネットワーク接続を確認するか、手動でパスを指定してください。";
        return false;
    }
    if (!file_exists(path)) {
        err = "cloudflared のダウンロード後にファイルが見つかりませんでした。";
        return false;
    }
    return true;
}

bool TunnelManager::resolve_cloudflared_path(const std::string& requested,
                                             std::string& out,
                                             std::string& err)
{
    bool is_auto = requested.empty() || _stricmp(requested.c_str(), "auto") == 0;
    if (!is_auto) {
        std::string expanded = expand_allowed_env_vars(requested);
        if (file_exists(expanded)) {
            out = expanded;
            return true;
        }
        err = "cloudflared.exe が見つかりません。パスを確認するか、空欄にして自動取得を使用してください。";
        return false;
    }

    out = get_default_cloudflared_path();
    if (file_exists(out)) return true;

    blog(LOG_INFO, "[obs-delay-stream] cloudflared auto-download: %s", out.c_str());
    return download_cloudflared(out, err);
}

// ============================================================
// プロセス管理
// ============================================================

bool TunnelManager::launch_process(const std::string& exe_path, const std::string& args) {
    last_launch_error_.clear();
    // ファイルリダイレクト方式（パイプバッファ詰まりを完全回避）
    char tmp_path[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp_path);
    if (tmp_path[0] == 0) strncpy(tmp_path, "C:\\Temp\\", MAX_PATH - 1);
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
        last_launch_error_ = "ログファイル作成に失敗しました。";
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
        char buf[512] = {};
        snprintf(buf, sizeof(buf), "CreateProcess failed: err=%lu [%s]", err, errmsg);
        last_launch_error_ = buf;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(proc_mtx_);
        proc_handle_   = pi.hProcess;
        thread_handle_ = pi.hThread;
    }
    blog(LOG_INFO, "[obs-delay-stream] Process started, log: %s", log_file_path_.c_str());
    return true;
}

void TunnelManager::kill_child() {
    std::lock_guard<std::mutex> lk(proc_mtx_);
    if (proc_handle_ != INVALID_HANDLE_VALUE) {
        TerminateProcess(proc_handle_, 0);
        WaitForSingleObject(proc_handle_, TUNNEL_KILL_TIMEOUT_MS);
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

void TunnelManager::run_loop() {
    if (exe_path_.empty()) {
        std::string resolved;
        std::string err;
        set_cloudflared_downloading(true);
        if (!resolve_cloudflared_path(requested_exe_path_, resolved, err)) {
            set_cloudflared_downloading(false);
            set_error(err);
            if (!stop_requested_) state_ = TunnelState::Error;
            return;
        }
        exe_path_ = resolved;
        set_cloudflared_downloading(false);
    }
    bool ok = run_cloudflared();

    if (!ok && !stop_requested_) {
        state_ = TunnelState::Error;
    }
    kill_child();
}

bool TunnelManager::run_cloudflared() {
    char args[256];
    snprintf(args, sizeof(args),
        "tunnel --url http://localhost:%d", ws_port_);

    if (!launch_process(exe_path_, args)) {
        if (!last_launch_error_.empty())
            set_error(std::string("cloudflared.exe launch failed: ") + last_launch_error_);
        else
            set_error("cloudflared.exe launch failed. Check path and permissions.");
        return false;
    }

    // ファイルからURLを読み取る
    std::string tunnel_url;
    std::string accum;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(CLOUDFLARED_URL_TIMEOUT_S);

    while (!stop_requested_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(CLOUDFLARED_POLL_INTV_MS));

        HANDLE hRead = CreateFileA(
            log_file_path_.c_str(),
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hRead == INVALID_HANDLE_VALUE) continue;

        DWORD fsize = GetFileSize(hRead, nullptr);
        if (fsize == 0 || fsize == INVALID_FILE_SIZE) { CloseHandle(hRead); continue; }
        std::string fbuf(fsize, '\0');
        DWORD bytes_read = 0;
        BOOL r = ReadFile(hRead, &fbuf[0], fsize, &bytes_read, nullptr);
        CloseHandle(hRead);
        if (!r || bytes_read == 0) continue;
        fbuf.resize(bytes_read);
        accum = fbuf;

        // エラーログ検出
        {
            std::string emsg;
            if (extract_cloudflared_error(accum, emsg)) {
                set_error(emsg);
                return false;
            }
        }

        // 行単位で解析
        size_t pos = 0;
        while (true) {
            auto nl = accum.find('\n', pos);
            std::string line = accum.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (nl == std::string::npos) {
                accum = line;
                break;
            }
            pos = nl + 1;

            if (line.find("trycloudflare.com") == std::string::npos) continue;

            auto hp = line.find("https://");
            if (hp == std::string::npos) continue;

            auto ep = line.find_first_of(" \t|\r\n\"'", hp);
            std::string candidate = line.substr(hp,
                ep == std::string::npos ? std::string::npos : ep - hp);

            if (candidate.find("trycloudflare.com") != std::string::npos
                && candidate.length() > 25) {
                std::string host;
                if (candidate.rfind("https://", 0) == 0) {
                    size_t he = candidate.find_first_of("/:?", 8);
                    host = candidate.substr(8, he == std::string::npos ? std::string::npos : he - 8);
                } else if (candidate.rfind("http://", 0) == 0) {
                    size_t he = candidate.find_first_of("/:?", 7);
                    host = candidate.substr(7, he == std::string::npos ? std::string::npos : he - 7);
                }
                if (!_stricmp(host.c_str(), "api.trycloudflare.com")) {
                    continue; // API URLは除外
                }
                tunnel_url = candidate;
                blog(LOG_INFO, "[obs-delay-stream] cloudflared URL found: %s", candidate.c_str());
                break;
            }
        }
        if (!tunnel_url.empty()) break;

        // プロセス終了チェック
        DWORD exit_code = STILL_ACTIVE;
        {
            std::lock_guard<std::mutex> lk(proc_mtx_);
            if (proc_handle_ != INVALID_HANDLE_VALUE)
                GetExitCodeProcess(proc_handle_, &exit_code);
            else
                break;
        }
        if (exit_code != STILL_ACTIVE) {
            set_error("cloudflared が終了しました。ログを確認してください。");
            return false;
        }
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
        {
            std::lock_guard<std::mutex> lk(proc_mtx_);
            if (proc_handle_ != INVALID_HANDLE_VALUE)
                GetExitCodeProcess(proc_handle_, &exit_code);
            else
                break;
        }
        if (exit_code != STILL_ACTIVE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(CLOUDFLARED_POLL_INTV_MS));
    }
    return true;
}

// ============================================================
// 内部状態ヘルパー
// ============================================================

void TunnelManager::set_cloudflared_downloading(bool v) {
    bool prev = downloading_cloudflared_.exchange(v);
    if (prev != v && on_download_state) on_download_state(v);
}

void TunnelManager::set_url(const std::string& u) {
    std::lock_guard<std::mutex> lk(mtx_);
    url_ = u;
}

void TunnelManager::set_error(const std::string& e) {
    std::lock_guard<std::mutex> lk(mtx_);
    error_ = e;
    state_ = TunnelState::Error;
    if (on_error) on_error(e);
}

// ============================================================
// cloudflaredエラーログ検出
// ============================================================

bool TunnelManager::extract_cloudflared_error(const std::string& text, std::string& out) {
    auto contains = [](const std::string& s, const char* sub) {
        return s.find(sub) != std::string::npos;
    };

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        if (line.empty()) continue;

        bool is_err_line =
            contains(line, "failed to request quick Tunnel") ||
            contains(line, " ERR ")                           ||
            contains(line, "\tERR\t")                         ||
            (contains(line, "failed to") &&
             (contains(line, "tunnel") || contains(line, "Tunnel"))) ||
            (contains(line, "error") &&
             (contains(line, "tunnel") || contains(line, "Tunnel")));

        if (is_err_line) {
            out = std::string("cloudflared エラー: ") + line;
            return true;
        }
    }
    return false;
}
