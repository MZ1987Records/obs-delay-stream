// ID 生成と、外部実行ファイルの検証（SHA-256・承認値との照合・ZIP からの取り出し）の自己チェック。
// 失敗すると非 0 で終了する。
//   cmake --build build --config RelWithDebInfo --target plugin-utils-check

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core/approved-binaries.hpp"
#include "core/binary-verify.hpp"
#include "core/child-process.hpp"
#include "core/string-format.hpp"
#include "plugin/plugin-utils.hpp"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>

using namespace ods::core;
using ods::plugin::generate_stream_id;

namespace {

	int failures = 0;

	void check(bool ok, const char *what) {
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++failures;
		}
	}

	std::string read_all(const std::filesystem::path &path) {
		std::ifstream ifs(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
	}

	// テスト用の ZIP を System32 の tar.exe で作る（-a で拡張子から ZIP 形式を選ぶ）。
	bool make_zip(const std::string &zip_path, const std::string &base_dir, const std::string &entry) {
		char sys_dir[MAX_PATH] = {};
		if (GetSystemDirectoryA(sys_dir, MAX_PATH) == 0) return false;
		const std::string tar = std::string(sys_dir) + "\\tar.exe";
		std::string cmd = quote_windows_arg(tar) + " -a -cf " + quote_windows_arg(zip_path) + " -C " +
						  quote_windows_arg(base_dir) + " " + quote_windows_arg(entry);
		STARTUPINFOA        si{};
		PROCESS_INFORMATION pi{};
		si.cb = sizeof(si);
		if (!CreateProcessA(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
							&si, &pi)) {
			return false;
		}
		WaitForSingleObject(pi.hProcess, 60000);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return code == 0;
	}

	std::string system_exe(const char *name) {
		char sys_dir[MAX_PATH] = {};
		GetSystemDirectoryA(sys_dir, MAX_PATH);
		return std::string(sys_dir) + "\\" + name;
	}

	// 子プロセスを起動し、標準出力をすべて読んで終了を待つ。
	bool run_and_capture(const std::string &exe, const std::string &cmdline, std::string &out, bool &in_job) {
		SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
		HANDLE              rd = nullptr;
		HANDLE              wr = nullptr;
		if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
		SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
		PROCESS_INFORMATION pi{};
		const bool          ok = launch_child_process(exe, cmdline, nullptr, wr, wr, pi, in_job);
		CloseHandle(wr);
		if (!ok) {
			CloseHandle(rd);
			return false;
		}
		char  buf[256];
		DWORD n = 0;
		while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
			out.append(buf, n);
		WaitForSingleObject(pi.hProcess, 30000);
		CloseHandle(rd);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return true;
	}

	// 中継役: 長く動く子（ping）を Job に入れて起動し、その PID を出力してすぐ終了する。
	// 中継役のプロセスが終わると Job のハンドルが閉じられ、子も終了するはず。
	int spawn_long_child_and_exit() {
		const std::string   ping = system_exe("PING.EXE");
		PROCESS_INFORMATION pi{};
		bool                in_job = false;
		HANDLE              out    = GetStdHandle(STD_OUTPUT_HANDLE);
		if (!launch_child_process(ping, quote_windows_arg(ping) + " -n 60 127.0.0.1", nullptr, nullptr, nullptr, pi,
								  in_job)) {
			return 2;
		}
		const std::string line = std::to_string(pi.dwProcessId) + (in_job ? " job\n" : " nojob\n");
		DWORD             n    = 0;
		WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return 0;
	}

	// 子として起動され、標準入力が有効なハンドル（NUL などのデバイス・ファイル）かを報告する。
	int report_stdin() {
		const HANDLE      in   = GetStdHandle(STD_INPUT_HANDLE);
		const bool        ok   = in && in != INVALID_HANDLE_VALUE && GetFileType(in) != FILE_TYPE_UNKNOWN;
		const std::string line = ok ? "stdin-ok\n" : "stdin-invalid\n";
		DWORD             n    = 0;
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
		return 0;
	}

} // namespace

int main(int argc, char **argv) {
	if (argc > 1 && std::strcmp(argv[1], "--spawn-long-child") == 0) return spawn_long_child_and_exit();
	if (argc > 1 && std::strcmp(argv[1], "--report-stdin") == 0) return report_stdin();

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

	// SHA-256 と承認値との照合
	{
		namespace fs   = std::filesystem;
		const auto dir = fs::temp_directory_path() / ("ods-plugin-utils-check-" + generate_stream_id(12));
		std::error_code ec;
		const bool      created = fs::create_directory(dir, ec);
		check(created, "create unique temp directory");
		if (created) {
			const std::string small = (dir / "abc.bin").string();
			const std::string large = (dir / "large.bin").string();
			std::ofstream(small, std::ios::binary) << "abc";
			std::ofstream(large, std::ios::binary) << std::string(2500000, 'a'); // 1 MiB の読込単位をまたぐ

			const char *abc_sha   = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
			const char *large_sha = "38a637965059125eeb67f54c30e7f48a61859a467a800ba09740ba48a924f2b9";
			check(sha256_file_hex(small) == abc_sha, "sha256 of abc");
			check(sha256_file_hex(large) == large_sha, "sha256 across read buffers");
			check(sha256_file_hex((dir / "missing.bin").string()).empty(), "sha256 of missing file");

			check(verify_file(small, 3, abc_sha) == VerifyResult::Ok, "verify ok");
			check(verify_file(small, 3, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") ==
					  VerifyResult::Ok,
				  "verify accepts uppercase hex");
			check(verify_file(small, 4, abc_sha) == VerifyResult::Mismatch, "verify size mismatch");
			check(verify_file(small, 3, large_sha) == VerifyResult::Mismatch, "verify hash mismatch");
			check(verify_file((dir / "missing.bin").string(), 3, abc_sha) == VerifyResult::Missing, "verify missing");
			check(verify_file(dir.string(), 0, abc_sha) == VerifyResult::Missing, "directory is not a file");

			ec.clear();
			fs::remove_all(dir, ec);
			check(!ec, "remove unique temp directory");
		}
	}

	// ZIP から 1 エントリだけを取り出す（FFmpeg の承認版 ZIP から ffmpeg.exe だけを取り出す処理）
	{
		namespace fs   = std::filesystem;
		const auto dir = fs::temp_directory_path() / ("ods-plugin-utils-check-zip-" + generate_stream_id(12));
		fs::create_directories(dir / "src" / "pkg" / "bin");
		std::ofstream(dir / "src" / "pkg" / "bin" / "tool.exe", std::ios::binary) << "tool-content";
		std::ofstream(dir / "src" / "pkg" / "readme.txt", std::ios::binary) << "readme";
		const std::string zip = (dir / "test.zip").string();
		check(make_zip(zip, (dir / "src").string(), "pkg"), "create test zip with tar.exe");

		std::string err;
		const auto  out = dir / "out";
		check(extract_zip_entry(zip, out.string(), "pkg\\bin\\tool.exe", err), "extract entry");
		check(read_all(out / "pkg" / "bin" / "tool.exe") == "tool-content", "extracted content");
		check(!fs::exists(out / "pkg" / "readme.txt"), "other entries are not extracted");

		const auto out2 = dir / "out2";
		check(extract_zip_entry(zip, out2.string(), "pkg/bin/tool.exe", err), "entry with forward slashes");
		check(read_all(out2 / "pkg" / "bin" / "tool.exe") == "tool-content", "extracted content (forward slashes)");

		err.clear();
		check(!extract_zip_entry(zip, (dir / "out3").string(), "pkg\\bin\\missing.exe", err) && !err.empty(),
			  "missing entry fails");
		check(!fs::exists(dir / "out3" / "pkg" / "bin" / "missing.exe"), "missing entry leaves no file");
		err.clear();
		check(!extract_zip_entry((dir / "missing.zip").string(), (dir / "out4").string(), "pkg\\bin\\tool.exe", err) &&
				  !err.empty(),
			  "missing zip fails");
		fs::remove_all(dir);
	}

	// 子プロセスの起動: 指定したパイプだけを継承して出力を受け取れ、Job に登録される
	{
		const std::string cmd = system_exe("cmd.exe");
		std::string       out;
		bool              in_job = false;
		check(run_and_capture(cmd, quote_windows_arg(cmd) + " /c echo child-ok", out, in_job), "launch child");
		check(out.find("child-ok") != std::string::npos, "child output through inherited pipe");
		check(in_job, "child is in the kill-on-close job");
	}

	// 渡していない継承可能なハンドルは子に引き継がれない（OBS のファイル・ソケットを漏らさない）。
	// 書き込み端を継承可能にしたパイプを用意し、子には渡さずに起動する。親が自分の書き込み端を
	// 閉じたとき、子が複製を持っていなければ読み取り端は ERROR_BROKEN_PIPE になる。
	{
		SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
		HANDLE              out_rd = nullptr, out_wr = nullptr; // 子の標準出力（継承を有効にするため渡す）
		HANDLE              leak_rd = nullptr, leak_wr = nullptr; // 子に渡さない継承可能なハンドル
		CreatePipe(&out_rd, &out_wr, &sa, 0);
		CreatePipe(&leak_rd, &leak_wr, &sa, 0);
		SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(leak_rd, HANDLE_FLAG_INHERIT, 0);

		const std::string   ping = system_exe("PING.EXE");
		PROCESS_INFORMATION pi{};
		bool                in_job = false;
		const bool          ok     = launch_child_process(
			ping, quote_windows_arg(ping) + " -n 30 127.0.0.1", nullptr, out_wr, out_wr, pi, in_job);
		check(ok, "launch child with an unrelated inheritable handle open");
		CloseHandle(leak_wr);
		DWORD      avail = 0;
		const BOOL peek  = PeekNamedPipe(leak_rd, nullptr, 0, nullptr, &avail, nullptr);
		check(ok && !peek && GetLastError() == ERROR_BROKEN_PIPE, "unrelated inheritable handle is not inherited");
		if (ok) {
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		CloseHandle(out_wr);
		CloseHandle(out_rd);
		CloseHandle(leak_rd);
	}

	// 渡さなかった標準入力は NUL で補われ、子から見て有効なハンドルになる
	{
		char self[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, self, MAX_PATH);
		std::string out;
		bool        in_job = false;
		check(run_and_capture(self, quote_windows_arg(self) + " --report-stdin", out, in_job), "launch stdin reporter");
		check(out.find("stdin-ok") != std::string::npos, "missing stdin is replaced with NUL");
	}

	// 継承できないハンドルを渡すと、黙って除外せずに失敗する
	{
		HANDLE rd = nullptr, wr = nullptr;
		CreatePipe(&rd, &wr, nullptr, 0); // 継承不可
		const std::string   cmd = system_exe("cmd.exe");
		PROCESS_INFORMATION pi{};
		bool                in_job = false;
		const bool          ok = launch_child_process(cmd, quote_windows_arg(cmd) + " /c echo x", nullptr, wr, wr, pi, in_job);
		check(!ok && GetLastError() == ERROR_INVALID_HANDLE, "non-inheritable handle is rejected");
		if (ok) {
			WaitForSingleObject(pi.hProcess, 5000);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		CloseHandle(wr);
		CloseHandle(rd);
	}

	// 親（中継役）が終了すると、Job に入れた子も終了する（OBS の異常終了で子が残らないこと）
	{
		char self[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, self, MAX_PATH);
		std::string out;
		bool        in_job = false;
		check(run_and_capture(self, quote_windows_arg(self) + " --spawn-long-child", out, in_job), "launch relay");
		const DWORD pid = static_cast<DWORD>(std::strtoul(out.c_str(), nullptr, 10));
		check(pid != 0 && out.find(" job") != std::string::npos, "relay put its child in a job");
		HANDLE child = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
		// 既に終了して開けない場合も、子は残っていない
		const bool gone = !child || WaitForSingleObject(child, 5000) == WAIT_OBJECT_0;
		check(gone, "child is terminated when its parent exits");
		if (child) {
			if (!gone) TerminateProcess(child, 1);
			CloseHandle(child);
		}
	}

	// 承認一覧の値の形（版を固定した URL・64 桁の小文字 16 進）
	for (const ApprovedBinary *bin : {&kApprovedCloudflared, &kApprovedFfmpeg}) {
		check(std::strstr(bin->url, "/latest/") == nullptr, "approved url is pinned");
		for (const char *h : {bin->download_sha256, bin->exe_sha256}) {
			bool lower_hex = std::strlen(h) == 64;
			for (const char *c = h; *c; ++c)
				lower_hex = lower_hex && (std::isdigit(static_cast<unsigned char>(*c)) || (*c >= 'a' && *c <= 'f'));
			check(lower_hex, "approved sha256 is 64 lowercase hex digits");
		}
		check(bin->download_size > 0 && bin->exe_size > 0, "approved sizes are set");
	}

	if (failures == 0) std::printf("plugin-utils-check: OK\n");
	return failures == 0 ? 0 : 1;
}
