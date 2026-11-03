// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace ext4win {

// Service log: %ProgramData%\ext4win\service.log, plus stderr in console mode.
void logInit(bool console);
void logLine(const std::string& text);
void logLine(const std::wstring& text);

// %ProgramData%\ext4win (created if missing).
std::wstring dataDirectory();

}  // namespace ext4win
