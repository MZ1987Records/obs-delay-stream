#include "widgets/bulk-add-dialog.hpp"

#include "core/constants.hpp"
#include "core/string-format.hpp"

#include <obs-module.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>

#define T_(s) obs_module_text(s)

namespace ods::widgets {

	namespace {

		// テキストを行ごとに分割し、trim・空行除去・SUB_MEMO_MAX_CHARS 以内に切り詰めて返す。
		std::vector<std::string> parse_lines(const QString &text) {
			std::vector<std::string> out;
			const QStringList        lines = text.split(QChar('\n'), Qt::KeepEmptyParts);
			for (const QString &raw : lines) {
				const QString trimmed = raw.trimmed();
				if (trimmed.isEmpty()) continue;
				std::string s = trimmed.toStdString();
				if (s.size() > static_cast<std::size_t>(ods::core::SUB_MEMO_MAX_CHARS)) {
					// UTF-8 マルチバイト境界で切り詰める（後続バイト 10xxxxxx を遡る）
					std::size_t cut = static_cast<std::size_t>(ods::core::SUB_MEMO_MAX_CHARS);
					while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
						--cut;
					}
					s.resize(cut);
				}
				if (!s.empty()) out.push_back(std::move(s));
			}
			return out;
		}

	} // namespace

	std::optional<BulkAddResult> bulk_add_dialog_exec(QWidget *parent, int existing_count, int max_count) {
		QDialog dlg(parent);
		dlg.setWindowTitle(T_("SubBulkAddTitle"));
		dlg.setModal(true);
		dlg.resize(440, 360);

		auto *root = new QVBoxLayout(&dlg);

		auto *editor = new QPlainTextEdit(&dlg);
		editor->setPlaceholderText(T_("SubBulkAddPlaceholder"));
		editor->setTabChangesFocus(true);
		root->addWidget(editor, 1);

		auto *radio_append  = new QRadioButton(T_("SubBulkAddModeAppend"), &dlg);
		auto *radio_replace = new QRadioButton(T_("SubBulkAddModeReplace"), &dlg);
		radio_append->setChecked(true);
		root->addWidget(radio_append);
		root->addWidget(radio_replace);

		auto *counter = new QLabel(&dlg);
		root->addWidget(counter);

		auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		root->addWidget(btns);
		QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

		QPushButton *ok_btn = btns->button(QDialogButtonBox::Ok);

		auto update_status = [&]() {
			const auto lines    = parse_lines(editor->toPlainText());
			const int  n_lines  = static_cast<int>(lines.size());
			const bool replace  = radio_replace->isChecked();
			const int  vacant   = std::max(0, max_count - existing_count);
			const int  will_add = replace ? std::min(n_lines, max_count)
										  : std::min(n_lines, vacant);

			counter->setText(QString::fromUtf8(
				ods::core::string_printf(T_("SubBulkAddCounterFmt"),
										 n_lines,
										 will_add,
										 max_count)
					.c_str()));
			ok_btn->setEnabled(will_add > 0);
		};

		QObject::connect(editor, &QPlainTextEdit::textChanged, &dlg, update_status);
		QObject::connect(radio_append, &QRadioButton::toggled, &dlg, update_status);
		QObject::connect(radio_replace, &QRadioButton::toggled, &dlg, update_status);
		update_status();

		if (dlg.exec() != QDialog::Accepted) {
			return std::nullopt;
		}

		BulkAddResult result;
		result.names       = parse_lines(editor->toPlainText());
		result.replace_all = radio_replace->isChecked();
		return result;
	}

} // namespace ods::widgets
