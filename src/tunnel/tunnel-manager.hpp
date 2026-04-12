#pragma once

/**
 * cloudflared を子プロセスとして起動し、
 * 取得したパブリック URL (wss://) を返す。
 */

// HANDLE メンバー利用に必要な最小限の Windows ヘッダーだけを有効化する。
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

namespace ods::tunnel {

	using ods::core::WS_PORT;
	using ods::core::CLOUDFLARED_URL_TIMEOUT_S;
	using ods::core::CLOUDFLARED_POLL_INTV_MS;
	using ods::core::TUNNEL_KILL_TIMEOUT_MS;

	/**
	 * トンネル実行状態。
	 */
	enum class TunnelState {
		Idle,     ///< 未起動
		Starting, ///< 起動中（URL 取得待ち）
		Running,  ///< 動作中
		Stopped,  ///< 停止済み
		Error,    ///< エラー
	};

	/**
	 * cloudflared プロセスを管理して公開 URL を取得する。
	 */
	class TunnelManager {
	public:

		std::function<void(const std::string &url)>    on_url_ready;      ///< URL 取得完了コールバック
		std::function<void(const std::string &reason)> on_error;          ///< エラー通知コールバック
		std::function<void()>                          on_stopped;        ///< 停止通知コールバック
		std::function<void(bool downloading)>          on_download_state; ///< cloudflared ダウンロード状態通知

		/// インスタンスを初期化する。
		TunnelManager() = default;
		/// 管理中の child process を停止して破棄する。
		~TunnelManager();

		/// 自動配置先に cloudflared.exe が存在する場合のみパスを返す。
		static bool get_auto_cloudflared_path_if_exists(std::string &out);
		/// 自動配置先へ cloudflared.exe を配置し、利用可能な絶対パスを返す。
		static bool ensure_auto_cloudflared_path(std::string &out, std::string &err);
		/// パスを `%LOCALAPPDATA%` 基準の環境変数表現へ正規化する。
		static std::string to_localappdata_env_path(const std::string &path);
		/// cloudflared の自動ダウンロード処理が進行中かを返す。
		bool cloudflared_downloading() const;

		/// cloudflared を起動する。
		/// @param exe_path cloudflared.exe の解決指定（"auto" / "%PATH%" / 絶対パス）
		/// @param ws_port  転送先 WebSocket ポート
		/// @param token    Named Tunnel 用トークン（空なら Quick Tunnel）
		/// @param domain   Named Tunnel 用ドメイン（空なら Quick Tunnel）
		bool start(const std::string &exe_path, int ws_port,
				   const std::string &token = "", const std::string &domain = "");
		/// 起動中の cloudflared を停止する。
		void stop();

		/// 現在のトンネル状態を返す。
		TunnelState state() const;
		/// 取得済み公開 URL（未取得時は空）を返す。
		std::string url() const;
		/// 最後に発生したエラー文字列を返す。
		std::string error() const;

		std::string make_ch_url(const std::string &stream_id, int ch_1idx) const; ///< 各チャンネルの受信 URL（wss://host/stream_id/ch）を生成する

	private:

		std::string log_file_path_;      ///< cloudflared ログ出力先
		std::string exe_path_;           ///< 実行中に使用する cloudflared パス
		std::string requested_exe_path_; ///< ユーザー指定の元パス
		int         ws_port_ = WS_PORT;  ///< トンネル転送先 WebSocket ポート
		std::string token_;              ///< Named Tunnel 用トークン
		std::string named_domain_;       ///< Named Tunnel 用ドメイン

		std::string        url_;               ///< 取得済み公開 URL
		std::string        error_;             ///< 公開用エラー文字列
		std::string        last_launch_error_; ///< 起動失敗時の内部エラー詳細
		mutable std::mutex mtx_;               ///< URL/エラー等の排他制御

		std::atomic<TunnelState> state_{TunnelState::Idle};       ///< トンネル状態
		std::atomic<bool>        stop_requested_{false};          ///< 停止要求フラグ
		std::atomic<bool>        downloading_cloudflared_{false}; ///< ダウンロード進行中フラグ
		std::thread              worker_;                         ///< 監視ワーカースレッド

		std::mutex proc_mtx_;                             ///< プロセス HANDLE の排他制御
		HANDLE     proc_handle_   = INVALID_HANDLE_VALUE; ///< cloudflared プロセス HANDLE
		HANDLE     thread_handle_ = INVALID_HANDLE_VALUE; ///< cloudflared スレッド HANDLE

		/// ダウンロード中フラグを更新し、必要に応じて通知する。
		void set_cloudflared_downloading(bool v);
		/// 公開 URL を更新する。
		void set_url(const std::string &u);
		/// エラー文字列を更新する。
		void set_error(const std::string &e);
		/// child process を起動する。
		bool launch_process(const std::string &exe_path, const std::string &args);
		/// 起動済み child process を終了させる。
		void kill_child();
		/// 監視スレッド本体。
		void run_loop();
		/// cloudflared 起動と URL 取得処理を実行する。
		bool run_cloudflared();
		/// Quick Tunnel ログから公開 URL を検出する。
		bool poll_quick_tunnel_url();
		/// Named Tunnel ログから接続確立を検出する。
		bool poll_named_tunnel_ready();
		/// cloudflared プロセスの終了を監視する。
		void monitor_process_until_exit();

		/// `%LOCALAPPDATA%` の実体パスを取得する。
		static std::string get_local_appdata_dir();
		/// 一時ディレクトリの実体パスを取得する。
		static std::string get_temp_dir();
		/// 許可済み環境変数のみ展開用に取得する。
		static std::string get_allowed_env_value(const std::string &name);
		/// 許可済み環境変数だけを展開したパスを返す。
		static std::string expand_allowed_env_vars(const std::string &path);
		/// ファイル存在確認。
		static bool file_exists(const std::string &path);
		/// 既定の cloudflared 配置先パスを返す。
		static std::string get_default_cloudflared_path();
		/// cloudflared を指定パスへダウンロードする。
		static bool download_cloudflared(const std::string &path, std::string &err);
		/// 要求文字列から利用可能な cloudflared 実行パスを解決する。
		static bool resolve_cloudflared_path(const std::string &requested,
											 std::string       &out,
											 std::string       &err);
		/// `%PATH%` から cloudflared.exe を探索する。
		static bool search_path_cloudflared(std::string &out);
		/// cloudflared 出力ログから要約エラーを抽出する。
		static bool extract_cloudflared_error(const std::string &text, std::string &out);
	};

} // namespace ods::tunnel
