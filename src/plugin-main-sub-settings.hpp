#pragma once

#include <array>

namespace plugin_main_sub_settings {

using SubSettingKey = std::array<char, 32>;

SubSettingKey make_sub_key(const char* suffix, int ch);
SubSettingKey make_sub_delay_key(int ch);
SubSettingKey make_sub_adjust_key(int ch);
SubSettingKey make_sub_memo_key(int ch);
SubSettingKey make_sub_code_key(int ch);
SubSettingKey make_sub_remove_row_key(int ch);

float calc_sub_delay_raw_value_ms(float base_delay_ms,
                                  float adjust_ms,
                                  float global_offset_ms);
float calc_effective_sub_delay_value_ms(float base_delay_ms,
                                        float adjust_ms,
                                        float global_offset_ms);

} // namespace plugin_main_sub_settings
