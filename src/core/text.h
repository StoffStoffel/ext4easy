// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace ext4win {

// Plain UTF-8 <-> UTF-16 conversion (invalid input is replaced, not mapped).
std::wstring utf8ToWide(std::string_view s);
std::string wideToUtf8(std::wstring_view s);

// Human readable size, e.g. "14.9 GiB".
std::string formatSize(unsigned long long bytes);

}  // namespace ext4win
