// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace ext4win {

int runServiceDispatcher();  // started by the Service Control Manager
int runServiceConsole();     // same logic in a console window (Ctrl+C stops)
int installService();        // register + start service, tray autostart (admin)
int uninstallService();      // stop + remove (admin)
int runCtl(const std::vector<std::wstring>& args);  // send a raw request to the service

}  // namespace ext4win
