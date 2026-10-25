#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace ods::core {

	inline std::string string_vprintf(const char *fmt, std::va_list args) {
		if (!fmt) return {};

		std::vector<char> buf(128, '\0');
		while (true) {
			std::va_list copy;
			va_copy(copy, args);
			const int n = std::vsnprintf(buf.data(), buf.size(), fmt, copy);
			va_end(copy);

			if (n >= 0 && static_cast<size_t>(n) < buf.size()) {
				return std::string(buf.data(), static_cast<size_t>(n));
			}
			if (n >= 0) {
				buf.resize(static_cast<size_t>(n) + 1);
			} else {
				buf.resize(buf.size() * 2);
			}
		}
	}

	inline std::string string_printf(const char *fmt, ...) {
		std::va_list args;
		va_start(args, fmt);
		std::string out = string_vprintf(fmt, args);
		va_end(args);
		return out;
	}

	/// CreateProcess のコマンドラインへ渡す 1 引数を、Windows CRT の解析規則どおりに引用符で囲む。
	/// 引用符の直前のバックスラッシュは 2 倍に、引用符自体は \" にし、末尾のバックスラッシュも
	/// 2 倍にする。これを怠ると、入力中の \" で引用が閉じて別の引数を挿入される。
	inline std::string quote_windows_arg(const std::string &text) {
		std::string quoted;
		quoted.reserve(text.size() + 4);
		quoted += '"';
		size_t backslashes = 0;
		for (char c : text) {
			if (c == '\\') {
				++backslashes;
				continue;
			}
			if (c == '"') {
				quoted.append(backslashes * 2 + 1, '\\');
			} else {
				quoted.append(backslashes, '\\');
			}
			quoted += c;
			backslashes = 0;
		}
		quoted.append(backslashes * 2, '\\');
		quoted += '"';
		return quoted;
	}

	/// 文字列中の URL の userinfo（scheme://user:pass@host の user:pass）を *** に置き換える。
	/// FFmpeg のエラー行は入力 URL を含み得るため、UI に出す前に通す。FFmpeg の解析と同じく
	/// ホスト部は / ? # の手前までとし（空白も含む）、その中で最後の @ を区切りとする。
	/// @param truncated text が途中で切られている場合は true。ホスト部が末尾まで続く URL は
	///                  @ が切り捨てられた可能性があるため、:// 以降をすべて伏せる
	inline std::string redact_url_userinfo(std::string text, bool truncated = false) {
		for (size_t pos = text.find("://"); pos != std::string::npos; pos = text.find("://", pos)) {
			pos += 3;
			const size_t end = (std::min)(text.find_first_of("/?#\r\n", pos), text.size()); // windows.h の min 対策
			if (truncated && end == text.size()) {
				text.replace(pos, end - pos, "***");
				break;
			}
			const size_t at = text.rfind('@', end - 1);
			if (end > pos && at != std::string::npos && at >= pos) {
				text.replace(pos, at - pos, "***");
				pos += 3;
			}
		}
		return text;
	}

} // namespace ods::core
