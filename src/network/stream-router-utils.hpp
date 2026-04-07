#pragma once

#include <string>

namespace ods::network {

	/// 配信 ID とチャンネルから内部キー `"sid/ch"` を生成する。
	std::string make_key(const std::string &sid, int ch);

	/// 英数字のみを残して ID として安全な文字列にする。
	std::string sanitize_id(const std::string &raw);

	/// JSON 文字列へ埋め込めるようにエスケープする。
	std::string json_escape(const std::string &s);

	/// URL エンコード文字列をデコードする（`+` は空白扱い）。
	std::string url_decode(const std::string &s);

	enum class PathParseResult {
		Ok,           ///< 正常に解析できた
		Invalid,      ///< パス形式が不正
		ChOutOfRange, ///< チャンネル番号が範囲外
		CodeNotFound, ///< チャンネル識別コードが未登録
	};

	// チャンネル識別コード版: stream_id と code を抽出する。
	PathParseResult parse_path_code(const std::string &path,
									std::string       &stream_id,
									std::string       &code);

	/// 相対パスとして扱ってよい形式かを判定する。
	bool is_safe_rel_path(const std::string &rel);

	/// ベースパスと相対パスを結合する。
	std::string join_path(const std::string &base, const std::string &rel);

	/// ファイル全体を文字列として読み込む。
	bool read_file_to_string(const std::string &path, std::string &out);

	/// 拡張子から HTTP Content-Type を推定する。
	const char *guess_content_type(const std::string &path);

	/// Opus で利用可能なサンプルレートかを判定する。
	bool is_valid_opus_sample_rate(int sample_rate);

	/// PCM ダウンサンプル比（1/2/4）が有効かを判定する。
	bool is_valid_pcm_downsample_ratio(int r);

} // namespace ods::network
