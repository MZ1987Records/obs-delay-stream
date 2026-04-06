#pragma once

/*
 * tunnel-manager.hpp
 *
 * cloudflared を子プロセスとして起動し、
 * 取得したパブリックURL (wss://) を返す。
 */

// HANDLE メンバーのために最小限の Windows ヘッダーが必要
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/constants.hpp"

// ============================================================
// TunnelState
// ============================================================
enum class TunnelState {
	Idle,     // 未起動
	Starting, // 起動中 (URL取得待ち)
	Running,  // 動作中
	Stopped,  // 停止済み
	Error,    // エラー
};

// ============================================================
// TunnelManager
// ============================================================
class TunnelManager {
	public:
	// URL取得完了 / エラー コールバック
	std::function<void(const std::string &url)>    on_url_ready;
	std::function<void(const std::string &reason)> on_error;
	std::function<void()>                          on_stopped;
	std::function<void(bool downloading)>          on_download_state;

	static bool        get_auto_cloudflared_path_if_exists(std::string &out);
	static std::string to_localappdata_env_path(const std::string &path);
	bool               cloudflared_downloading() const;

	TunnelManager() = default;
	~TunnelManager();

	// exe_path: cloudflared.exe のフルパス（"auto" または空で自動取得）
	bool start(const std::string &exe_path, int ws_port);
	void stop();

	TunnelState state() const;
	std::string url() const;
	std::string error() const;

	// 各chの受信URL生成（wss://host/stream_id/ch）
	std::string make_ch_url(const std::string &stream_id, int ch_1idx) const;

	private:
	void set_cloudflared_downloading(bool v);
	void set_url(const std::string &u);
	void set_error(const std::string &e);
	bool launch_process(const std::string &exe_path, const std::string &args);
	void kill_child();
	void run_loop();
	bool run_cloudflared();

	static std::string get_local_appdata_dir();
	static std::string get_temp_dir();
	static std::string get_allowed_env_value(const std::string &name);
	static std::string expand_allowed_env_vars(const std::string &path);
	static bool        file_exists(const std::string &path);
	static std::string get_default_cloudflared_path();
	static bool        download_cloudflared(const std::string &path, std::string &err);
	static bool        resolve_cloudflared_path(const std::string &requested,
												std::string       &out,
												std::string       &err);
	static bool        extract_cloudflared_error(const std::string &text, std::string &out);

	std::string log_file_path_;
	std::string exe_path_;
	std::string requested_exe_path_;
	int         ws_port_ = WS_PORT;

	std::string        url_;
	std::string        error_;
	std::string        last_launch_error_;
	mutable std::mutex mtx_;

	std::atomic<TunnelState> state_{TunnelState::Idle};
	std::atomic<bool>        stop_requested_{false};
	std::atomic<bool>        downloading_cloudflared_{false};
	std::thread              worker_;

	std::mutex proc_mtx_;
	HANDLE     proc_handle_   = INVALID_HANDLE_VALUE;
	HANDLE     thread_handle_ = INVALID_HANDLE_VALUE;
};
