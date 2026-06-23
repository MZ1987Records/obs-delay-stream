#pragma once

#include <obs-module.h>
#include <cstdint>
#include <string>

namespace ods::plugin {

	/// フィルタソースの親ソースの音声同期オフセット(ns)を取得する
	bool try_get_parent_audio_sync_offset_ns(obs_source_t *filter_source, int64_t &out_offset_ns);

	/// フィルタ親ソースの音声が OBS 配信出力のトラックに乗っているか判定する。
	/// 親ソースが取得できない、または配信出力が無い場合は false を返す（判定不可）。
	/// @param filter_source フィルタソース
	/// @param out_on_track  配信トラックに乗っていれば true
	/// @return 親ソースを特定できた場合のみ true
	bool try_get_parent_on_streaming_track(obs_source_t *filter_source, bool &out_on_track);

	/// 計測タブで表示するトラック割当の注意状態。
	enum class RtspTrackNote {
		None,         ///< 表示なし
		WarnOffTrack, ///< 配信中だがトラック未割当 → 計測不可の警告
		NoteRevert,   ///< 計測用にトラックへ乗せた状態 → 戻し忘れ注意
	};

	/// トラック割当状態から計測タブの注意表示を分類する（純粋関数）。
	/// @param on_track       親ソースが配信トラックに乗っているか
	/// @param seen_offtrack  計測文脈でトラック未割当を観測済みか
	/// @param streaming      OBS 配信がアクティブか
	RtspTrackNote classify_rtsp_track_note(bool on_track, bool seen_offtrack, bool streaming);

	/// ソースの設定から RTMP URL を解決する
	std::string resolve_rtmp_url_from_source(obs_source_t *source);
	/// RTMP URL のスキームを RTSP 系へ変換する
	std::string to_rtsp_url_from_rtmp(const std::string &rtmp_url);

	/// cloudflared パスが未設定なら自動検索結果を補完する
	void maybe_fill_cloudflared_path_from_auto(obs_source_t *source);

	/// 自動取得完了後に必要なら cloudflared パス設定を補完する
	void maybe_persist_cloudflared_path_after_auto_ready(obs_source_t *source);

} // namespace ods::plugin
