#pragma once

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

} // namespace ods::core
