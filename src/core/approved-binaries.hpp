#pragma once

#include <cstdint>

namespace ods::core {

	/**
	 * プラグインが自動取得して起動する外部実行ファイルの承認版。
	 * 取得物はこのサイズと SHA-256 に一致したときだけ配置・起動する。
	 *
	 * 更新手順: 上流の告知とリリースを確認し、版・URL・サイズ・SHA-256 を書き換える。
	 * SHA-256 は取得元と別の経路（リリースノートの記載、GitHub API の digest など）とも照合する。
	 * 書き換えたら docs/dependencies.md も更新する。
	 */
	struct ApprovedBinary {
		const char   *name;            ///< 表示名
		const char   *version;         ///< 版（配置先ディレクトリ名にも使う）
		const char   *url;             ///< 取得 URL（latest ではなく版を固定したもの）
		std::uint64_t download_size;   ///< 取得物のバイト数
		const char   *download_sha256; ///< 取得物の SHA-256（小文字 16 進）
		const char   *exe_in_archive;  ///< 取得物が ZIP のとき中の実行ファイルの相対パス。exe そのものなら nullptr
		std::uint64_t exe_size;        ///< 実行ファイルのバイト数
		const char   *exe_sha256;      ///< 実行ファイルの SHA-256（起動前の検証に使う）
	};

	// cloudflared 2026.11.0（2026-11-12 公開）。SHA-256 はリリースノートの記載と GitHub API の digest で照合済み。
	inline constexpr ApprovedBinary kApprovedCloudflared = {
		"cloudflared",
		"2026.11.0",
		"https://github.com/cloudflare/cloudflared/releases/download/2026.11.0/cloudflared-windows-amd64.exe",
		55397592,
		"d5c1d82031da6253dcdcff64c944cd0fe063016edeb2e4fd088075fc9bd52d0a",
		nullptr,
		55397592,
		"d5c1d82031da6253dcdcff64c944cd0fe063016edeb2e4fd088075fc9bd52d0a",
	};

	// FFmpeg 9.0 系 LGPL 静的ビルド（BtbN autobuild-2026-10-31-12-59）。月末ビルドは約 2 年保持される。
	// 上流から消えたら新しい月末ビルドを承認し直す（自前では再配布しない）。
	inline constexpr ApprovedBinary kApprovedFfmpeg = {
		"FFmpeg",
		"n9.0.2-39-g7adfbe40dc",
		"https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-31-12-59/"
		"ffmpeg-n9.0.2-39-g7adfbe40dc-win64-lgpl-9.0.zip",
		168715328,
		"9d37c564e0d01c18cde184a079fa6ef04d7d2783ada41da81d04165cc8f1a650",
		"ffmpeg-n9.0.2-39-g7adfbe40dc-win64-lgpl-9.0\\bin\\ffmpeg.exe",
		132190720,
		"d03c858a130cae1382902925d9f46eb15d7de42e025d34cc29b7ddca6cfb4c62",
	};

} // namespace ods::core
