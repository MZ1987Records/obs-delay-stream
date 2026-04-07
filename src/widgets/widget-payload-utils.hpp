#pragma once

#include <QString>
#include <QStringList>
#include <string>

namespace ods::widgets {

	std::string escape_field(const char *src);

	bool split_escaped_pipe_fields(const QString &text, QStringList &fields);

} // namespace ods::widgets
