// 受信サーバーの照合関数と、外部入力を子プロセスへ渡す引数クォートの自己チェック。
// 失敗すると非 0 で終了する。
//   cmake --build build --config RelWithDebInfo --target stream-router-utils-check

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core/string-format.hpp"
#include "network/stream-router-utils.hpp"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

using namespace ods::network;
using ods::core::quote_windows_arg;

namespace {

	int failures = 0;

	void check(bool ok, const char *what) {
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++failures;
		}
	}

	// Windows 自身の規則（CommandLineToArgvW）でコマンドラインを分割する。入力は ASCII のみ。
	std::vector<std::string> split_windows_cmdline(const std::string &cmdline) {
		const std::wstring       wide(cmdline.begin(), cmdline.end());
		int                      argc = 0;
		LPWSTR                  *argv = CommandLineToArgvW(wide.c_str(), &argc);
		std::vector<std::string> out;
		for (int i = 0; argv && i < argc; ++i) {
			std::string arg;
			for (const wchar_t *p = argv[i]; *p; ++p)
				arg.push_back(static_cast<char>(*p));
			out.push_back(std::move(arg));
		}
		LocalFree(argv);
		return out;
	}

	// 引数 1 個として渡り、前後の引数がずれないこと。
	bool round_trips(const std::string &arg) {
		const auto argv = split_windows_cmdline("ffmpeg.exe -i " + quote_windows_arg(arg) + " -map 0:a:0");
		return argv.size() == 5 && argv[2] == arg && argv[3] == "-map";
	}

} // namespace

int main() {
	// 子プロセスへの引数クォート（RTSP URL の FFmpeg 引数注入の回帰テスト）
	check(round_trips("rtsp://host:8554/live"), "plain url");
	check(round_trips("rtsp://host/a b"), "space");
	check(round_trips("rtsp://host/\\\" -i file:secret.txt"), "backslash + quote injection");
	check(round_trips("rtsp://host/\" -y out.wav"), "quote injection");
	check(round_trips("rtsp://host/dir\\"), "trailing backslash");
	check(round_trips("rtsp://host/a\\\\\\\"b"), "many backslashes before quote");
	check(round_trips("C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe"), "exe path");
	check(round_trips(""), "empty");

	// トンネル経由・LAN・ループバックの正規の受信ページ
	check(is_same_origin("https://abc.trycloudflare.com", "abc.trycloudflare.com"), "tunnel");
	check(is_same_origin("http://192.168.1.5:19000", "192.168.1.5:19000"), "lan");
	check(is_same_origin("http://LOCALHOST:19000", "localhost:19000"), "case-insensitive");
	check(is_same_origin("", "localhost:19000"), "non-browser client without Origin");

	// 別サイト・別ポート・不正な Origin
	check(!is_same_origin("https://evil.example", "abc.trycloudflare.com"), "cross-site");
	check(!is_same_origin("http://192.168.1.5:8080", "192.168.1.5:19000"), "other port");
	check(!is_same_origin("https://abc.trycloudflare.com.evil.example", "abc.trycloudflare.com"), "suffix");
	check(!is_same_origin("null", "localhost:19000"), "null origin");
	check(!is_same_origin("http://localhost:19000", ""), "missing Host");

	std::string sid;
	std::string code;
	check(parse_path_code("/sid123/code456", sid, code) == PathParseResult::Ok && sid == "sid123" &&
			  code == "code456",
		  "path ok");
	check(parse_path_code("/sid123", sid, code) == PathParseResult::Invalid, "path without code");
	check(parse_path_code("//code456", sid, code) == PathParseResult::Invalid, "path without sid");
	check(parse_path_code("/sid123/code456?x=1", sid, code) == PathParseResult::Ok && code == "code456",
		  "query is not part of code");
	// 不正な文字は除去して読み替えず拒否する（"co-de" を "code" として照合しない）
	check(parse_path_code("/sid123/co-de", sid, code) == PathParseResult::Invalid, "symbol in code");
	check(parse_path_code("/sid123/code456/", sid, code) == PathParseResult::Invalid, "extra segment");
	check(parse_path_code("/sid%31/code456", sid, code) == PathParseResult::Invalid, "percent in sid");

	check(is_valid_id("abcXYZ019"), "alnum id");
	check(is_valid_id(std::string(64, 'a')), "64 chars");
	check(!is_valid_id(std::string(65, 'a')), "65 chars");
	check(!is_valid_id(""), "empty id");
	check(!is_valid_id("ab cd"), "space in id");

	// 頻度制限: 20 件/秒・最大 40 件
	{
		using std::chrono::milliseconds;
		const auto         t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
		MessageRateLimiter rl(20.0, 40.0);
		int                accepted = 0;
		for (int i = 0; i < 50; ++i)
			accepted += rl.allow(t0) ? 1 : 0;
		check(accepted == 40, "burst capped at 40");
		check(!rl.allow(t0 + milliseconds(10)), "no refill within 10 ms");
		check(rl.allow(t0 + milliseconds(60)), "refilled after 60 ms");

		// 正規の受信ページの pong（150 ms 間隔）は 1 分間続けても拒否されない
		MessageRateLimiter steady(20.0, 40.0);
		bool               all_ok = true;
		for (int i = 0; i < 400; ++i)
			all_ok = all_ok && steady.allow(t0 + milliseconds(150 * i));
		check(all_ok, "pong every 150 ms is allowed");
	}

	// 静的配信: 経路の reparse point 判定とサイズ上限（ジャンクションの拒否は手作業で確認する）
	{
		const auto root = std::filesystem::temp_directory_path() / "ods-stream-router-utils-check";
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root / "assets");
		std::ofstream(root / "assets" / "a.js", std::ios::binary) << "0123456789";
		const std::string r = root.string();
		check(is_free_of_reparse_points(r, "assets/a.js"), "plain file path");
		check(!is_free_of_reparse_points(r, "assets/missing.js"), "missing file");
		std::string body;
		check(read_file_to_string((root / "assets" / "a.js").string(), body, 10) && body == "0123456789",
			  "file at size limit");
		check(!read_file_to_string((root / "assets" / "a.js").string(), body, 9), "file over size limit");
		std::filesystem::remove_all(root);
	}

	if (failures == 0) std::printf("stream-router-utils-check: OK\n");
	return failures == 0 ? 0 : 1;
}
