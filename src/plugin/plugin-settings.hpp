#pragma once

#include <array>
#include <obs-module.h>

namespace ods::plugin {

	struct DelayStreamData;

	inline constexpr char kAvatarLatencyKey[]          = "avatar_latency_ms";
	inline constexpr char kMeasuredRtspE2eKey[]        = "measured_rtsp_e2e_ms";
	inline constexpr char kRtspE2eMeasuredKey[]        = "rtsp_e2e_measured";
	inline constexpr char kRtspUrlKey[]                = "rtsp_url";
	inline constexpr char kRtspUseRtmpUrlKey[]         = "rtsp_url_use_rtmp";
	inline constexpr char kFfmpegExePathKey[]          = "ffmpeg_exe_path";
	inline constexpr char kFfmpegExePathModeKey[]      = "ffmpeg_exe_path_mode";
	inline constexpr char kCloudflaredExePathKey[]     = "cloudflared_exe_path";
	inline constexpr char kCloudflaredExePathModeKey[] = "cloudflared_exe_path_mode";
	inline constexpr char kPathModeFromEnvPath[]       = "%PATH%";

	enum class ExePathMode {
		Auto     = 0, ///< 既定配置先から自動取得（未配置ならダウンロード）
		FromPath = 1, ///< `%PATH%` から実行ファイルを探索する
		Absolute = 2, ///< 絶対パスを手動指定する
	};

	/// 設定値を `ExePathMode` の有効範囲へ正規化する。
	ExePathMode normalize_exe_path_mode(int raw_mode);

	using SubSettingKey = std::array<char, 32>;

	SubSettingKey make_sub_key(const char *suffix, int ch); ///< `sub{ch}_{suffix}` 形式の設定キーを生成する
	SubSettingKey make_sub_measured_key(int ch);            ///< `sub{ch}_measured_ms` キーを生成する
	SubSettingKey make_sub_offset_key(int ch);              ///< `sub{ch}_adjust_ms` キーを生成する
	SubSettingKey make_sub_memo_key(int ch);                ///< `sub{ch}_memo` キーを生成する
	SubSettingKey make_sub_code_key(int ch);                ///< `sub{ch}_code` キーを生成する
	SubSettingKey make_sub_ws_measured_key(int ch);         ///< `sub{ch}_ws_measured` キーを生成する
	SubSettingKey make_sub_remove_row_key(int ch);          ///< `sub{ch}_remove_row` キーを生成する

	/// 1 チャンネルの補正前遅延値（R - A - C[i] + offset[i]、負値許容）を計算する
	int calc_ch_raw_delay_ms(int rtsp_e2e_ms,
							 int avatar_latency_ms,
							 int ch_measured_ms,
							 int offset_ms);

	/// 全チャンネルの遅延を一括再計算して DelayBuffer へ適用する
	void recalc_all_delays(DelayStreamData *d);

	/// OBS 設定値を `DelayStreamData` に反映する
	void apply_settings(DelayStreamData *d, obs_data_t *settings);

} // namespace ods::plugin
