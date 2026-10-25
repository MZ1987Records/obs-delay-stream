// ID 生成と、外部実行ファイルの検証（SHA-256・承認値との照合）の自己チェック。
// 失敗すると非 0 で終了する。
//   cmake --build build --config RelWithDebInfo --target plugin-utils-check

#include "core/approved-binaries.hpp"
#include "core/binary-verify.hpp"
#include "plugin/plugin-utils.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>

using namespace ods::core;
using ods::plugin::generate_stream_id;

namespace {

	int failures = 0;

	void check(bool ok, const char *what) {
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++failures;
		}
	}

} // namespace

int main() {
	const std::string a = generate_stream_id(12);
	check(a.size() == 12, "length 12");
	check(generate_stream_id(8).size() == 8, "length 8");
	check(generate_stream_id(100).size() == 100, "length beyond one random buffer");
	check(a != generate_stream_id(12), "two ids differ");

	// 62 文字すべてが現れ、英数字以外は出ない（棄却サンプリングの範囲誤りを検出する）
	std::set<char> seen;
	bool           alnum_only = true;
	for (char c : generate_stream_id(62 * 200)) {
		alnum_only = alnum_only && std::isalnum(static_cast<unsigned char>(c));
		seen.insert(c);
	}
	check(alnum_only, "alphanumeric only");
	check(seen.size() == 62, "all 62 characters appear");

	// SHA-256 と承認値との照合
	{
		namespace fs   = std::filesystem;
		const auto dir = fs::temp_directory_path() / ("ods-plugin-utils-check-" + generate_stream_id(12));
		std::error_code ec;
		const bool      created = fs::create_directory(dir, ec);
		check(created, "create unique temp directory");
		if (created) {
			const std::string small = (dir / "abc.bin").string();
			const std::string large = (dir / "large.bin").string();
			std::ofstream(small, std::ios::binary) << "abc";
			std::ofstream(large, std::ios::binary) << std::string(2500000, 'a'); // 1 MiB の読込単位をまたぐ

			const char *abc_sha   = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
			const char *large_sha = "38a637965059125eeb67f54c30e7f48a61859a467a800ba09740ba48a924f2b9";
			check(sha256_file_hex(small) == abc_sha, "sha256 of abc");
			check(sha256_file_hex(large) == large_sha, "sha256 across read buffers");
			check(sha256_file_hex((dir / "missing.bin").string()).empty(), "sha256 of missing file");

			check(verify_file(small, 3, abc_sha) == VerifyResult::Ok, "verify ok");
			check(verify_file(small, 3, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") ==
					  VerifyResult::Ok,
				  "verify accepts uppercase hex");
			check(verify_file(small, 4, abc_sha) == VerifyResult::Mismatch, "verify size mismatch");
			check(verify_file(small, 3, large_sha) == VerifyResult::Mismatch, "verify hash mismatch");
			check(verify_file((dir / "missing.bin").string(), 3, abc_sha) == VerifyResult::Missing, "verify missing");
			check(verify_file(dir.string(), 0, abc_sha) == VerifyResult::Missing, "directory is not a file");

			ec.clear();
			fs::remove_all(dir, ec);
			check(!ec, "remove unique temp directory");
		}
	}

	// 承認一覧の値の形（版を固定した URL・64 桁の小文字 16 進）
	for (const ApprovedBinary *bin : {&kApprovedCloudflared, &kApprovedFfmpeg}) {
		check(std::strstr(bin->url, "/latest/") == nullptr, "approved url is pinned");
		for (const char *h : {bin->download_sha256, bin->exe_sha256}) {
			bool lower_hex = std::strlen(h) == 64;
			for (const char *c = h; *c; ++c)
				lower_hex = lower_hex && (std::isdigit(static_cast<unsigned char>(*c)) || (*c >= 'a' && *c <= 'f'));
			check(lower_hex, "approved sha256 is 64 lowercase hex digits");
		}
		check(bin->download_size > 0 && bin->exe_size > 0, "approved sizes are set");
	}

	if (failures == 0) std::printf("plugin-utils-check: OK\n");
	return failures == 0 ? 0 : 1;
}
