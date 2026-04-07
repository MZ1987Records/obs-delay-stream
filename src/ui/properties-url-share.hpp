#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}

namespace ods::ui::url_share {

	/// URL 共有 UI グループを追加する
	void add_url_share_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

} // namespace ods::ui::url_share
