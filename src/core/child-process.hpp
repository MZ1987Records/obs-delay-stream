#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace ods::core {

	/// 外部実行ファイル（cloudflared・FFmpeg）を子プロセスとして起動する。
	///
	/// - 継承させるハンドルは std_out・std_err（とあれば std_in）だけに限る。OBS が持つ
	///   ほかの継承可能なハンドルは渡さない
	/// - プロセス全体で 1 つの Job Object（KILL_ON_JOB_CLOSE）に登録する。OBS が異常終了しても
	///   Job のハンドルが閉じられて子プロセスは終了する
	/// - 一時停止状態で起動し、Job へ登録してから再開する。登録前に動き出さない
	///
	/// Job への登録に失敗した場合（OBS が入れ子を許さない Job の中で動いているなど）も起動は
	/// 続け、in_job を false にする。呼び出し側は終了の保証が無いことをログに残す。
	/// @param exe      実行ファイルの絶対パス
	/// @param cmdline  コマンドライン全体（引数は quote_windows_arg でクォート済み）
	/// @param std_in   子の標準入力。不要なら nullptr
	/// @param std_out  子の標準出力（継承可能なハンドル）
	/// @param std_err  子の標準エラー出力（継承可能なハンドル。std_out と同じでもよい）
	/// @param pi       起動したプロセス・スレッドのハンドル（呼び出し側が閉じる）
	/// @param in_job   Job Object へ登録できたか
	/// @return 失敗時は false。GetLastError() で理由を得られる
	bool launch_child_process(const std::string   &exe,
							  std::string          cmdline,
							  HANDLE               std_in,
							  HANDLE               std_out,
							  HANDLE               std_err,
							  PROCESS_INFORMATION &pi,
							  bool                &in_job);

} // namespace ods::core
