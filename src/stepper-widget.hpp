#pragma once
#ifndef OBS_STEPPER_WIDGET_HPP_
#define OBS_STEPPER_WIDGET_HPP_

#include <obs-module.h>

obs_property_t* obs_properties_add_stepper(
    obs_properties_t* props,
    const char* prop_name,
    const char* label,
    const char* setting_key,
    double min_val,
    double max_val,
    double def_val,
    int decimals,
    const char* suffix,
    bool store_as_int = false);

void schedule_stepper_inject(obs_source_t* source);

#endif // OBS_STEPPER_WIDGET_HPP_
