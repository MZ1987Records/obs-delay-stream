#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace websocket_server_detail {

inline std::string make_key(const std::string& sid, int ch) {
    return sid + "/" + std::to_string(ch);
}

inline std::string sanitize_id(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (std::isalnum((unsigned char)c))
            out += c;
    }
    return out;
}

inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
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

inline std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%' && i + 2 < s.size()
            && std::isxdigit((unsigned char)s[i + 1])
            && std::isxdigit((unsigned char)s[i + 2])) {
            auto hex = [](unsigned char v) -> int {
                if (v >= '0' && v <= '9') return v - '0';
                if (v >= 'a' && v <= 'f') return 10 + (v - 'a');
                if (v >= 'A' && v <= 'F') return 10 + (v - 'A');
                return 0;
            };
            int hi = hex((unsigned char)s[i + 1]);
            int lo = hex((unsigned char)s[i + 2]);
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

enum class PathParseResult {
    Ok,
    Invalid,
    ChOutOfRange,
};

inline PathParseResult parse_path(const std::string& path,
                                  std::string& stream_id, int& ch_0idx,
                                  int max_ch)
{
    std::string p = path;
    if (!p.empty() && p[0] == '/') p = p.substr(1);

    auto slash = p.find('/');
    if (slash == std::string::npos || slash == 0) return PathParseResult::Invalid;

    stream_id = sanitize_id(p.substr(0, slash));
    std::string ch_str = p.substr(slash + 1);
    auto q = ch_str.find_first_of("?#");
    if (q != std::string::npos) ch_str = ch_str.substr(0, q);

    char* end = nullptr;
    long ch_1idx = std::strtol(ch_str.c_str(), &end, 10);
    if (end == ch_str.c_str() || *end != '\0') return PathParseResult::Invalid;
    if (ch_1idx < 1 || ch_1idx > max_ch) return PathParseResult::ChOutOfRange;
    ch_0idx = (int)ch_1idx - 1;
    return stream_id.empty() ? PathParseResult::Invalid : PathParseResult::Ok;
}

inline bool is_safe_rel_path(const std::string& rel) {
    if (rel.empty()) return false;
    if (rel.find("..") != std::string::npos) return false;
    if (rel.find('\\') != std::string::npos) return false;
    if (rel.find(':') != std::string::npos) return false;
    return true;
}

inline std::string join_path(const std::string& base, const std::string& rel) {
    if (base.empty()) return rel;
    std::string out = base;
    char last = out.back();
    if (last != '/' && last != '\\') out += '/';
    out += rel;
    return out;
}

inline bool read_file_to_string(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    out.assign(std::istreambuf_iterator<char>(ifs),
               std::istreambuf_iterator<char>());
    return true;
}

inline const char* guess_content_type(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){
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

inline bool is_valid_opus_sample_rate(int sample_rate) {
    switch (sample_rate) {
    case 0: // auto (same as input sample rate)
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

inline bool is_valid_pcm_downsample_ratio(int r) {
    return r == 1 || r == 2 || r == 4;
}

} // namespace websocket_server_detail
