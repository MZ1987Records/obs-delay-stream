#pragma once
#ifndef OBS_TEXT_BUTTON_WIDGET_HPP_
#define OBS_TEXT_BUTTON_WIDGET_HPP_

#include <obs-module.h>

obs_property_t* obs_properties_add_text_button(
    obs_properties_t* props,
    const char* prop_name,
    const char* label,
    const char* setting_key,
    const char* button_label,
    obs_property_clicked_t clicked,
    void* clicked_priv = nullptr,
    bool input_enabled = true,
    bool button_enabled = true);

void schedule_text_button_inject(obs_source_t* source);

#endif // OBS_TEXT_BUTTON_WIDGET_HPP_
