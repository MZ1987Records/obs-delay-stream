#pragma once

#include <obs-module.h>
#include <string>

namespace ods::plugin {

	/// サブチャンネルのデフォルトメモ文字列を生成する
	std::string make_default_sub_memo(int counter);

	/// コーデック選択に応じてオプション項目の表示を切り替える
	void apply_codec_option_visibility(obs_properties_t *props, obs_data_t *settings);

	/// フィルタ設定のデフォルト値を書き込む
	void set_delay_stream_defaults(obs_data_t *settings);

} // namespace ods::plugin
