#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "plugin/plugin-utils.hpp"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#include "core/constants.hpp"

namespace plugin_utils {

std::string trim_copy(std::string s) {
	const char  *ws    = " \t\r\n";
	const size_t begin = s.find_first_not_of(ws);
	if (begin == std::string::npos) return "";
	const size_t end = s.find_last_not_of(ws);
	return s.substr(begin, end - begin + 1);
}

std::string extract_host_from_url(const std::string &url) {
	if (url.empty()) return "";
	const size_t scheme     = url.find("://");
	const size_t host_begin = (scheme == std::string::npos) ? 0 : (scheme + 3);
	if (host_begin >= url.size()) return "";

	size_t host_end = url.find_first_of("/?#", host_begin);
	if (host_end == std::string::npos) host_end = url.size();
	if (host_end <= host_begin) return "";

	std::string host_port = url.substr(host_begin, host_end - host_begin);
	if (host_port.empty()) return "";

	const size_t at = host_port.rfind('@');
	if (at != std::string::npos) {
		if (at + 1 >= host_port.size()) return "";
		host_port = host_port.substr(at + 1);
	}

	if (host_port.empty()) return "";
	if (host_port.front() == '[') {
		const size_t close = host_port.find(']');
		if (close == std::string::npos || close <= 1) return "";
		return host_port.substr(1, close - 1);
	}

	const size_t colon = host_port.find(':');
	return colon == std::string::npos ? host_port : host_port.substr(0, colon);
}

std::string normalize_rtmp_url_candidate(const char *raw) {
	if (!raw || !*raw) return "";
	std::string url = trim_copy(raw);
	if (url.empty()) return "";
	if (_strnicmp(url.c_str(), "rtmp://", 7) == 0) return url;
	if (_strnicmp(url.c_str(), "rtmps://", 8) == 0) return url;
	if (url.find("://") != std::string::npos) return "";
	if (_stricmp(url.c_str(), "auto") == 0) return "";
	if (_stricmp(url.c_str(), "default") == 0) return "";
	if (url.find_first_of(" \t\r\n") != std::string::npos) return "";
	return "rtmp://" + url;
}

std::string join_rtmp_url_and_stream_key(const std::string &base_url,
										 const std::string &raw_key) {
	std::string base = trim_copy(base_url);
	std::string key  = trim_copy(raw_key);
	if (base.empty() || key.empty()) return base;

	if (_strnicmp(key.c_str(), "rtmp://", 7) == 0 ||
		_strnicmp(key.c_str(), "rtmps://", 8) == 0) {
		return normalize_rtmp_url_candidate(key.c_str());
	}

	while (!key.empty() && key.front() == '/')
		key.erase(key.begin());
	if (key.empty()) return base;

	if (base.size() >= key.size() &&
		base.compare(base.size() - key.size(), key.size(), key) == 0) {
		if (base.size() == key.size() || base[base.size() - key.size() - 1] == '/') {
			return base;
		}
	}

	if (!base.empty() && base.back() != '/') base.push_back('/');
	return base + key;
}

std::string get_local_ip() {
	ULONG             buf_size = 15000;
	std::vector<BYTE> buf(buf_size);
	auto             *adapters = reinterpret_cast<IP_ADAPTER_INFO *>(buf.data());
	if (GetAdaptersInfo(adapters, &buf_size) != NO_ERROR) return "127.0.0.1";
	std::string fallback;
	for (auto *a = adapters; a; a = a->Next) {
		for (auto *ip = &a->IpAddressList; ip; ip = ip->Next) {
			std::string addr = ip->IpAddress.String;
			if (addr == "0.0.0.0" || addr.empty()) continue;
			if (addr.rfind("192.168.", 0) == 0 || addr.rfind("10.", 0) == 0) return addr;
			unsigned a0, a1, a2, a3;
			if (sscanf(addr.c_str(), "%u.%u.%u.%u", &a0, &a1, &a2, &a3) == 4 &&
				a0 == 172 && a1 >= 16 && a1 <= 31) return addr;
			if (fallback.empty()) fallback = addr;
		}
	}
	return fallback.empty() ? "127.0.0.1" : fallback;
}

void copy_to_clipboard(const std::string &text) {
	if (!OpenClipboard(nullptr)) return;
	EmptyClipboard();
	std::wstring w;
	if (!text.empty()) {
		int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
		if (wlen > 0) {
			w.resize(static_cast<size_t>(wlen));
			MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &w[0], wlen);
		}
	}
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
	if (h) {
		wchar_t *p = static_cast<wchar_t *>(GlobalLock(h));
		if (p) {
			if (!w.empty()) memcpy(p, w.data(), w.size() * sizeof(wchar_t));
			p[w.size()] = L'\0';
			GlobalUnlock(h);
			SetClipboardData(CF_UNICODETEXT, h);
		}
	}
	CloseClipboard();
}

std::string sanitize_stream_id(const char *raw) {
	if (!raw) return "";
	std::string s(raw);
	s.erase(std::remove_if(s.begin(), s.end(), [](char c) {
				return !std::isalnum((unsigned char)c) && c != '-' && c != '_';
			}),
			s.end());
	return s;
}

std::string generate_stream_id(std::size_t len) {
	static const char kChars[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	std::random_device                    rd;
	std::mt19937                          rng(rd());
	std::uniform_int_distribution<size_t> dist(0, sizeof(kChars) - 2);
	std::string                           out;
	out.reserve(len);
	for (size_t i = 0; i < len; ++i)
		out.push_back(kChars[dist(rng)]);
	return out;
}

std::string make_alpha_counter_label(int index) {
	if (index < 0) index = 0;
	std::string out;
	for (int n = index; n >= 0; n = (n / 26) - 1) {
		out.push_back(static_cast<char>('A' + (n % 26)));
	}
	std::reverse(out.begin(), out.end());
	return out;
}

int clamp_sub_ch_count(int v) {
	if (v < 1) return 1;
	if (v > MAX_SUB_CH) return MAX_SUB_CH;
	return v;
}

int normalize_opus_sample_rate(int v) {
	switch (v) {
	case 8000:
	case 12000:
	case 16000:
	case 24000:
	case 48000:
		return v;
	default:
		return 48000;
	}
}

int normalize_quantization_bits(int v) {
	switch (v) {
	case 8:
	case 16:
		return v;
	default:
		return 16;
	}
}

int normalize_pcm_downsample_ratio(int v) {
	switch (v) {
	case 1:
	case 2:
	case 4:
		return v;
	default:
		return 1;
	}
}

int normalize_playback_buffer_ms(int v) {
	if (v < PLAYBACK_BUFFER_MIN_MS) return PLAYBACK_BUFFER_MIN_MS;
	if (v > PLAYBACK_BUFFER_MAX_MS) return PLAYBACK_BUFFER_MAX_MS;
	return v;
}

std::string read_file_to_string(const char *path) {
	if (!path || !*path) return "";
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs) return "";
	return std::string((std::istreambuf_iterator<char>(ifs)),
					   std::istreambuf_iterator<char>());
}

void replace_all(std::string &s, const std::string &from, const std::string &to) {
	if (from.empty()) return;
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.length(), to);
		pos += to.length();
	}
}

} // namespace plugin_utils
