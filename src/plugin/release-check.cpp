#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "plugin/release-check.hpp"

namespace ods::plugin {

	namespace {

		static constexpr const char *kLatestReleaseUrl =
			"https://github.com/MZ1987Records/obs-delay-stream/releases/latest";
		static constexpr const char *kTagReleaseBaseUrl =
			"https://github.com/MZ1987Records/obs-delay-stream/releases/tag/v";
		// 無限ループ的なリダイレクト追従を避けるための上限。
		static constexpr int kMaxRedirects = 8;

		// WinINet ハンドルをスコープ終端で確実に閉じる。
		class ScopedInternetHandle {
		public:

			/// 既存の WinINet ハンドルを受け取り、所有権を保持する。
			explicit ScopedInternetHandle(HINTERNET h = nullptr) : h_(h) {}
			~ScopedInternetHandle() {
				if (h_) InternetCloseHandle(h_);
			}
			ScopedInternetHandle(const ScopedInternetHandle &)            = delete;
			ScopedInternetHandle &operator=(const ScopedInternetHandle &) = delete;
			ScopedInternetHandle(ScopedInternetHandle &&other) noexcept : h_(other.h_) {
				other.h_ = nullptr;
			}
			ScopedInternetHandle &operator=(ScopedInternetHandle &&other) noexcept {
				if (this == &other) return *this;
				if (h_) InternetCloseHandle(h_);
				h_       = other.h_;
				other.h_ = nullptr;
				return *this;
			}

			/// 内部で所有している WinINet ハンドルを取得する。
			HINTERNET get() const { return h_; }

			/// HTTP ステータスコードを数値で取得する。
			bool query_status_code(DWORD &out_status) const;

			/// 指定クエリの HTTP ヘッダー値を文字列で取得する。
			bool query_header_string(DWORD query, std::string &out) const;

			/// 実際にアクセスした URL（リダイレクト後を含む）を取得する。
			bool query_effective_url(std::string &out_url) const;

			/// レスポンス本文の先頭のみを読み込む。
			bool read_response_prefix(std::string &out_html) const;

		private:

			/// このインスタンスが所有する WinINet ハンドル。
			HINTERNET h_ = nullptr;
		};

		bool ScopedInternetHandle::query_status_code(DWORD &out_status) const {
			out_status = 0;
			DWORD size = sizeof(out_status);
			return HttpQueryInfoA(h_, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &out_status, &size, nullptr) == TRUE;
		}

		bool ScopedInternetHandle::query_header_string(DWORD query, std::string &out) const {
			out.clear();
			DWORD size = 0;
			if (HttpQueryInfoA(h_, query, nullptr, &size, nullptr)) return false;
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
			std::string buf(size, '\0');
			if (!HttpQueryInfoA(h_, query, buf.data(), &size, nullptr)) return false;
			if (!buf.empty() && buf.back() == '\0') buf.pop_back();
			while (!buf.empty() &&
				   (buf.back() == '\r' || buf.back() == '\n' || std::isspace((unsigned char)buf.back()))) {
				buf.pop_back();
			}
			size_t begin = 0;
			while (begin < buf.size() && std::isspace((unsigned char)buf[begin]))
				++begin;
			if (begin > 0) buf.erase(0, begin);
			out = std::move(buf);
			return true;
		}

		bool ScopedInternetHandle::query_effective_url(std::string &out_url) const {
			out_url.clear();
			DWORD size = 0;
			if (InternetQueryOptionA(h_, INTERNET_OPTION_URL, nullptr, &size)) return false;
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
			std::string buf(size, '\0');
			if (!InternetQueryOptionA(h_, INTERNET_OPTION_URL, buf.data(), &size)) return false;
			if (!buf.empty() && buf.back() == '\0') buf.pop_back();
			out_url = std::move(buf);
			return true;
		}

		bool ScopedInternetHandle::read_response_prefix(std::string &out_html) const {
			out_html.clear();
			// 全文は不要なので、メモリ使用量を抑えるため先頭だけ読む。
			constexpr size_t kMaxRead = 256 * 1024;
			char             buf[4096];
			DWORD            read = 0;
			while (out_html.size() < kMaxRead &&
				   InternetReadFile(h_, buf, sizeof(buf), &read) == TRUE &&
				   read > 0) {
				size_t remain     = kMaxRead - out_html.size();
				size_t append_len = std::min(remain, (size_t)read);
				out_html.append(buf, append_len);
			}
			return !out_html.empty();
		}

		/// バージョン文字列を "x.y.z" 形式へ正規化する。
		std::string normalize_version_string(const std::string &raw) {
			size_t i = 0;
			while (i < raw.size() && std::isspace((unsigned char)raw[i]))
				++i;
			if (i < raw.size() && (raw[i] == 'v' || raw[i] == 'V')) ++i;
			size_t start = i;
			while (i < raw.size()) {
				char c = raw[i];
				if ((c >= '0' && c <= '9') || c == '.') {
					++i;
					continue;
				}
				break;
			}
			std::string out = raw.substr(start, i - start);
			while (!out.empty() && out.back() == '.')
				out.pop_back();
			return out;
		}

		/// リリースURLからタグバージョンを抽出する。
		bool extract_version_from_release_url(const std::string &url, std::string &out_version) {
			out_version.clear();
			const std::string key = "/releases/tag/";
			size_t            pos = url.find(key);
			if (pos == std::string::npos) return false;
			std::string tag = url.substr(pos + key.size());
			size_t      end = tag.find_first_of("/?#");
			if (end != std::string::npos) tag.resize(end);
			std::string ver = normalize_version_string(tag);
			if (ver.empty()) return false;
			out_version = std::move(ver);
			return true;
		}

		/// HTML断片から /releases/tag/... を探してバージョンを抽出する。
		bool extract_version_from_html(const std::string &html, std::string &out_version) {
			out_version.clear();
			const std::string key = "/releases/tag/";
			size_t            pos = html.find(key);
			if (pos == std::string::npos) return false;
			std::string tag = html.substr(pos + key.size());
			size_t      end = tag.find_first_of("\"'?#<>& \r\n\t");
			if (end != std::string::npos) tag.resize(end);
			std::string ver = normalize_version_string(tag);
			if (ver.empty()) return false;
			out_version = std::move(ver);
			return true;
		}

		/// URL から scheme://host[:port] を取り出す。
		std::string url_origin(const std::string &base) {
			size_t scheme_end = base.find("://");
			if (scheme_end == std::string::npos) return "";
			size_t host_begin = scheme_end + 3;
			size_t host_end   = base.find('/', host_begin);
			if (host_end == std::string::npos) return base;
			return base.substr(0, host_end);
		}

		/// URL のディレクトリ部分（末尾 `/` 含む）を返す。
		std::string url_base_dir(const std::string &base) {
			size_t      q        = base.find_first_of("?#");
			std::string no_query = (q == std::string::npos) ? base : base.substr(0, q);
			size_t      slash    = no_query.find_last_of('/');
			if (slash == std::string::npos) return no_query + "/";
			return no_query.substr(0, slash + 1);
		}

		/// リダイレクト先の Location を絶対URLへ解決する。
		std::string resolve_location_url(const std::string &current_url, const std::string &location) {
			if (location.empty()) return "";
			if (location.rfind("https://", 0) == 0 || location.rfind("http://", 0) == 0) {
				return location;
			}
			size_t      scheme_end = current_url.find("://");
			std::string scheme     = (scheme_end == std::string::npos)
										 ? "https"
										 : current_url.substr(0, scheme_end);
			if (location.rfind("//", 0) == 0) {
				return scheme + ":" + location;
			}
			if (location[0] == '/') {
				std::string origin = url_origin(current_url);
				return origin.empty() ? "" : (origin + location);
			}
			return url_base_dir(current_url) + location;
		}

		/// バージョン文字列を比較可能な数値配列へ分解する。
		bool parse_version_numbers(const std::string &raw, std::vector<int> &out) {
			out.clear();
			std::string normalized = normalize_version_string(raw);
			if (normalized.empty()) return false;
			size_t pos = 0;
			while (pos < normalized.size()) {
				size_t      dot   = normalized.find('.', pos);
				std::string token = (dot == std::string::npos)
										? normalized.substr(pos)
										: normalized.substr(pos, dot - pos);
				if (token.empty()) return false;
				for (char c : token) {
					if (c < '0' || c > '9') return false;
				}
				out.push_back(std::atoi(token.c_str()));
				if (dot == std::string::npos) break;
				pos = dot + 1;
			}
			return !out.empty();
		}

		/// バージョン配列を辞書順で比較する。
		int compare_versions(const std::vector<int> &a, const std::vector<int> &b) {
			const size_t n = std::max(a.size(), b.size());
			for (size_t i = 0; i < n; ++i) {
				int av = (i < a.size()) ? a[i] : 0;
				int bv = (i < b.size()) ? b[i] : 0;
				if (av < bv) return -1;
				if (av > bv) return 1;
			}
			return 0;
		}

	} // namespace

	bool fetch_latest_release_info(LatestReleaseInfo &out) {
		out = LatestReleaseInfo{};
		ScopedInternetHandle session(InternetOpenA(
			"obs-delay-stream-update-check",
			INTERNET_OPEN_TYPE_PRECONFIG,
			nullptr,
			nullptr,
			0));
		if (!session.get()) {
			out.error = "InternetOpenA failed";
			return false;
		}

		std::string current_url = kLatestReleaseUrl;
		for (int i = 0; i < kMaxRedirects; ++i) {
			DWORD              flags = INTERNET_FLAG_RELOAD |
									   INTERNET_FLAG_NO_CACHE_WRITE |
									   INTERNET_FLAG_PRAGMA_NOCACHE |
									   INTERNET_FLAG_NO_UI |
									   INTERNET_FLAG_NO_AUTO_REDIRECT;
			static const char *kHeaders =
				"User-Agent: obs-delay-stream\r\n"
				"Accept: text/html\r\n";
			ScopedInternetHandle req(InternetOpenUrlA(
				session.get(),
				current_url.c_str(),
				kHeaders,
				-1,
				flags,
				0));
			if (!req.get()) {
				out.error = "InternetOpenUrlA failed";
				return false;
			}

			std::string effective_url;
			if (!req.query_effective_url(effective_url) || effective_url.empty()) {
				effective_url = current_url;
			}

			DWORD status = 0;
			if (!req.query_status_code(status)) {
				out.error = "HttpQueryInfoA(status) failed";
				return false;
			}

			if (status >= 300 && status < 400) {
				std::string location;
				if (!req.query_header_string(HTTP_QUERY_LOCATION, location)) {
					out.error = "redirect location header not found";
					return false;
				}
				std::string next = resolve_location_url(effective_url, location);
				if (next.empty()) {
					out.error = "redirect location resolve failed";
					return false;
				}
				current_url = std::move(next);
				continue;
			}

			if (status != 200) {
				out.error = "unexpected HTTP status";
				return false;
			}

			std::string version;
			if (!extract_version_from_release_url(effective_url, version)) {
				std::string html;
				if (req.read_response_prefix(html)) {
					extract_version_from_html(html, version);
				}
			}
			if (version.empty()) {
				out.error = "latest version parse failed";
				return false;
			}

			out.latest_version = std::move(version);
			std::string url_version;
			if (extract_version_from_release_url(effective_url, url_version)) {
				out.release_url = effective_url;
				return true;
			}

			out.release_url =
				std::string(kTagReleaseBaseUrl) + out.latest_version;
			return true;
		}

		out.error = "too many redirects";
		return false;
	}

	bool is_newer_version(const std::string &latest, const std::string &current) {
		std::vector<int> lnums;
		std::vector<int> cnums;
		if (!parse_version_numbers(latest, lnums)) return false;
		if (!parse_version_numbers(current, cnums)) return false;
		return compare_versions(cnums, lnums) < 0;
	}

} // namespace ods::plugin
