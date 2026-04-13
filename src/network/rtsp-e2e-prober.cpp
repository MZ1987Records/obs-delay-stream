#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ShlObj.h>
#include <urlmon.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Urlmon.lib")

#include "network/rtsp-e2e-prober.hpp"

#include "core/constants.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <obs-module.h>
#include <string>
#include <thread>
#include <vector>

namespace ods::network {

	using namespace ods::core;

	namespace {

		void close_handle(HANDLE &h) {
			if (h && h != INVALID_HANDLE_VALUE) {
				CloseHandle(h);
			}
			h = INVALID_HANDLE_VALUE;
		}

		constexpr char kFfmpegBtbnZipUrl[] =
			"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";
		constexpr char kFfmpegZipName[]         = "ffmpeg-master-latest-win64-gpl.zip";
		constexpr char kFfmpegExpandedDirName[] = "ffmpeg-master-latest-win64-gpl";

		std::string escape_cmd_arg(const std::string &text) {
			std::string escaped;
			escaped.reserve(text.size() + 4);
			for (char c : text) {
				if (c == '"')
					escaped += "\\\"";
				else
					escaped += c;
			}
			return escaped;
		}

		std::string get_local_appdata_dir() {
			char base[MAX_PATH] = {};
			if (SUCCEEDED(SHGetFolderPathA(
					nullptr,
					CSIDL_LOCAL_APPDATA,
					nullptr,
					SHGFP_TYPE_CURRENT,
					base))) {
				return std::string(base);
			}
			return "";
		}

		std::string get_temp_dir() {
			char  tmp_path[MAX_PATH] = {};
			DWORD n                  = GetTempPathA(MAX_PATH, tmp_path);
			if (n > 0 && n < MAX_PATH) return std::string(tmp_path);
			return "";
		}

		bool ensure_dir(const std::string &path) {
			if (path.empty()) return false;
			const int r = SHCreateDirectoryExA(nullptr, path.c_str(), nullptr);
			return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
		}

		bool file_exists(const std::string &path) {
			const DWORD attr = GetFileAttributesA(path.c_str());
			return attr != INVALID_FILE_ATTRIBUTES &&
				   (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}

		bool dir_exists(const std::string &path) {
			const DWORD attr = GetFileAttributesA(path.c_str());
			return attr != INVALID_FILE_ATTRIBUTES &&
				   (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}

		std::string get_default_ffmpeg_base_dir() {
			std::string base = get_local_appdata_dir();
			if (!base.empty()) {
				const std::string dir = base + "\\obs-delay-stream\\bin\\ffmpeg";
				ensure_dir(base + "\\obs-delay-stream");
				ensure_dir(base + "\\obs-delay-stream\\bin");
				ensure_dir(dir);
				return dir;
			}
			std::string tmp = get_temp_dir();
			if (tmp.empty()) tmp = "C:\\Temp\\";
			if (!tmp.empty() && tmp.back() == '\\') tmp.pop_back();
			const std::string dir = tmp + "\\obs-delay-stream\\bin\\ffmpeg";
			ensure_dir(tmp + "\\obs-delay-stream");
			ensure_dir(tmp + "\\obs-delay-stream\\bin");
			ensure_dir(dir);
			return dir;
		}

		std::string get_default_ffmpeg_auto_path() {
			return get_default_ffmpeg_base_dir() +
				   "\\" + kFfmpegExpandedDirName + "\\bin\\ffmpeg.exe";
		}

		bool find_ffmpeg_recursive(const std::string &dir, int depth, std::string &out);

		bool find_auto_ffmpeg_in_default_dir(std::string &out) {
			const std::string base_dir     = get_default_ffmpeg_base_dir();
			const std::string expected_exe = get_default_ffmpeg_auto_path();
			if (file_exists(expected_exe)) {
				out = expected_exe;
				return true;
			}
			return find_ffmpeg_recursive(base_dir, 6, out);
		}

		bool find_ffmpeg_recursive(const std::string &dir, int depth, std::string &out) {
			if (depth < 0 || !dir_exists(dir)) return false;
			WIN32_FIND_DATAA  fd{};
			const std::string pattern = dir + "\\*";
			HANDLE            h       = FindFirstFileA(pattern.c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) return false;

			do {
				const char *name = fd.cFileName;
				if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;

				const std::string full = dir + "\\" + name;
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
					if (find_ffmpeg_recursive(full, depth - 1, out)) {
						FindClose(h);
						return true;
					}
					continue;
				}
				if (_stricmp(name, "ffmpeg.exe") == 0) {
					out = full;
					FindClose(h);
					return true;
				}
			} while (FindNextFileA(h, &fd) == TRUE);
			FindClose(h);
			return false;
		}

		bool search_path_ffmpeg(std::string &out) {
			char  path[MAX_PATH] = {};
			DWORD n              = SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, path, nullptr);
			if (n > 0 && n < MAX_PATH) {
				out = path;
				return true;
			}
			return false;
		}

		std::string get_allowed_env_value(const std::string &name) {
			std::string upper = name;
			std::transform(
				upper.begin(),
				upper.end(),
				upper.begin(),
				[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			if (upper == "LOCALAPPDATA") return get_local_appdata_dir();
			if (upper == "TEMP" || upper == "TMP") return get_temp_dir();
			return "";
		}

		std::string expand_allowed_env_vars(const std::string &path) {
			std::string out;
			out.reserve(path.size());
			size_t i = 0;
			while (i < path.size()) {
				const size_t p = path.find('%', i);
				if (p == std::string::npos || p + 1 >= path.size()) {
					out.append(path.substr(i));
					break;
				}
				const size_t q = path.find('%', p + 1);
				if (q == std::string::npos) {
					out.append(path.substr(i));
					break;
				}
				out.append(path.substr(i, p - i));
				const std::string var = path.substr(p + 1, q - p - 1);
				const std::string val = get_allowed_env_value(var);
				if (!val.empty()) {
					out.append(val);
				} else {
					out.append(path.substr(p, q - p + 1));
				}
				i = q + 1;
			}
			return out;
		}

		std::string ps_single_quote(const std::string &s) {
			std::string out;
			out.reserve(s.size() + 8);
			out.push_back('\'');
			for (char c : s) {
				if (c == '\'')
					out += "''";
				else
					out.push_back(c);
			}
			out.push_back('\'');
			return out;
		}

		bool expand_zip_powershell(const std::string &zip_path,
								   const std::string &dest_dir,
								   std::string       &err) {
			const std::string command =
				"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \""
				"try { Expand-Archive -LiteralPath " +
				ps_single_quote(zip_path) +
				" -DestinationPath " + ps_single_quote(dest_dir) +
				" -Force; exit 0 } catch { exit 1 }\"";
			std::vector<char> cmd(command.begin(), command.end());
			cmd.push_back('\0');

			STARTUPINFOA        si{};
			PROCESS_INFORMATION pi{};
			si.cb         = sizeof(si);
			const BOOL ok = CreateProcessA(
				nullptr,
				cmd.data(),
				nullptr,
				nullptr,
				FALSE,
				CREATE_NO_WINDOW,
				nullptr,
				nullptr,
				&si,
				&pi);
			if (!ok) {
				err = "ZIP 展開のための PowerShell 起動に失敗しました。";
				return false;
			}

			const DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
			if (wait != WAIT_OBJECT_0) {
				TerminateProcess(pi.hProcess, 1);
				err = "ffmpeg ZIP 展開がタイムアウトしました。";
				close_handle(pi.hThread);
				close_handle(pi.hProcess);
				return false;
			}
			DWORD code = 1;
			GetExitCodeProcess(pi.hProcess, &code);
			close_handle(pi.hThread);
			close_handle(pi.hProcess);

			if (code != 0) {
				err = "ffmpeg ZIP の展開に失敗しました。";
				return false;
			}
			return true;
		}

		bool ensure_auto_ffmpeg_binary(std::string &out, std::string &err) {
			if (find_auto_ffmpeg_in_default_dir(out)) return true;

			const std::string base_dir = get_default_ffmpeg_base_dir();
			const std::string zip_path = base_dir + "\\" + kFfmpegZipName;
			blog(LOG_INFO, "[obs-delay-stream] ffmpeg auto-download: %s", zip_path.c_str());
			const HRESULT hr = URLDownloadToFileA(nullptr, kFfmpegBtbnZipUrl, zip_path.c_str(), 0, nullptr);
			if (FAILED(hr)) {
				char hr_hex[32] = {};
				std::snprintf(hr_hex, sizeof(hr_hex), "0x%08lx", (unsigned long)hr);
				err = std::string("ffmpeg の自動ダウンロードに失敗しました (") + hr_hex +
					  ")。ネットワーク接続を確認するか、%PATH% から解決を使用してください。";
				return false;
			}
			if (!file_exists(zip_path)) {
				err = "ffmpeg のダウンロード後に ZIP ファイルが見つかりませんでした。";
				return false;
			}
			if (!expand_zip_powershell(zip_path, base_dir, err)) {
				return false;
			}
			DeleteFileA(zip_path.c_str());

			if (find_auto_ffmpeg_in_default_dir(out)) return true;
			err = "ffmpeg の自動展開後に ffmpeg.exe が見つかりませんでした。";
			return false;
		}

		bool resolve_ffmpeg_path(const std::string &requested, std::string &out, std::string &err) {
			if (!requested.empty() && _stricmp(requested.c_str(), "%PATH%") == 0) {
				if (search_path_ffmpeg(out)) return true;
				err = "ffmpeg.exe が %PATH% から見つかりません。";
				return false;
			}

			const bool is_auto =
				requested.empty() || _stricmp(requested.c_str(), "auto") == 0;
			if (!is_auto) {
				const std::string expanded = expand_allowed_env_vars(requested);
				if (file_exists(expanded)) {
					out = expanded;
					return true;
				}
				err = "ffmpeg.exe が見つかりません。パスを確認するか、解決モードを変更してください。";
				return false;
			}

			return ensure_auto_ffmpeg_binary(out, err);
		}

		std::string first_non_empty_line(const std::string &text) {
			size_t pos = 0;
			while (pos < text.size()) {
				size_t end = text.find_first_of("\r\n", pos);
				if (end == std::string::npos) end = text.size();
				std::string line  = text.substr(pos, end - pos);
				const auto  begin = line.find_first_not_of(" \t");
				if (begin != std::string::npos) {
					const auto last = line.find_last_not_of(" \t");
					return line.substr(begin, last - begin + 1);
				}
				if (end >= text.size()) break;
				pos = text.find_first_not_of("\r\n", end);
				if (pos == std::string::npos) break;
			}
			return "";
		}

		std::string build_ffmpeg_cmdline(const std::string &ffmpeg_exe_path, const std::string &rtsp_url) {
			const int         timeout_us = RTSP_E2E_READY_TIMEOUT_S * 1000 * 1000;
			const std::string url        = escape_cmd_arg(rtsp_url);
			const std::string exe        = escape_cmd_arg(ffmpeg_exe_path);
			return "\"" + exe + "\" -nostdin -v error -rtsp_transport tcp -timeout " +
				   std::to_string(timeout_us) + " -i \"" + url +
				   "\" -map 0:a:0 -vn -f s16le -ar 48000 -ac 1 -";
		}

		bool launch_ffmpeg_process(const std::string   &ffmpeg_exe_path,
								   const std::string   &rtsp_url,
								   PROCESS_INFORMATION &pi,
								   HANDLE              &stdout_read,
								   HANDLE              &stderr_read,
								   std::string         &launch_error) {
			SECURITY_ATTRIBUTES sa{};
			sa.nLength              = sizeof(sa);
			sa.lpSecurityDescriptor = nullptr;
			sa.bInheritHandle       = TRUE;

			HANDLE stdout_write = INVALID_HANDLE_VALUE;
			HANDLE stderr_write = INVALID_HANDLE_VALUE;
			stdout_read         = INVALID_HANDLE_VALUE;
			stderr_read         = INVALID_HANDLE_VALUE;
			pi                  = PROCESS_INFORMATION{};

			if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
				launch_error = "標準出力パイプの作成に失敗しました。";
				return false;
			}
			if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
				close_handle(stdout_read);
				close_handle(stdout_write);
				launch_error = "標準出力パイプ属性の設定に失敗しました。";
				return false;
			}
			if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
				close_handle(stdout_read);
				close_handle(stdout_write);
				launch_error = "標準エラー出力パイプの作成に失敗しました。";
				return false;
			}
			if (!SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
				close_handle(stdout_read);
				close_handle(stdout_write);
				close_handle(stderr_read);
				close_handle(stderr_write);
				launch_error = "標準エラー出力パイプ属性の設定に失敗しました。";
				return false;
			}

			STARTUPINFOA si{};
			si.cb         = sizeof(si);
			si.dwFlags    = STARTF_USESTDHANDLES;
			si.hStdOutput = stdout_write;
			si.hStdError  = stderr_write;
			si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

			std::string       cmdline = build_ffmpeg_cmdline(ffmpeg_exe_path, rtsp_url);
			std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
			cmdline_buf.push_back('\0');

			BOOL ok = CreateProcessA(
				ffmpeg_exe_path.c_str(),
				cmdline_buf.data(),
				nullptr,
				nullptr,
				TRUE,
				CREATE_NO_WINDOW,
				nullptr,
				nullptr,
				&si,
				&pi);

			close_handle(stdout_write);
			close_handle(stderr_write);

			if (!ok) {
				char  msg[256] = {};
				DWORD err      = GetLastError();
				FormatMessageA(
					FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					nullptr,
					err,
					0,
					msg,
					sizeof(msg) - 1,
					nullptr);
				launch_error = std::string("ffmpeg 起動失敗: ") + msg;
				close_handle(stdout_read);
				close_handle(stderr_read);
				return false;
			}

			return true;
		}

		void drain_text_pipe(HANDLE pipe, std::string &dst) {
			if (!pipe || pipe == INVALID_HANDLE_VALUE) return;
			DWORD avail = 0;
			while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
				char  buf[512];
				DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buf)));
				DWORD read    = 0;
				if (!ReadFile(pipe, buf, to_read, &read, nullptr) || read == 0) break;
				dst.append(buf, buf + read);
			}
		}

		bool process_alive(HANDLE process) {
			if (!process || process == INVALID_HANDLE_VALUE) return false;
			return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
		}

	} // namespace

	RtspE2eProber::RtspE2eProber()
		: detector_(probe_signal_) {}

	RtspE2eProber::~RtspE2eProber() {
		cancel();
	}

	bool RtspE2eProber::get_auto_ffmpeg_path_if_exists(std::string &out) {
		return find_auto_ffmpeg_in_default_dir(out);
	}

	bool RtspE2eProber::ensure_auto_ffmpeg_path(std::string &out, std::string &err) {
		return ensure_auto_ffmpeg_binary(out, err);
	}

	bool RtspE2eProber::start(const std::string &rtsp_url, const std::string &ffmpeg_path_hint) {
		if (running_.load(std::memory_order_acquire)) return false;
		if (rtsp_url.empty()) return false;

		cancel();
		{
			std::lock_guard<std::mutex> lk(mtx_);
			last_result_ = RtspE2eResult{};
		}
		{
			std::lock_guard<std::mutex> lk(t0_mtx_);
			t0_ = std::chrono::steady_clock::time_point{};
		}
		rtsp_url_         = rtsp_url;
		ffmpeg_path_hint_ = ffmpeg_path_hint.empty() ? "auto" : ffmpeg_path_hint;
		measure_sets_     = RTSP_E2E_MEASURE_SETS_DEFAULT;
		impulse_sent_.store(false, std::memory_order_release);
		running_.store(true, std::memory_order_release);

		worker_ = std::thread([this]() { worker_loop(); });
		return true;
	}

	void RtspE2eProber::notify_impulse_sent(std::chrono::steady_clock::time_point t0) {
		{
			std::lock_guard<std::mutex> lk(t0_mtx_);
			t0_ = t0;
		}
		impulse_sent_.store(true, std::memory_order_release);
	}

	void RtspE2eProber::cancel() {
		running_.store(false, std::memory_order_release);
		if (worker_.joinable()) worker_.join();
	}

	bool RtspE2eProber::is_running() const {
		return running_.load(std::memory_order_acquire);
	}

	RtspE2eResult RtspE2eProber::last_result() const {
		std::lock_guard<std::mutex> lk(mtx_);
		return last_result_;
	}

	RtspE2eResult RtspE2eProber::run_single_probe(const std::string &ffmpeg_exe_path) {
		detector_.reset();
		RtspE2eResult       result;
		std::string         ffmpeg_stderr;
		PROCESS_INFORMATION pi{};
		HANDLE              stdout_read = INVALID_HANDLE_VALUE;
		HANDLE              stderr_read = INVALID_HANDLE_VALUE;
		std::string         launch_error;
		if (!launch_ffmpeg_process(ffmpeg_exe_path, rtsp_url_, pi, stdout_read, stderr_read, launch_error)) {
			result.error_msg = launch_error.empty() ? "ffmpeg を起動できませんでした。" : launch_error;
			return result;
		}

		std::array<int16_t, 2048>             samples{};
		std::array<char, sizeof(samples)>     raw{};
		bool                                  ready_notified   = false;
		const auto                            attempt_start_tp = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point ready_tp{};

		while (running_.load(std::memory_order_acquire)) {
			drain_text_pipe(stderr_read, ffmpeg_stderr);

			if (!ready_notified) {
				const auto now = std::chrono::steady_clock::now();
				if (now - attempt_start_tp > std::chrono::seconds(RTSP_E2E_READY_TIMEOUT_S)) {
					result.error_msg = "RTSP 音声の受信開始待機がタイムアウトしました。";
					break;
				}
			}

			DWORD avail    = 0;
			bool  can_peek = PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &avail, nullptr) == TRUE;
			if (!can_peek) break;

			if (avail < sizeof(int16_t)) {
				if (!process_alive(pi.hProcess) && avail == 0) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}

			DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(raw.size()));
			to_read &= ~static_cast<DWORD>(1);
			if (to_read == 0) continue;

			DWORD bytes_read = 0;
			if (!ReadFile(stdout_read, raw.data(), to_read, &bytes_read, nullptr) || bytes_read < sizeof(int16_t)) {
				if (!process_alive(pi.hProcess)) break;
				continue;
			}

			const size_t n = bytes_read / sizeof(int16_t);
			std::memcpy(samples.data(), raw.data(), n * sizeof(int16_t));

			if (!ready_notified) {
				ready_notified = true;
				ready_tp       = std::chrono::steady_clock::now();
				if (on_ready) on_ready();
			}

			if (!impulse_sent_.load(std::memory_order_acquire)) {
				const auto now = std::chrono::steady_clock::now();
				if (now - ready_tp > std::chrono::seconds(RTSP_E2E_READY_TIMEOUT_S)) {
					result.error_msg = "プローブ注入待機がタイムアウトしました。";
					break;
				}
				continue;
			}

			std::chrono::steady_clock::time_point t0;
			{
				std::lock_guard<std::mutex> lk(t0_mtx_);
				t0 = t0_;
			}
			if (t0 == std::chrono::steady_clock::time_point{}) continue;

			auto det = detector_.feed(samples.data(), n);
			if (det.detected) {
				const auto t1 = std::chrono::steady_clock::now();
				// サブサンプル精度のタイミング補正（放物線ピーク補間済み）
				const double correction_s =
					(static_cast<double>(n) - det.peak_offset) / 48000.0;
				const auto t_detected = t1 - std::chrono::duration_cast<
												 std::chrono::steady_clock::duration>(
												 std::chrono::duration<double>(correction_s));
				result.valid          = true;
				result.latency_ms =
					std::chrono::duration<double, std::milli>(t_detected - t0).count();
				break;
			}

			const auto now = std::chrono::steady_clock::now();
			if (now - t0 > std::chrono::seconds(RTSP_E2E_TIMEOUT_S)) {
				result.error_msg = "プローブ検出がタイムアウトしました。";
				break;
			}
		}

		if (!running_.load(std::memory_order_acquire) && process_alive(pi.hProcess)) {
			TerminateProcess(pi.hProcess, 1);
		}
		WaitForSingleObject(pi.hProcess, 3000);
		drain_text_pipe(stderr_read, ffmpeg_stderr);
		close_handle(stdout_read);
		close_handle(stderr_read);
		close_handle(pi.hThread);
		close_handle(pi.hProcess);

		if (!result.valid && result.error_msg.empty()) {
			if (!running_.load(std::memory_order_acquire)) {
				result.error_msg = "計測はキャンセルされました。";
			} else if (!ready_notified) {
				result.error_msg = "RTSP 音声の受信開始を確認できませんでした。";
			} else {
				result.error_msg = "プローブを検出できませんでした。";
			}
		}
		if (!result.valid) {
			const std::string ffmpeg_err = first_non_empty_line(ffmpeg_stderr);
			if (!ffmpeg_err.empty()) result.error_msg += "\nffmpeg: " + ffmpeg_err;
		}
		return result;
	}

	void RtspE2eProber::worker_loop() {
		RtspE2eResult       result;
		std::vector<double> latencies_ms;
		std::string         last_error;
		std::string         ffmpeg_exe_path;
		std::string         resolve_error;

		if (!resolve_ffmpeg_path(ffmpeg_path_hint_, ffmpeg_exe_path, resolve_error)) {
			result.error_msg = resolve_error.empty()
								   ? "ffmpeg.exe が見つかりません。"
								   : resolve_error;
			running_.store(false, std::memory_order_release);
			{
				std::lock_guard<std::mutex> lk(mtx_);
				last_result_ = result;
			}
			if (on_result) on_result(result);
			return;
		}

		const int sets = std::max(1, measure_sets_);
		latencies_ms.reserve(static_cast<size_t>(sets));
		if (on_progress) on_progress(0, sets);
		for (int i = 0; i < sets && running_.load(std::memory_order_acquire); ++i) {
			impulse_sent_.store(false, std::memory_order_release);
			{
				std::lock_guard<std::mutex> lk(t0_mtx_);
				t0_ = std::chrono::steady_clock::time_point{};
			}

			RtspE2eResult one = run_single_probe(ffmpeg_exe_path);
			if (one.valid) {
				latencies_ms.push_back(one.latency_ms);
			} else {
				last_error = one.error_msg;
				if (one.error_msg.find("ffmpeg 起動失敗") != std::string::npos ||
					one.error_msg.find("ffmpeg を起動できませんでした。") != std::string::npos) {
					break;
				}
				if (!running_.load(std::memory_order_acquire)) break;
			}
			if (on_progress) on_progress(i + 1, sets);
		}

		// 1 セットの失敗を許容する（RTSP 再公開の競合で散発的に 404 になるため）
		const int min_required = std::max(1, sets - 1);

		if (static_cast<int>(latencies_ms.size()) >= min_required) {
			std::sort(latencies_ms.begin(), latencies_ms.end());
			const size_t mid      = latencies_ms.size() / 2;
			result.valid          = true;
			result.min_latency_ms = latencies_ms.front();
			result.max_latency_ms = latencies_ms.back();
			if ((latencies_ms.size() % 2) == 0) {
				result.latency_ms = (latencies_ms[mid - 1] + latencies_ms[mid]) / 2.0;
			} else {
				result.latency_ms = latencies_ms[mid];
			}

			// MAD 安定性チェック: ばらつきが大きすぎる場合は失敗扱い
			std::vector<double> abs_devs;
			abs_devs.reserve(latencies_ms.size());
			for (double v : latencies_ms)
				abs_devs.push_back(std::abs(v - result.latency_ms));
			std::sort(abs_devs.begin(), abs_devs.end());
			const double mad = abs_devs[abs_devs.size() / 2];
			if (mad > 4.0) {
				result.valid     = false;
				result.error_msg = "計測結果のばらつきが大きすぎます (MAD " +
								   std::to_string(static_cast<int>(std::round(mad))) + " ms)。";
			}
		} else if (!running_.load(std::memory_order_acquire)) {
			result.error_msg = "計測はキャンセルされました。";
		} else {
			result.error_msg = "RTSP E2E 計測に失敗しました。";
			result.error_msg += "（成功 " + std::to_string(latencies_ms.size()) +
								"/" + std::to_string(sets) + " セット）";
			if (!last_error.empty()) {
				result.error_msg += "\n" + last_error;
			}
		}

		running_.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lk(mtx_);
			last_result_ = result;
		}
		if (on_result) on_result(result);
	}

} // namespace ods::network
