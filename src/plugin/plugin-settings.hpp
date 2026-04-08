#pragma once

#include <array>
#include <obs-module.h>

namespace ods::plugin {

	struct DelayStreamData;

	inline constexpr char kMasterBaseDelayKey[] = "master_base_delay_ms";
	inline constexpr char kMasterOffsetKey[]    = "master_offset_ms";

	using SubSettingKey = std::array<char, 32>;

	SubSettingKey make_sub_key(const char *suffix, int ch); ///< `sub{ch}_{suffix}` 形式の設定キーを生成する
	SubSettingKey make_sub_base_delay_key(int ch);          ///< `sub{ch}_delay_ms` キーを生成する
	SubSettingKey make_sub_offset_key(int ch);              ///< `sub{ch}_adjust_ms` キーを生成する
	SubSettingKey make_sub_memo_key(int ch);                ///< `sub{ch}_memo` キーを生成する
	SubSettingKey make_sub_code_key(int ch);                ///< `sub{ch}_code` キーを生成する
	SubSettingKey make_sub_remove_row_key(int ch);          ///< `sub{ch}_remove_row` キーを生成する

	/// 補正前のサブ遅延値（base + offset + master_offset + master_base、0 未満許容）を計算する
	int calc_sub_delay_raw_value_ms(int base_delay_ms,
									int offset_ms,
									int master_offset_ms,
									int master_base_delay_ms);

	/// 実適用するサブ遅延値（下限 0 を適用）を計算する
	int calc_effective_sub_delay_value_ms(int base_delay_ms,
										  int offset_ms,
										  int master_offset_ms,
										  int master_base_delay_ms);

	/// OBS 設定値を `DelayStreamData` に反映する
	void apply_settings(DelayStreamData *d, obs_data_t *settings);

} // namespace ods::plugin
