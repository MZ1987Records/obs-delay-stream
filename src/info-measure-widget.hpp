#pragma once
#ifndef OBS_INFO_MEASURE_WIDGET_HPP_
#define OBS_INFO_MEASURE_WIDGET_HPP_

#include <obs-module.h>

// 適用遅延テキスト + 計測ボタン + 結果テキストを1行に収めるウィジェット。
// 計測ボタンは binding_id を介して隠しボタンプロパティにバインドする。
obs_property_t* obs_properties_add_info_measure(
    obs_properties_t* props,
    const char* prop_name,
    const char* label,
    const char* info_text,
    const char* result_text,
    const char* button_label,
    obs_property_clicked_t clicked,
    void* clicked_priv,
    bool button_enabled,
    bool info_warn = false,
    int ch_index = -1);

void schedule_info_measure_inject(obs_source_t* source);

#endif // OBS_INFO_MEASURE_WIDGET_HPP_
