#pragma once

#include <string>

namespace ods::plugin {

	/// 受信側 UI の index.html を文字列で返す
	std::string load_receiver_index_html();

	/// 受信側 UI のルートディレクトリパスを返す
	std::string get_receiver_root_dir();

	/// 受信側 UI のビルドタイムスタンプを返す
	const char *get_receiver_build_timestamp();

} // namespace ods::plugin
