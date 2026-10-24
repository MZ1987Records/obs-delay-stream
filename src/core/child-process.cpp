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

		// 継承させるハンドルの一覧。重複や継承不可のハンドルを含めると CreateProcess が失敗する。
		std::vector<HANDLE> inherit;
		for (HANDLE h : {std_in, std_out, std_err}) {
			if (!is_inheritable(h)) continue;
			bool dup = false;
			for (HANDLE x : inherit)
				dup = dup || (x == h);
			if (!dup) inherit.push_back(h);
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
		si.StartupInfo.cb         = sizeof(si);
		si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput  = std_in;
		si.StartupInfo.hStdOutput = std_out;
		si.StartupInfo.hStdError  = std_err;
		si.lpAttributeList        = attrs;

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
		ResumeThread(pi.hThread);
		return true;
	}

} // namespace ods::core
