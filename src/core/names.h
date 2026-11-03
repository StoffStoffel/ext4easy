// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace ext4win {

// Linux file names are arbitrary bytes (usually UTF-8); Windows names are
// UTF-16 and forbid some characters. The mapping is reversible:
//  - valid UTF-8 is converted normally
//  - characters forbidden on Windows (control chars, " * : < > ? \ |) and a
//    trailing dot or space become U+F000 + char (same scheme as Cygwin/WSL)
//  - bytes that are not valid UTF-8 become U+F100 + byte
std::wstring linuxNameToWindows(std::string_view name);
std::string windowsNameToLinux(std::wstring_view name);

// "\dir\file" (WinFsp style) -> "/dir/file" with each component mapped.
std::string windowsPathToLinux(std::wstring_view path);

}  // namespace ext4win
