// stream-router-utils の照合関数の自己チェック。失敗すると非 0 で終了する。
//   cmake --build build --config RelWithDebInfo --target stream-router-utils-check

#include "network/stream-router-utils.hpp"

#include <cstdio>

using namespace ods::network;

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

	if (failures == 0) std::printf("stream-router-utils-check: OK\n");
	return failures == 0 ? 0 : 1;
}
