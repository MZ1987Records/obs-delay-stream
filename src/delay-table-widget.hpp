#pragma once

#include <obs-module.h>

// 遅延テーブルウィジェットに渡す1チャンネル分の情報。
struct DelayTableChannelInfo {
    const char* name;         // メモ名 (空文字列可)
    float measured_ms;        // 計測値・片道遅延。未計測は -1.0f
    float base_ms;            // 基準遅延 (proposed_delay)
    float adjust_ms;          // 追加遅延 (adjust_ms)
    float global_ms;          // 全体オフセット (sub_offset_ms)
    float total_ms;           // 合計 = max(0, base + adjust + global)
    bool  warn;               // true if raw total < 0
};

// テーブルの列ヘッダーとエディタラベルの文字列。
// plugin-main.cpp 側で obs_module_text() (T_()) を使って渡す。
struct DelayTableLabels {
    const char* hdr_ch       = nullptr;  // "Ch" 列ヘッダー
    const char* hdr_name     = nullptr;  // "名前" 列ヘッダー
    const char* hdr_measured = nullptr;  // "計測" 列ヘッダー
    const char* hdr_base     = nullptr;  // "基準" 列ヘッダー
    const char* hdr_adjust   = nullptr;  // "追加" 列ヘッダー
    const char* hdr_global   = nullptr;  // "全体" 列ヘッダー
    const char* hdr_total    = nullptr;  // "合計 ms" 列ヘッダー
    const char* lbl_editor   = nullptr;  // エディタ行ラベル ("追加遅延")
};

// QFormLayout のプレースホルダーとして OBS_TEXT_INFO プロパティを追加する。
// schedule_delay_table_inject() を呼ぶとプレースホルダーが実ウィジェットに差し替わる。
obs_property_t* obs_properties_add_delay_table(
    obs_properties_t*            props,
    const char*                  prop_name,
    int                          selected_ch,
    int                          ch_count,
    const DelayTableChannelInfo* channels,
    const DelayTableLabels&      labels);

void schedule_delay_table_inject(obs_source_t* source);
