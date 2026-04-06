#pragma once

#include <string>

namespace ods::plugin {

struct LatestReleaseInfo {
	std::string latest_version;
	std::string release_url;
	std::string error;
};

bool fetch_latest_release_info(LatestReleaseInfo &out);
bool is_newer_version(const std::string &latest, const std::string &current);

} // namespace ods::plugin
