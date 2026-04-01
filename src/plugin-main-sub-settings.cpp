#include "plugin-main-sub-settings.hpp"

#include <cstdio>

namespace plugin_main_sub_settings {

SubSettingKey make_sub_key(const char* suffix, int ch) {
    SubSettingKey key{};
    snprintf(key.data(), key.size(), "sub%d_%s", ch, suffix);
    return key;
}

SubSettingKey make_sub_delay_key(int ch) { return make_sub_key("delay_ms", ch); }
SubSettingKey make_sub_adjust_key(int ch) { return make_sub_key("adjust_ms", ch); }
SubSettingKey make_sub_memo_key(int ch) { return make_sub_key("memo", ch); }
SubSettingKey make_sub_code_key(int ch) { return make_sub_key("code", ch); }
SubSettingKey make_sub_remove_row_key(int ch) { return make_sub_key("memo_remove_row", ch); }

float calc_sub_delay_raw_value_ms(float base_delay_ms,
                                  float adjust_ms,
                                  float global_offset_ms) {
    return base_delay_ms + adjust_ms + global_offset_ms;
}

float calc_effective_sub_delay_value_ms(float base_delay_ms,
                                        float adjust_ms,
                                        float global_offset_ms) {
    float effective = calc_sub_delay_raw_value_ms(base_delay_ms, adjust_ms, global_offset_ms);
    if (effective < 0.0f) effective = 0.0f;
    return effective;
}

} // namespace plugin_main_sub_settings
