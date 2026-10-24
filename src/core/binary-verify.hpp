#pragma once

#include <cstdint>
#include <string>

namespace ods::core {

	/// ファイルの検証結果。
	enum class VerifyResult {
		Ok,       ///< サイズと SHA-256 が一致した
		Missing,  ///< ファイルが無い
		Mismatch, ///< サイズか SHA-256 が一致しない（改変・破損・別の版）
	};

	/// ファイルの SHA-256 を小文字 16 進で返す。読めなければ空文字。
	std::string sha256_file_hex(const std::string &path);

	/// ファイルが承認値のサイズと SHA-256 に一致するかを判定する。サイズが違えばハッシュは計算しない。
	VerifyResult verify_file(const std::string &path, std::uint64_t size, const char *sha256);

	/// url を dest へ取得し、サイズと SHA-256 が一致したときだけ dest に置く。
	/// 取得は dest + ".partial" へ行い、一致したら置き換える。失敗・不一致のときは一時ファイルを消し、
	/// 既存の dest には触れない。
	/// @param err 失敗理由（利用者向けの日本語）
	bool download_verified(const std::string &url,
						   const std::string &dest,
						   std::uint64_t      size,
						   const char        *sha256,
						   std::string       &err);

} // namespace ods::core
