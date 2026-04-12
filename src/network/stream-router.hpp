#pragma once

/**
 * 単一ポートで複数チャンネルをルーティングする WebSocket サーバー。
 *
 * 接続 URL: `ws://[IP]:[PORT]/[配信ID]/[ch番号(1-20)]`
 * 例: `ws://192.168.1.10:19000/myshow2024/1`
 *
 * 配信 ID が一致するチャンネルへのみ音声・ping を配信し、
 * 異なる配信 ID は別セッションとして分離する。
 */

#define ASIO_STANDALONE
#define _WEBSOCKETPP_CPP11_STL_

// SIMDe による標準整数型の再定義を防ぐ。
#ifndef SIMDE_NO_NATIVE
#define SIMDE_NO_NATIVE
#endif

// websocketpp/asio より先に Windows ヘッダーを読み込む。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/constants.hpp"
#include "network/opus-encoder.hpp"
#include "network/stream-router-types.hpp"
#include "network/stream-router-utils.hpp"

/**
 * WebSocket 配信と RTT 計測など、ネットワーク通信系コンポーネントを提供する。
 */
namespace ods::network {

	/**
	 * 全チャンネルを 1 ポートで管理し、配信 ID + チャンネル番号でルーティングする。
	 */
	class StreamRouter {
	public:

		/// 接続数変化通知（sid, ch_0idx, count）
		ConnChangeCallback on_conn_change;

		/// 計測結果通知（sid, ch_0idx, result）
		LatencyCallback on_any_latency_result;

		/// ping 送信通知（sid, ch_0idx, seq）
		std::function<void(const std::string &, int, int)> on_any_ping_sent;

		/// リッスン中のポート番号を返す。
		uint16_t port() const { return port_; }

		/// サーバーが稼働中かを返す。
		bool is_running() const { return running_; }

		/// 既定状態で構築する。
		StreamRouter() = default;

		/// 稼働中サーバーを停止して破棄する。
		~StreamRouter() { stop(); }

		bool start(uint16_t port);                          ///< サーバーを起動する
		void stop();                                        ///< サーバーを停止する
		void set_stream_id(const std::string &id);          ///< 配信 ID を設定する
		std::string stream_id() const;                      ///< 現在の配信 ID を返す
		void set_active_channels(int n);                    ///< 有効チャンネル数を設定する
		int active_channels() const;                        ///< 有効チャンネル数を返す
		void set_sub_memo(int ch, const std::string &memo); ///< チャンネルメモを設定する
		void set_sub_code(int ch, const std::string &code); ///< チャンネル識別コードを設定する
		std::string sub_code(int ch) const;                 ///< チャンネル識別コードを取得する

		/// 識別コードからチャンネル (0-indexed) を解決する。見つからなければ -1
		int resolve_code(const std::string &code) const;

		/// 音声を送信する（ch は 0-indexed）。送信は ASIO スレッドへ委譲される
		void send_audio(
			int          ch,
			const float *data,
			size_t       frames,
			uint32_t     sample_rate,
			uint32_t     channels);

		/// RTT 計測を開始する（ch は 0-indexed）
		bool start_measurement(int ch,
							   int num_pings      = ods::core::DEFAULT_PING_COUNT,
							   int interval_ms    = 150,
							   int start_delay_ms = 0);

		/// RTT 計測中かを返す（ch は 0-indexed）
		bool is_measuring(int ch) const;

		/// 最後の計測結果を返す（ch は 0-indexed）
		LatencyResult last_result(int ch) const;

		/// 計測完了コールバックを設定する（ch は 0-indexed）
		void set_on_latency_result(int ch, LatencyCallback cb);

		/// HTTP インデックス HTML を設定する
		void set_http_index_html(std::string html);

		/// 全コールバックをクリアする（破棄前に呼ぶ）
		void clear_callbacks();

		/// ディレイ反映通知を送る
		void notify_apply_delay(int ch, double ms, const char *reason = "auto_measure");

		/// 接続数を返す（ch は 0-indexed）
		size_t client_count(int ch) const;

		/// 受信 URL を生成する
		std::string make_url(const std::string &host, int ch_1indexed) const;

		/// 音声コーデック設定を反映する
		void set_audio_config(const AudioConfig &cfg);

		/// HTTP ルートディレクトリを設定する
		void set_http_root_dir(std::string dir);

	private:

		struct MeasureThread {
			std::thread       th;          ///< 計測処理スレッド
			std::atomic<bool> done{false}; ///< スレッド完了フラグ
		};

		struct ChannelCache {
			LatencyResult last_result;              ///< 最終計測結果キャッシュ
			double        last_applied_delay{-1.0}; ///< 最終反映ディレイキャッシュ
			std::string   last_applied_reason;      ///< 最終反映理由キャッシュ
		};

		std::shared_ptr<WsServer> server_ptr_;                 ///< websocketpp サーバー本体
		std::thread               thread_;                     ///< サーバー実行スレッド
		mutable std::mutex        mtx_;                        ///< 共有状態の排他制御
		uint16_t                  port_ = 0;                   ///< リッスンポート
		std::atomic<bool>         running_{false};             ///< 稼働状態フラグ
		std::string               stream_id_;                  ///< 現在の配信 ID
		std::string               http_index_html_;            ///< ルート `/` の固定 HTML
		std::string               http_root_dir_;              ///< 静的配信ディレクトリ
		std::atomic<int>          audio_codec_{0};             ///< 0: Opus, 1: PCM
		std::atomic<bool>         opus_reset_pending_{false};  ///< Opus 状態再初期化要求
		std::atomic<bool>         opus_flush_pending_{false};  ///< Opus FIFO ドレイン要求
		std::atomic<int>          opus_bitrate_kbps_{96};      ///< Opus ビットレート
		std::atomic<int>          opus_target_sample_rate_{0}; ///< 0: 入力サンプルレート
		std::atomic<int>          audio_quantization_bits_{8}; ///< PCM 量子化ビット数
		std::atomic<bool>         audio_mono_{true};           ///< モノラル化有効フラグ
		std::atomic<int>          pcm_downsample_ratio_{4};    ///< 1: そのまま, 2: 1/2, 4: 1/4

		/// クライアント再生バッファ目標
		std::atomic<int> playback_buffer_ms_{ods::core::PLAYBACK_BUFFER_DEFAULT_MS};

		std::atomic<uint64_t>               pb_debounce_seq_{0};                    ///< バッファ設定通知の世代番号
		std::atomic<bool>                   pb_debounce_running_{false};            ///< 通知ディボウンス実行中フラグ
		std::vector<OpusEncoder>            opus_;                                  ///< チャンネル別 Opus エンコーダ
		int                                 active_ch_max_ = ods::core::MAX_SUB_CH; ///< 有効 CH 上限
		ConnectionMap                       conn_map_;                              ///< 接続ハンドルごとの情報
		std::map<std::string, ChannelState> ch_map_;                                ///< "stream_id/ch_0idx" ごとの状態
		std::map<std::string, ChannelCache> ch_cache_;                              ///< stop() 時に退避する計測結果・適用ディレイキャッシュ
		std::mutex                          measure_threads_mtx_;                   ///< 計測スレッド配列の排他制御

		std::vector<std::unique_ptr<MeasureThread>>    measure_threads_; ///< 実行中/完了待ち計測スレッド
		std::array<std::string, ods::core::MAX_SUB_CH> sub_memo_{};      ///< CH ごとのメモ文字列
		std::array<std::string, ods::core::MAX_SUB_CH> sub_code_{};      ///< CH ごとの識別コード

		// 三角形フィルタ [0.25, 0.5, 0.25] による 1/2 間引きを段階適用する。
		// factor=2 は 1 段、factor=4 は 2 段カスケード（等価 7 タップ FIR）。
		/// PCM を指定比率でダウンサンプリングする。
		static void downsample_pcm(const float        *in,
								   size_t              in_frames,
								   uint32_t            channels,
								   int                 factor,
								   std::vector<float> &out,
								   size_t             &out_frames);

		/// 浮動小数サンプルを指定ビット深度で量子化する。
		static float quantize_sample(float v, int bits);

		/// レイテンシ結果を JSON 文字列に整形する。
		static std::string format_latency_result_json(const LatencyResult &r);

		/// 配信 ID + CH に対応する状態を取得する（非 const）。
		ChannelState *find_ch(const std::string &sid, int ch);

		/// 配信 ID + CH に対応する状態を取得する（const）。
		const ChannelState *find_ch(const std::string &sid, int ch) const;

		/// 全計測スレッドの終了を待機する。
		void join_all_measure_threads();

		/// 完了済み計測スレッドを回収する。
		void cleanup_done_measure_threads();

		/// チャンネル数/量子化/モノラル化などの音声前処理を行う。
		void preprocess_audio(const float        *in_data,
							  size_t              frames,
							  uint32_t            in_channels,
							  const float       *&out_data,
							  uint32_t           &out_channels,
							  std::vector<float> &work);

		/// 指定 CH の Opus エンコーダを利用可能状態にする。
		bool ensure_opus_encoder(int ch, uint32_t sample_rate, uint32_t channels);

		/// 全 CH の Opus エンコード状態を初期化する。
		void reset_opus_state();

		/// PCM 入力を Opus パケット列に変換する。
		bool encode_opus_packets(int             ch,
								 const float    *data,
								 size_t          frames,
								 uint32_t        sample_rate,
								 uint32_t        channels,
								 OpusPacketList &out);

		void on_open(ConnectionHandle h);                               ///< WebSocket 接続開始イベント。
		void on_close(ConnectionHandle h);                              ///< WebSocket 接続終了イベント。
		void on_message(ConnectionHandle h, WsServer::message_ptr msg); ///< WebSocket メッセージ受信イベント。
		void on_http(ConnectionHandle h);                               ///< HTTP リクエスト処理。

		/// RTT 計測ループ本体。
		void measure_loop(
			const std::string &sid,
			int                ch,
			int                num_pings,
			int                interval_ms,
			int                start_delay_ms);

		/// 収集済みサンプルから最終計測結果を確定する。
		void finalize_result(const std::string &sid, int ch);

		/// 指定チャンネルの全接続へテキストをブロードキャストする。
		void broadcast_text(const std::string &sid, int ch, const std::string &msg);
	};

} // namespace ods::network
