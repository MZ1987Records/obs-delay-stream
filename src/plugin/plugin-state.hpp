#pragma once

#include "audio/probe-signal.hpp"
#include "core/constants.hpp"
#include "core/delay-buffer.hpp"
#include "model/delay-state.hpp"
#include "network/rtmp-prober.hpp"
#include "network/rtsp-e2e-prober.hpp"
#include "network/stream-router.hpp"
#include "tunnel/tunnel-manager.hpp"
#include "ui/props-refresh.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <obs-module.h>
#include <string>
#include <vector>

/**
 * OBS フィルタのプラグイン本体。状態管理・設定・ヘルパーを含む。
 */
namespace ods::plugin {

	using ods::core::DelayBuffer;
	using ods::core::MAX_SUB_CH;
	using ods::core::WS_PORT;
	using ods::core::DEFAULT_PING_COUNT;
	using ods::core::PLAYBACK_BUFFER_DEFAULT_MS;
	using ods::core::TAB_COUNT;
	using ods::model::DelayState;
	using ods::model::DelaySnapshot;

	using ods::network::LatencyResult;
	using ods::network::RtmpProber;
	using ods::network::RtmpProbeResult;
	using ods::network::RtspE2eProber;
	using ods::network::RtspE2eResult;
	using ods::network::StreamRouter;
	using ods::network::AudioConfig;
	using ods::tunnel::TunnelManager;
	using ods::tunnel::TunnelState;

	/**
	 * スレッドセーフな計測状態管理クラス。
	 *
	 * mutex を外部公開せず、状態更新 API のみ提供する。
	 */
	class MeasureState {
	public:

		/// 計測開始状態へ遷移し、結果とエラーをクリアする。
		void start() {
			std::lock_guard<std::mutex> lk(mtx_);
			measuring_ = true;
			result_    = LatencyResult{};
			last_error_.clear();
		}

		/// 計測完了状態へ遷移し、結果とエラーを反映する。
		void set_result(const LatencyResult &r, const std::string &error = "") {
			std::lock_guard<std::mutex> lk(mtx_);
			result_     = r;
			measuring_  = false;
			applied_    = r.valid;
			last_error_ = error;
		}

		/// 内部状態を初期化する。
		void reset() {
			std::lock_guard<std::mutex> lk(mtx_);
			result_    = LatencyResult{};
			measuring_ = false;
			applied_   = false;
			last_error_.clear();
		}

		/// 計測中か否かを返す。スレッドセーフ。
		bool is_measuring() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return measuring_;
		}

		/// 直近の計測結果を返す。スレッドセーフ。
		LatencyResult result() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return result_;
		}

		/// 結果が適用済みか返す。スレッドセーフ。
		bool is_applied() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return applied_;
		}

		/// 直近のエラー文字列を返す。スレッドセーフ。
		std::string last_error() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return last_error_;
		}

	private:

		mutable std::mutex mtx_;               ///< メンバアクセスを保護する mutex
		LatencyResult      result_;            ///< 直近の計測結果
		bool               measuring_ = false; ///< 計測中フラグ
		bool               applied_   = false; ///< 結果適用済みフラグ
		std::string        last_error_;        ///< 直近のエラー文字列
	};

	/**
	 * RtmpProber と計測結果をまとめた状態クラス。
	 *
	 * `prober.on_result` は外部で設定する。
	 */
	class RtmpMeasureState {
	public:

		RtmpProber prober; ///< on_result は外部（plugin-main.cpp）で設定

		/// 計測結果をスレッドセーフに記録する。
		void apply_result(const RtmpProbeResult &r) {
			std::lock_guard<std::mutex> lk(mtx_);
			result_  = r;
			applied_ = false;
		}

		/// 直近の RTMP 計測結果を返す。スレッドセーフ。
		RtmpProbeResult result() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return result_;
		}

		/// 結果が適用済みか返す。スレッドセーフ。
		bool is_applied() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return applied_;
		}

		/// 適用済みフラグを設定する。スレッドセーフ。
		void set_applied(bool v) {
			std::lock_guard<std::mutex> lk(mtx_);
			applied_ = v;
		}

		/// キャッシュ済み RTMP URL を返す。スレッドセーフ。
		std::string cached_url() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return cached_url_;
		}

		/// RTMP URL をキャッシュする。スレッドセーフ。
		void set_cached_url(const std::string &url) {
			std::lock_guard<std::mutex> lk(mtx_);
			cached_url_ = url;
		}

	private:

		mutable std::mutex mtx_;             ///< メンバアクセスを保護する mutex
		RtmpProbeResult    result_;          ///< 直近の RTMP 計測結果
		bool               applied_ = false; ///< 結果適用済みフラグ
		std::string        cached_url_;      ///< 直近の計測対象 URL
	};

	/**
	 * RtspE2eProber と計測結果をまとめた状態クラス。
	 */
	class RtspE2eMeasureState {
	public:

		RtspE2eProber prober; ///< on_ready/on_result は外部で設定

		/// 計測中か返す。スレッドセーフ。
		bool is_measuring() const { return prober.is_running(); }

		/// 計測結果をスレッドセーフに記録する。
		void apply_result(const RtspE2eResult &r) {
			std::lock_guard<std::mutex> lk(mtx_);
			result_  = r;
			applied_ = false;
		}

		/// 直近の RTSP E2E 計測結果を返す。スレッドセーフ。
		RtspE2eResult result() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return result_;
		}

		/// 結果が適用済みか返す。スレッドセーフ。
		bool is_applied() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return applied_;
		}

		/// 適用済みフラグを設定する。スレッドセーフ。
		void set_applied(bool v) {
			std::lock_guard<std::mutex> lk(mtx_);
			applied_ = v;
		}

		/// キャッシュ済み RTSP URL を返す。スレッドセーフ。
		std::string cached_url() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return cached_url_;
		}

		/// RTSP URL をキャッシュする。スレッドセーフ。
		void set_cached_url(const std::string &url) {
			std::lock_guard<std::mutex> lk(mtx_);
			cached_url_ = url;
		}

		/// 計測進捗を更新する。スレッドセーフ。
		void set_progress(int completed, int total) {
			completed_sets_.store(completed, std::memory_order_relaxed);
			total_sets_.store(total, std::memory_order_relaxed);
		}

		int completed_sets() const { return completed_sets_.load(std::memory_order_relaxed); }
		int total_sets() const { return total_sets_.load(std::memory_order_relaxed); }

		/// 直近のエラー文字列を返す。スレッドセーフ。
		std::string last_error() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return last_error_;
		}

		/// エラー文字列を設定する。スレッドセーフ。
		void set_last_error(const std::string &err) {
			std::lock_guard<std::mutex> lk(mtx_);
			last_error_ = err;
		}

		/// 計測をキャンセルし進捗をリセットする。
		void cancel() {
			prober.cancel();
			completed_sets_.store(0, std::memory_order_relaxed);
			total_sets_.store(0, std::memory_order_relaxed);
		}

	private:

		mutable std::mutex mtx_;               ///< メンバアクセスを保護する mutex
		RtspE2eResult      result_;            ///< 直近の RTSP E2E 計測結果
		bool               applied_ = false;   ///< 結果適用済みフラグ
		std::string        cached_url_;        ///< 直近の計測対象 URL
		std::string        last_error_;        ///< 直近のエラー文字列
		std::atomic<int>   completed_sets_{0}; ///< 完了セット数
		std::atomic<int>   total_sets_{0};     ///< 総セット数
	};

	struct DelayStreamData;

	/**
	 * チャンネル番号付きコールバック引数。
	 */
	struct SubChannelCtx {
		DelayStreamData *d;  ///< 対象データ
		int              ch; ///< 0-indexed チャンネル番号
	};

	/**
	 * タブ番号付きコールバック引数。
	 */
	struct TabCtx {
		DelayStreamData *d;   ///< 対象データ
		int              tab; ///< 0-indexed タブ番号
	};

	/**
	 * 更新確認の状態種別。
	 */
	enum class UpdateCheckStatus {
		Unknown = 0,     ///< 未確認
		Checking,        ///< 確認中
		UpToDate,        ///< 最新
		UpdateAvailable, ///< 更新あり
		Error,           ///< 取得失敗
	};

	/**
	 * 更新確認状態を保持するクラス。
	 *
	 * `status` / `inflight` はアトミック運用のため public、
	 * 文字列フィールドはミューテックス保護アクセサ経由で扱う。
	 */
	class UpdateCheckState {
	public:

		std::atomic<UpdateCheckStatus> status{UpdateCheckStatus::Unknown}; ///< 更新確認の現在状態
		std::atomic<bool>              inflight{false};                    ///< HTTP リクエスト処理中フラグ

		/// 文字列フィールドをまとめて書き込む。スレッドセーフ。
		void set_strings(const std::string &version, const std::string &url, const std::string &error) {
			std::lock_guard<std::mutex> lk(mtx_);
			latest_version_ = version;
			latest_url_     = url;
			error_          = error;
		}

		/// 取得した最新バージョン文字列を返す。スレッドセーフ。
		std::string latest_version() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return latest_version_;
		}

		/// 取得した最新版ダウンロード URL を返す。スレッドセーフ。
		std::string latest_url() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return latest_url_;
		}

		/// 取得失敗時のエラー文字列を返す。スレッドセーフ。
		std::string error() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return error_;
		}

	private:

		mutable std::mutex mtx_;            ///< 文字列フィールドを保護する mutex
		std::string        latest_version_; ///< 取得した最新バージョン文字列
		std::string        latest_url_;     ///< 取得した最新版ダウンロード URL
		std::string        error_;          ///< 取得失敗時のエラー文字列
	};

	/**
	 * OBS フィルタ全体の実行状態。
	 */
	struct DelayStreamData {
		obs_source_t           *context               = nullptr; ///< OBS フィルタコンテキスト
		bool                    is_duplicate_instance = false;   ///< 二重起動されたインスタンスか
		bool                    has_settings_mismatch = false;   ///< 保存設定の整合性エラーで警告専用モードか
		bool                    owns_singleton_slot   = false;   ///< シングルトンスロットを確保しているか
		uint64_t                singleton_generation  = 0;       ///< シングルトン世代番号（重複判定用）
		std::atomic<bool>       destroying{false};               ///< デストラクタ実行中フラグ
		std::atomic<bool>       enabled{true};                   ///< フィルタ有効フラグ
		std::atomic<bool>       ws_send_enabled{true};           ///< WebSocket 音声送信有効フラグ
		std::atomic<bool>       inject_impulse{false};           ///< RTSP E2E 計測用プローブ注入フラグ
		std::atomic<bool>       probe_mute_active{false};        ///< ミュートモード計測中フラグ（入力音声をミュート）
		ods::audio::ProbeSignal probe_signal;                    ///< RTSP E2E 計測用チャープ信号

		/// 非同期タスクがフィルタ生存中かチェックするトークン
		std::shared_ptr<std::atomic<bool>> life_token =
			std::make_shared<std::atomic<bool>>(true);

		mutable std::mutex                        stream_id_mtx;                                   ///< stream_id / host_ip / auto_ip を保護する mutex
		std::string                               stream_id;                                       ///< 配信 ID（例: "myshow2024"）
		std::string                               host_ip;                                         ///< 接続先ホスト IP（手動設定 or auto_ip から解決）
		std::string                               auto_ip;                                         ///< 自動取得したローカル IP
		std::atomic<int>                          ws_port{WS_PORT};                                ///< WebSocket ポート番号
		std::atomic<int>                          ping_count_setting{DEFAULT_PING_COUNT};          ///< WebSocket 計測の ping 送信回数
		int                                       playback_buffer_ms = PLAYBACK_BUFFER_DEFAULT_MS; ///< 受信側再生バッファ量 (ms)
		DelayState                                delay;                                           ///< ディレイ計算の入力値（MVVM Model 層）
		std::atomic<int>                          active_tab{0};                                   ///< 設定UIの現在タブ（0-indexed）
		DelayBuffer                               master_buf;                                      ///< マスターチャンネルのディレイバッファ
		RtmpMeasureState                          rtmp_measure;                                    ///< RTMP 計測状態
		RtspE2eMeasureState                       rtsp_e2e_measure;                                ///< RTSP E2E 計測状態
		StreamRouter                              router;                                          ///< WebSocket ルーター
		std::atomic<bool>                         router_running{false};                           ///< WebSocket ルーター起動中フラグ
		std::atomic<bool>                         auto_measure_enabled{false};                     ///< 接続時の自動計測フラグ
		std::array<std::atomic<bool>, MAX_SUB_CH> auto_measure_pending{};                          ///< チャンネル別自動計測予約中フラグ

		// WS 一括計測の進捗トラッキング
		struct WsBatchProgress {
			std::atomic<int> ping_sent_count{0};  ///< 送信済み ping 数
			std::atomic<int> ping_total_count{0}; ///< 送信予定の ping 総数
			void reset() {
				ping_sent_count.store(0, std::memory_order_relaxed);
				ping_total_count.store(0, std::memory_order_relaxed);
			}
		};

		WsBatchProgress ws_batch_progress; ///< WS 一括計測の進捗

		/// サブチャンネルボタンのコールバック引数
		std::array<SubChannelCtx, MAX_SUB_CH> sub_btn_ctx;
		/// タブ選択ボタンのコールバック引数
		std::array<TabCtx, TAB_COUNT> tab_btn_ctx;

		/// サブチャンネルの音声バッファと計測状態。
		/// ディレイ計算の入力値（measured_ms, ws_measured, offset_ms）は delay.channels[] に移動済み。
		struct SubChannel {
			DelayBuffer  buf;     ///< 音声ディレイバッファ
			MeasureState measure; ///< 計測状態
		};

		std::array<SubChannel, MAX_SUB_CH> sub_channels; ///< サブチャンネルの状態配列

		TunnelManager     tunnel;                                     ///< cloudflared トンネルマネージャー
		std::atomic<bool> manual_cloudflared_download_running{false}; ///< cloudflared 手動ダウンロード実行中フラグ
		std::atomic<bool> manual_ffmpeg_download_running{false};      ///< ffmpeg 手動ダウンロード実行中フラグ
		uint32_t          sample_rate = 48000;                        ///< 音声サンプルレート (Hz)
		uint32_t          channels    = 2;                            ///< 音声チャンネル数
		bool              initialized = false;                        ///< 音声処理の初期化完了フラグ
		std::atomic<bool> create_done{false};                         ///< obs_source_create 完了フラグ
		std::atomic<int>  get_props_depth{0};                         ///< obs_get_properties の再入深度

		/// 最後にレンダリングした音声同期オフセット (ns)
		std::atomic<int64_t> last_rendered_audio_sync_offset_ns{INT64_MIN};

		UpdateCheckState   update_check;                          ///< 更新確認状態
		std::atomic<bool>  sid_autofill_guard{false};             ///< stream_id 自動補完の二重実行防止フラグ
		std::atomic<bool>  rtmp_url_auto{true};                   ///< RTMP URL 自動補完を有効にするフラグ
		bool               prev_stream_id_has_user_value = false; ///< 直前の stream_id にユーザー設定値があったか（デフォルトリセット検出用）
		std::vector<float> work_buf;                              ///< 音声処理用ワークバッファ

		/// 警告専用モード（通常機能を無効化し、プラグイングループのみ表示）か。
		bool is_warning_only_instance() const {
			return is_duplicate_instance || has_settings_mismatch;
		}

		/// stream_id をスレッドセーフに取得する。
		std::string get_stream_id() const {
			std::lock_guard<std::mutex> lk(stream_id_mtx);
			return stream_id;
		}

		/// stream_id をスレッドセーフに設定する。
		void set_stream_id(const std::string &id) {
			std::lock_guard<std::mutex> lk(stream_id_mtx);
			stream_id = id;
		}

		/// host_ip をスレッドセーフに取得する。
		std::string get_host_ip() const {
			std::lock_guard<std::mutex> lk(stream_id_mtx);
			return host_ip;
		}

		/// manual_override が空なら auto_ip を使う。スレッドセーフ。
		void set_host_ip(const char *manual_override) {
			std::lock_guard<std::mutex> lk(stream_id_mtx);
			host_ip = (manual_override && *manual_override) ? manual_override : auto_ip;
		}

		/// create_done / destroying / get_props_depth を参照してプロパティ再描画を要求する。
		void request_props_refresh(const char *reason = nullptr) const {
			ods::ui::props_refresh_request(
				context,
				create_done.load(std::memory_order_acquire),
				destroying.load(std::memory_order_acquire),
				get_props_depth.load(std::memory_order_acquire),
				reason);
		}

		/// active_tab を正規化して保持する。
		void set_active_tab(int tab) {
			if (tab < 0 || tab >= TAB_COUNT) tab = 0;
			active_tab.store(tab, std::memory_order_release);
		}

		/// 現在の active_tab を返す。
		int get_active_tab() const {
			int tab = active_tab.load(std::memory_order_acquire);
			if (tab < 0 || tab >= TAB_COUNT) tab = 0;
			return tab;
		}

		/// 指定タブが表示中のときだけプロパティ再描画を要求する。
		void request_props_refresh_for_tabs(std::initializer_list<int> tabs,
											const char                *reason = nullptr) const {
			const int active = get_active_tab();
			for (int tab : tabs) {
				if (tab == active) {
					request_props_refresh(reason);
					return;
				}
			}
		}

		/// いずれかのサブチャンネルで WS 計測が実行中かを返す。
		bool ws_any_measuring() const {
			const int n = delay.sub_ch_count;
			for (int i = 0; i < n; ++i) {
				if (sub_channels[i].measure.is_measuring())
					return true;
			}
			return false;
		}
	};

} // namespace ods::plugin
