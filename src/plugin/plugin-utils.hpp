#pragma once

#include <cstddef>
#include <string>

namespace plugin_utils {

std::string trim_copy(std::string s);
std::string extract_host_from_url(const std::string &url);
std::string normalize_rtmp_url_candidate(const char *raw);
std::string join_rtmp_url_and_stream_key(const std::string &base_url,
										 const std::string &raw_key);
std::string get_local_ip();
void        copy_to_clipboard(const std::string &text);
std::string sanitize_stream_id(const char *raw);
std::string generate_stream_id(std::size_t len = 12);
std::string make_alpha_counter_label(int index);
int         clamp_sub_ch_count(int v);
int         normalize_opus_sample_rate(int v);
int         normalize_quantization_bits(int v);
int         normalize_pcm_downsample_ratio(int v);
int         normalize_playback_buffer_ms(int v);
std::string read_file_to_string(const char *path);
void        replace_all(std::string &s, const std::string &from, const std::string &to);

} // namespace plugin_utils
