#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "network/stream-router-utils.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace ods::network {

	std::string make_key(const std::string &sid, int ch) {
		return sid + "/" + std::to_string(ch);
	}

	std::string sanitize_id(const std::string &raw) {
		std::string out;
		for (char c : raw) {
			if (std::isalnum((unsigned char)c))
				out += c;
		}
		return out;
	}

	std::string json_escape(const std::string &s) {
		std::string out;
		out.reserve(s.size() + 8);
		for (unsigned char c : s) {
			switch (c) {
			case '\\':
				out += "\\\\";
				break;
			case '"':
				out += "\\\"";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (c < 0x20) {
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					out.push_back((char)c);
				}
			}
		}
		return out;
	}

	namespace {
		/// 16進1文字を数値へ変換する（不正値は0）。
		int hex_value(unsigned char v) {
			if (v >= '0' && v <= '9') return v - '0';
			if (v >= 'a' && v <= 'f') return 10 + (v - 'a');
			if (v >= 'A' && v <= 'F') return 10 + (v - 'A');
			return 0;
		}
	} // namespace

	std::string url_decode(const std::string &s) {
		std::string out;
		out.reserve(s.size());
		for (size_t i = 0; i < s.size(); ++i) {
			unsigned char c = (unsigned char)s[i];
			if (c == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
				int hi = hex_value((unsigned char)s[i + 1]);
				int lo = hex_value((unsigned char)s[i + 2]);
				out.push_back((char)((hi << 4) | lo));
				i += 2;
			} else if (c == '+') {
				out.push_back(' ');
			} else {
				out.push_back((char)c);
			}
		}
		return out;
	}

	PathParseResult parse_path_code(const std::string &path,
									std::string       &stream_id,
									std::string       &code) {
		std::string p = path;
		if (!p.empty() && p[0] == '/') p = p.substr(1);

		auto slash = p.find('/');
		if (slash == std::string::npos || slash == 0) return PathParseResult::Invalid;

		stream_id            = p.substr(0, slash);
		std::string raw_code = p.substr(slash + 1);
		auto        q        = raw_code.find_first_of("?#");
		if (q != std::string::npos) raw_code = raw_code.substr(0, q);

		code = raw_code;
		if (!is_valid_id(stream_id) || !is_valid_id(code)) return PathParseResult::Invalid;
		return PathParseResult::Ok;
	}

	bool MessageRateLimiter::allow(std::chrono::steady_clock::time_point now) {
		if (started_) {
			const double elapsed_s = std::chrono::duration<double>(now - last_).count();
			if (elapsed_s > 0.0) tokens_ = std::min(burst_, tokens_ + elapsed_s * rate_);
		}
		started_ = true;
		last_    = now;
		if (tokens_ < 1.0) return false;
		tokens_ -= 1.0;
		return true;
	}

	bool is_valid_id(const std::string &id) {
		constexpr std::size_t kMaxIdLength = 64;
		if (id.empty() || id.size() > kMaxIdLength) return false;
		return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum((unsigned char)c) != 0; });
	}

	bool is_same_origin(const std::string &origin, const std::string &host) {
		if (origin.empty()) return true;
		// "null"（sandbox iframe・file:// 等）や scheme の無い値は同一オリジンになり得ない。
		const auto sep = origin.find("://");
		if (sep == std::string::npos || host.empty()) return false;
		const std::string authority = origin.substr(sep + 3);
		if (authority.size() != host.size()) return false;
		return std::equal(authority.begin(), authority.end(), host.begin(), [](char a, char b) {
			return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
		});
	}

	bool is_safe_rel_path(const std::string &rel) {
		if (rel.empty()) return false;
		if (rel.find("..") != std::string::npos) return false;
		if (rel.find('\\') != std::string::npos) return false;
		if (rel.find(':') != std::string::npos) return false;
		return true;
	}

	std::string join_path(const std::string &base, const std::string &rel) {
		if (base.empty()) return rel;
		std::string out  = base;
		char        last = out.back();
		if (last != '/' && last != '\\') out += '/';
		out += rel;
		return out;
	}

	bool is_free_of_reparse_points(const std::string &root, const std::string &rel) {
		// ifstream と同じく ANSI API で解釈し、読み込み時と同じ実体を調べる。
		std::string path = root;
		size_t      pos  = 0;
		while (pos <= rel.size()) {
			size_t      next = rel.find('/', pos);
			std::string seg  = rel.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
			if (!seg.empty()) {
				path              = join_path(path, seg);
				const DWORD attrs = GetFileAttributesA(path.c_str());
				if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
			}
			if (next == std::string::npos) break;
			pos = next + 1;
		}
		return true;
	}

	bool read_file_to_string(const std::string &path, std::string &out, std::size_t max_bytes) {
		std::ifstream ifs(path, std::ios::binary | std::ios::ate);
		if (!ifs) return false;
		const auto size = ifs.tellg();
		if (size < 0 || static_cast<unsigned long long>(size) > max_bytes) return false;
		ifs.seekg(0);
		out.assign(std::istreambuf_iterator<char>(ifs),
				   std::istreambuf_iterator<char>());
		return true;
	}

	const char *guess_content_type(const std::string &path) {
		auto        dot = path.find_last_of('.');
		std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
			return (char)std::tolower(c);
		});
		if (ext == ".html") return "text/html; charset=utf-8";
		if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
		if (ext == ".css") return "text/css; charset=utf-8";
		if (ext == ".wasm") return "application/wasm";
		if (ext == ".json" || ext == ".map") return "application/json; charset=utf-8";
		if (ext == ".svg") return "image/svg+xml";
		if (ext == ".png") return "image/png";
		if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
		if (ext == ".gif") return "image/gif";
		return "application/octet-stream";
	}

	bool is_valid_opus_sample_rate(int sample_rate) {
		switch (sample_rate) {
		case 8000:
		case 12000:
		case 16000:
		case 24000:
		case 48000:
			return true;
		default:
			return false;
		}
	}

	bool is_valid_pcm_downsample_ratio(int r) {
		return r == 1 || r == 2 || r == 4;
	}

} // namespace ods::network
