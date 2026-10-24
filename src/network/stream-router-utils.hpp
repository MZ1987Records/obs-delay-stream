#pragma once

#include <chrono>
#include <string>

namespace ods::network {

	/**
	 * 接続ごとの制御メッセージの頻度制限（トークンバケット）。
	 * 毎秒 rate_per_sec 件ずつ補充し、最大 burst 件まで貯められる。
	 */
	class MessageRateLimiter {
	public:

		MessageRateLimiter(double rate_per_sec, double burst)
			: rate_(rate_per_sec), burst_(burst), tokens_(burst) {}

		/// 1 件を受け付けてよいかを返す。false なら上限超過。
		bool allow(std::chrono::steady_clock::time_point now);

	private:

		double                                rate_;
		double                                burst_;
		double                                tokens_;
		std::chrono::steady_clock::time_point last_{};
		bool                                  started_ = false;
	};

	/// 配信 ID とチャンネルから内部キー `"sid/ch"` を生成する。
	std::string make_key(const std::string &sid, int ch);

	/// 英数字のみを残して ID として安全な文字列にする。
	std::string sanitize_id(const std::string &raw);

	/// 受信者が送った配信 ID・チャンネルコードが英数字 1〜64 文字かを判定する。
	/// 不正な文字を除去して照合すると別の値へ読み替えてしまうため、除去せずに拒否する。
	bool is_valid_id(const std::string &id);

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

	/// WebSocket の Origin が Host と同一オリジンかを判定する。
	/// 受信ページは同じサーバーから配信されるため、別サイトのページからの接続を拒否できる。
	/// Origin を送らない非ブラウザークライアントは true（Origin は偽装でき、ブラウザー対策に限られる）。
	/// @param origin Origin ヘッダー（例: "https://xxx.trycloudflare.com"）
	/// @param host   Host ヘッダー（例: "xxx.trycloudflare.com"）
	bool is_same_origin(const std::string &origin, const std::string &host);

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
