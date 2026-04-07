#pragma once

#include <cstddef>
#include <string>

namespace ods::plugin {

	/// 先頭・末尾の空白を除去した文字列を返す
	std::string trim_copy(std::string s);

	/// URL からホスト部分を抽出する
	std::string extract_host_from_url(const std::string &url);

	/// RTMP URL 候補を正規化する（スキーム補完など）
	std::string normalize_rtmp_url_candidate(const char *raw);

	/// RTMP URL とストリームキーを結合する
	std::string join_rtmp_url_and_stream_key(const std::string &base_url,
											 const std::string &raw_key);

	/// ローカル IP アドレスを取得する
	std::string get_local_ip();

	/// テキストをクリップボードにコピーする
	void copy_to_clipboard(const std::string &text);

	/// 配信 ID として使用できない文字を除去・置換する
	std::string sanitize_stream_id(const char *raw);

	/// ランダムな配信 ID を生成する
	std::string generate_stream_id(std::size_t len = 12);

	/// 0-indexed インデックスをアルファベットラベル（A, B, ...）に変換する
	std::string make_alpha_counter_label(int index);

	/// サブチャンネル数を有効範囲にクランプする
	int clamp_sub_ch_count(int v);

	/// Opus がサポートするサンプルレートに丸める
	int normalize_opus_sample_rate(int v);

	/// 量子化ビット数を有効値に丸める
	int normalize_quantization_bits(int v);

	/// PCM ダウンサンプル比を有効値に丸める
	int normalize_pcm_downsample_ratio(int v);

	/// 再生バッファ量を有効範囲にクランプする
	int normalize_playback_buffer_ms(int v);

	/// ファイルを読み込んで文字列として返す
	std::string read_file_to_string(const char *path);

	/// 文字列中の `from` をすべて `to` に置換する
	void replace_all(std::string &s, const std::string &from, const std::string &to);

} // namespace ods::plugin
