#include "core/child-process.hpp"

#include <mutex>
#include <vector>

namespace ods::core {

	namespace {

		// プロセスの終了まで閉じない。OBS が終了・異常終了するとカーネルが閉じ、登録した
		// 子プロセスはすべて終了する。
		HANDLE get_kill_on_close_job() {
			static std::once_flag once;
			static HANDLE         job = nullptr;
			std::call_once(once, []() {
				HANDLE h = CreateJobObjectA(nullptr, nullptr);
				if (!h) return;
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
				info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
					CloseHandle(h);
					return;
				}
				job = h;
			});
			return job;
		}

		bool is_inheritable(HANDLE h) {
			if (!h || h == INVALID_HANDLE_VALUE) return false;
			DWORD flags = 0;
			return GetHandleInformation(h, &flags) && (flags & HANDLE_FLAG_INHERIT);
		}

	} // namespace

	bool launch_child_process(const std::string   &exe,
							  std::string          cmdline,
							  HANDLE               std_in,
							  HANDLE               std_out,
							  HANDLE               std_err,
							  PROCESS_INFORMATION &pi,
							  bool                &in_job) {
		pi     = PROCESS_INFORMATION{};
		in_job = false;

		// 渡されたハンドルは継承可能でなければならない。黙って除外すると、子の標準出力などが
		// 無効なまま起動してしまう。
		for (HANDLE h : {std_in, std_out, std_err}) {
			if (h && !is_inheritable(h)) {
				SetLastError(ERROR_INVALID_HANDLE);
				return false;
			}
		}

		// STARTF_USESTDHANDLES では 3 つとも有効なハンドルが要るため、渡されなかったものは
		// 継承可能な NUL デバイスで補う。3 つとも不要なら標準ハンドルを指定しない。
		const bool use_std_handles = std_in || std_out || std_err;
		HANDLE     nul             = nullptr;
		if (use_std_handles && (!std_in || !std_out || !std_err)) {
			SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
			nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
							  OPEN_EXISTING, 0, nullptr);
			if (nul == INVALID_HANDLE_VALUE) return false;
			if (!std_in) std_in = nul;
			if (!std_out) std_out = nul;
			if (!std_err) std_err = nul;
		}
		struct NulCloser {
			HANDLE h;
			~NulCloser() {
				if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
			}
		} nul_closer{nul};

		// 継承させるハンドルの一覧。重複を含めると CreateProcess が失敗する。
		std::vector<HANDLE> inherit;
		if (use_std_handles) {
			for (HANDLE h : {std_in, std_out, std_err}) {
				bool dup = false;
				for (HANDLE x : inherit)
					dup = dup || (x == h);
				if (!dup) inherit.push_back(h);
			}
		}

		SIZE_T attr_size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
		std::vector<unsigned char> attr_buf(attr_size);
		auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
		if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) return false;
		if (!inherit.empty() &&
			!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
									   inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
			const DWORD e = GetLastError();
			DeleteProcThreadAttributeList(attrs);
			SetLastError(e);
			return false;
		}

		STARTUPINFOEXA si{};
		si.StartupInfo.cb = sizeof(si);
		if (use_std_handles) {
			si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
			si.StartupInfo.hStdInput  = std_in;
			si.StartupInfo.hStdOutput = std_out;
			si.StartupInfo.hStdError  = std_err;
		}
		si.lpAttributeList = attrs;

		cmdline.push_back('\0');
		const BOOL ok = CreateProcessA(exe.c_str(),
									   cmdline.data(),
									   nullptr,
									   nullptr,
									   inherit.empty() ? FALSE : TRUE,
									   CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
									   nullptr,
									   nullptr,
									   &si.StartupInfo,
									   &pi);
		const DWORD create_err = GetLastError();
		DeleteProcThreadAttributeList(attrs);
		if (!ok) {
			SetLastError(create_err);
			return false;
		}

		HANDLE job = get_kill_on_close_job();
		in_job     = job && AssignProcessToJobObject(job, pi.hProcess);

		// 戻り値は再開前の停止回数。1 以外ならスレッドはまだ止まっている（-1 は失敗）。
		// 起動済みと扱うと呼び出し側はタイムアウトまで待つため、子を終了して失敗を返す。
		const DWORD prev_suspend = ResumeThread(pi.hThread);
		if (prev_suspend != 1) {
			const DWORD e = (prev_suspend == static_cast<DWORD>(-1)) ? GetLastError() : ERROR_INVALID_STATE;
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			pi     = PROCESS_INFORMATION{};
			in_job = false;
			SetLastError(e);
			return false;
		}
		return true;
	}

} // namespace ods::core
