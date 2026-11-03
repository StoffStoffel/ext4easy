// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace ext4easy {

// Service log: %ProgramData%\ext4easy\service.log, plus stderr in console mode.
void logInit(bool console);
void logLine(const std::string& text);
void logLine(const std::wstring& text);

// %ProgramData%\ext4easy (created if missing).
std::wstring dataDirectory();

}  // namespace ext4easy
