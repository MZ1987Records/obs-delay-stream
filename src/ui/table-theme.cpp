#include "ui/table-theme.hpp"

#include <cmath>

namespace ods::ui {

	QColor pick_table_alt_row_color(const QColor &base, const QColor &alt_candidate) {
		const double diff = std::abs(base.lightnessF() - alt_candidate.lightnessF());
		if (diff > 0.18) return base;
		return alt_candidate;
	}

	TableThemeColors make_table_theme_colors(const QPalette &palette) {
		const QColor     base_color = palette.color(QPalette::Base);
		TableThemeColors out;
		out.table_bg    = palette.color(QPalette::Window);
		out.header_bg   = palette.color(QPalette::Button);
		out.header_text = palette.color(QPalette::ButtonText);
		out.row_bg      = base_color;
		out.alt_row_bg  = pick_table_alt_row_color(
			base_color,
			palette.color(QPalette::AlternateBase));
		out.text   = palette.color(QPalette::Text);
		out.border = palette.color(QPalette::Mid);
		out.link   = palette.color(QPalette::Link);
		return out;
	}

} // namespace ods::ui
