#pragma once

#include <string>

namespace plugin_main_release_check {

struct LatestReleaseInfo {
    std::string latest_version;
    std::string release_url;
    std::string error;
};

bool fetch_latest_release_info(LatestReleaseInfo& out);
bool is_newer_version(const std::string& latest, const std::string& current);

} // namespace plugin_main_release_check

