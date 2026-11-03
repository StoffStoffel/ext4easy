// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/service_main.h"

#include "core/disk_source.h"
#include "core/text.h"
#include "ipc/pipe.h"
#include "service/device_watcher.h"
#include "service/log.h"
#include "service/pipe_server.h"
#include "service/volume_manager.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <memory>

namespace ext4win {

namespace {

const wchar_t* kServiceName = L"ext4win";
const wchar_t* kServiceDisplay = L"ext4win - Linux drives";
const wchar_t* kServiceDescription =
    L"Mounts Linux ext2/ext3/ext4 drives read-only as drive letters.";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kTrayExe = L"ext4win-tray.exe";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;

// The running components. Started and stopped in this order.
struct Components {
    VolumeManager manager;
    PipeServer server{manager};
    DeviceWatcher watcher;

    bool start() {
        std::string error;
        if (!server.start(error)) {
            logLine(error);
            return false;
        }
        watcher.start([this](const std::wstring& path, DeviceWatcher::Event ev) {
            switch (ev) {
                case DeviceWatcher::Event::DiskArrived: manager.diskArrived(path); break;
                case DeviceWatcher::Event::DiskRemoved: manager.diskRemoved(path); break;
                case DeviceWatcher::Event::VolumeArrived: manager.volumeArrived(path); break;
                case DeviceWatcher::Event::VolumeRemoved: manager.volumeRemoved(); break;
            }
        });
        return true;
    }
    void stop() {
        watcher.stop();
        server.stop();
        manager.unmountAll();
    }
};

// Restart automatically if the service crashes (5 s delay, twice per day).
void setFailureActions(SC_HANDLE svc) {
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_NONE, 0}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 24 * 3600;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
}

// Called at service start, so installers do not need to set it up.
void configureOwnFailureActions() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_CHANGE_CONFIG | SERVICE_START);
    if (svc) {
        setFailureActions(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

void setStatus(DWORD state, DWORD exitCode = NO_ERROR) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 10000;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI controlHandler(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        setStatus(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, controlHandler, nullptr);
    if (!g_statusHandle) return;
    setStatus(SERVICE_START_PENDING);
    logInit(false);
    logLine("service starting");
    configureOwnFailureActions();

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto c = std::make_unique<Components>();
    if (!c->start()) {
        setStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }
    setStatus(SERVICE_RUNNING);
    WaitForSingleObject(g_stopEvent, INFINITE);
    logLine("service stopping");
    c->stop();
    c.reset();
    setStatus(SERVICE_STOPPED);
}

BOOL WINAPI consoleCtrl(DWORD) {
    if (g_stopEvent) SetEvent(g_stopEvent);
    return TRUE;
}

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    return p.substr(0, p.find_last_of(L'\\'));
}

std::wstring exePath() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, n);
}

void printError(const std::wstring& what) {
    DWORD e = GetLastError();
    std::fprintf(stderr, "error: %s: %s\n", wideToUtf8(what).c_str(),
                 wideToUtf8(win32ErrorText(e)).c_str());
    if (e == ERROR_ACCESS_DENIED) std::fprintf(stderr, "Run this command as administrator.\n");
}

bool stopServiceAndWait(SC_HANDLE svc) {
    SERVICE_STATUS st{};
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &st) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
        return false;
    for (int i = 0; i < 100; ++i) {
        if (!QueryServiceStatus(svc, &st) || st.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(100);
    }
    return false;
}

}  // namespace

int runServiceDispatcher() {
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), serviceMain},
                                    {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            std::fprintf(stderr,
                         "This command is started by Windows. Use 'ext4win install', or\n"
                         "'ext4win service --console' to run it in this window.\n");
        return 1;
    }
    return 0;
}

int runServiceConsole() {
    logInit(true);
    logLine("running in console mode - press Ctrl+C to stop");
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(consoleCtrl, TRUE);
    auto c = std::make_unique<Components>();
    if (!c->start()) return 1;
    WaitForSingleObject(g_stopEvent, INFINITE);
    logLine("stopping");
    c->stop();
    return 0;
}

int installService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        printError(L"cannot open service manager");
        return 1;
    }
    std::wstring cmd = L"\"" + exePath() + L"\" service";
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                   SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr,
                                   nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (svc) {
            stopServiceAndWait(svc);
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                                 cmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr,
                                 kServiceDisplay);
        }
    }
    if (!svc) {
        printError(L"cannot create service");
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_DESCRIPTIONW desc{const_cast<LPWSTR>(kServiceDescription)};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    setFailureActions(svc);

    if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        printError(L"cannot start service");
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::printf("Service installed and started.\n");

    std::wstring tray = exeDirectory() + L"\\" + kTrayExe;
    if (GetFileAttributesW(tray.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring value = L"\"" + tray + L"\"";
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kRunKey, L"ext4win", REG_SZ, value.c_str(),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        // Start the tray app without administrator rights (via Explorer).
        ShellExecuteW(nullptr, L"open", L"explorer.exe", value.c_str(), nullptr, SW_SHOWNORMAL);
        std::printf("Tray app registered for autostart and started.\n");
    }
    return 0;
}

int uninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        printError(L"cannot open service manager");
        return 1;
    }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        printError(L"cannot open service");
        CloseServiceHandle(scm);
        return 1;
    }
    stopServiceAndWait(svc);
    BOOL ok = DeleteService(svc);
    if (!ok) printError(L"cannot delete service");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kRunKey, L"ext4win");
    if (ok) std::printf("Service removed. Close the tray app via its menu.\n");
    return ok ? 0 : 1;
}

int runCtl(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "usage: ext4win ctl LIST | MOUNT id [X] | UNMOUNT id | "
                             "POLICY id ask|always|never | ATTACH file | DETACH id | SUBSCRIBE\n");
        return 2;
    }
    std::vector<std::string> request;
    for (const auto& a : args) request.push_back(wideToUtf8(a));
    if (request[0] == "ATTACH" && request.size() > 1) {
        wchar_t full[MAX_PATH];
        if (GetFullPathNameW(args[1].c_str(), MAX_PATH, full, nullptr)) request[1] = wideToUtf8(full);
    }

    if (request[0] == "SUBSCRIBE") {
        PipeStream pipe;
        if (!pipe.connect() || !pipe.writeLine("SUBSCRIBE")) {
            std::fprintf(stderr, "error: service not reachable\n");
            return 1;
        }
        std::string line;
        while (pipe.readLine(line)) {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
        return 0;
    }

    std::vector<std::string> reply;
    std::string error;
    if (!pipeRequest(request, reply, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    for (const auto& l : reply) std::printf("%s\n", l.c_str());
    return !reply.empty() && reply.back().rfind("ERR", 0) == 0 ? 1 : 0;
}

}  // namespace ext4win
