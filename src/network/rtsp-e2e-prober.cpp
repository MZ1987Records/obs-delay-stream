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

#include "core/approved-binaries.hpp"
#include "core/binary-verify.hpp"
#include "core/child-process.hpp"
#include "core/constants.hpp"
#include "core/string-format.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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

		// 承認版の導入前に master-latest を自動取得・展開していたディレクトリ名。
		constexpr char kLegacyFfmpegExpandedDirName[] = "ffmpeg-master-latest-win64-gpl";

		std::string normalize_rtsp_url_for_ffmpeg(const std::string &rtsp_url) {
			constexpr char kRtsptScheme[] = "rtspt://";
			if (_strnicmp(rtsp_url.c_str(), kRtsptScheme, sizeof(kRtsptScheme) - 1) == 0) {
				return "rtsp://" + rtsp_url.substr(sizeof(kRtsptScheme) - 1);
			}
			return rtsp_url;
		}

		// FFmpeg の -i は file: や http: など任意の入力を開けるため、RTSP の scheme だけを渡す。
		bool is_allowed_rtsp_url(const std::string &rtsp_url) {
			for (const char *scheme : {"rtsp://", "rtsps://", "rtspt://"}) {
				if (_strnicmp(rtsp_url.c_str(), scheme, std::strlen(scheme)) == 0) return true;
			}
			return false;
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
			// 承認版 ZIP の展開先は版名入りのディレクトリになる。ディレクトリを再帰探索して
			// 別の ffmpeg.exe を拾わないよう、実行パスは承認一覧の値に固定する。
			return get_default_ffmpeg_base_dir() + "\\" + kApprovedFfmpeg.exe_in_archive;
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

		bool ensure_auto_ffmpeg_binary(std::string &out, std::string &err) {
			const auto &a = kApprovedFfmpeg;
			out           = get_default_ffmpeg_auto_path();
			if (verify_file(out, a.exe_size, a.exe_sha256) == VerifyResult::Ok) return true;

			const std::string base_dir = get_default_ffmpeg_base_dir();
			const std::string zip_path = base_dir + "\\ffmpeg-" + a.version + ".zip";
			blog(LOG_INFO, "[obs-delay-stream] ffmpeg auto-download: %s", a.version);
			std::string reason;
			if (!download_verified(a.url, zip_path, a.download_size, a.download_sha256, reason)) {
				err = std::string("FFmpeg ") + a.version + " の自動取得: " + reason;
				return false;
			}
			// ZIP は承認値と一致したものだけから、ffmpeg.exe 1 つだけを取り出し、最後に exe を検証する。
			const bool expanded = extract_zip_entry(zip_path, base_dir, a.exe_in_archive, err);
			DeleteFileA(zip_path.c_str());
			if (!expanded) return false;
			if (verify_file(out, a.exe_size, a.exe_sha256) != VerifyResult::Ok) {
				err = "展開した ffmpeg.exe が承認版と一致しません（改変または破損の可能性があります）。";
				return false;
			}
			return true;
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
			const std::string url        = quote_windows_arg(rtsp_url);
			const std::string exe        = quote_windows_arg(ffmpeg_exe_path);
			// 低遅延受信フラグ: 既定の解析バッファ (analyzeduration 5s / probesize 5MB) や
			// デマックス・RTP リオーダーバッファが E2E 計測値に数秒乗るのを防ぐ。
			//   nobuffer / low_delay            … デマックス・デコードのバッファリングを無効化
			//   probesize 32 / analyzeduration 0 … RTSP は SDP で codec 既知のため最小解析でよい
			//   max_delay 0 / reorder_queue_size 0 … TCP 伝送では並べ替え不要
			//   flush_packets 1                 … パイプ出力を都度フラッシュし検出遅れを抑える
			// 入力の制限: 受信するのは配信者自身の OBS が配信サーバーへ送った音声（RTMP 用の AAC、
			// 構成によって Opus）だけ。配信サーバーや暗号化されていない rtsp/rtspt 経路から細工した
			// ストリームを送られても、FFmpeg の他の機能を使わせない。
			//   f rtsp                          … 入力形式を RTSP に固定する
			//   allowed_media_types audio       … SETUP を音声だけにし、映像は受信も解析もしない
			//   protocol_whitelist tcp,tls      … RTSP over TCP が開くのは tcp、rtsps は tls（その下で tcp）だけ
			//   codec_whitelist aac,opus        … ストリーム解析と復号に使うデコーダーを限る
			return exe + " -nostdin -v error"
								" -fflags nobuffer -flags low_delay"
								" -probesize 32 -analyzeduration 0"
								" -max_delay 0 -reorder_queue_size 0"
								" -f rtsp -allowed_media_types audio"
								" -protocol_whitelist tcp,tls -codec_whitelist aac,opus"
								" -rtsp_transport tcp -timeout " +
				   std::to_string(timeout_us) + " -i " + url +
				   " -map 0:a:0 -vn -f s16le -ar 48000 -ac 1 -flush_packets 1 -";
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

			// 継承させるのは標準出力・標準エラー出力のパイプだけ（-nostdin のため標準入力は渡さない）。
			// OBS 異常終了時も Job Object で終了させる。
			bool        in_job     = false;
			const bool  ok         = launch_child_process(ffmpeg_exe_path,
															  build_ffmpeg_cmdline(ffmpeg_exe_path, rtsp_url),
															  nullptr,
															  stdout_write,
															  stderr_write,
															  pi,
															  in_job);
			const DWORD launch_err = GetLastError();

			close_handle(stdout_write);
			close_handle(stderr_write);

			if (!ok) {
				char  msg[256] = {};
				DWORD err      = launch_err;
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
			if (!in_job) {
				blog(LOG_WARNING, "[obs-delay-stream] ffmpeg is not in the kill-on-close job; it may survive an OBS crash");
			}

			return true;
		}

		// 使うのは先頭行だけ。エラー行を出し続けるストリームでもメモリを使い切らないよう保持量を
		// 限る。FFmpeg が書き込みで止まらないよう、超過分も読み捨てる。
		constexpr size_t kMaxStderrBytes = 4096;
		// 1 回の呼び出しで読む量。出力が途切れなくても呼び出し元のタイムアウト・キャンセル確認へ戻る。
		constexpr size_t kMaxDrainBytesPerCall = 64 * 1024;

		void drain_text_pipe(HANDLE pipe, std::string &dst) {
			if (!pipe || pipe == INVALID_HANDLE_VALUE) return;
			DWORD  avail   = 0;
			size_t drained = 0;
			while (drained < kMaxDrainBytesPerCall &&
				   PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
				char  buf[512];
				DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buf)));
				DWORD read    = 0;
				if (!ReadFile(pipe, buf, to_read, &read, nullptr) || read == 0) break;
				drained += read;
				if (dst.size() < kMaxStderrBytes) dst.append(buf, std::min<size_t>(read, kMaxStderrBytes - dst.size()));
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
		out = get_default_ffmpeg_auto_path();
		return file_exists(out);
	}

	bool RtspE2eProber::is_legacy_auto_ffmpeg_path(const std::string &path) {
		const std::string legacy =
			get_default_ffmpeg_base_dir() + "\\" + kLegacyFfmpegExpandedDirName + "\\bin\\ffmpeg.exe";
		return _stricmp(expand_allowed_env_vars(path).c_str(), legacy.c_str()) == 0;
	}

	bool RtspE2eProber::ensure_auto_ffmpeg_path(std::string &out, std::string &err) {
		return ensure_auto_ffmpeg_binary(out, err);
	}

	bool RtspE2eProber::start(const std::string &rtsp_url, const std::string &ffmpeg_path_hint) {
		if (running_.load(std::memory_order_acquire)) return false;
		if (!is_allowed_rtsp_url(rtsp_url)) return false;

		cancel();
		{
			std::lock_guard<std::mutex> lk(mtx_);
			last_result_ = RtspE2eResult{};
		}
		{
			std::lock_guard<std::mutex> lk(t0_mtx_);
			t0_ = std::chrono::steady_clock::time_point{};
		}
		rtsp_url_         = normalize_rtsp_url_for_ffmpeg(rtsp_url);
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
			if (!ffmpeg_err.empty()) {
				// 保持量の上限で切れていれば、先頭行の URL も @ の手前で切れている可能性がある。
				const bool truncated = ffmpeg_stderr.size() >= kMaxStderrBytes;
				result.error_msg += "\nffmpeg: " + redact_url_userinfo(ffmpeg_err, truncated);
			}
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
			// 各セットで新しいプロセスを起動するため、自動配置版も起動直前に毎回検証する。
			if (i > 0 && !resolve_ffmpeg_path(ffmpeg_path_hint_, ffmpeg_exe_path, resolve_error)) {
				last_error = resolve_error.empty() ? "ffmpeg.exe が見つかりません。" : resolve_error;
				break;
			}
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
