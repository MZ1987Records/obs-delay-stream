// plugin-utils の ID 生成の自己チェック。失敗すると非 0 で終了する。
//   cmake --build build --config RelWithDebInfo --target plugin-utils-check

#include "plugin/plugin-utils.hpp"

#include <cctype>
#include <cstdio>
#include <set>
#include <string>

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

	if (failures == 0) std::printf("plugin-utils-check: OK\n");
	return failures == 0 ? 0 : 1;
}
