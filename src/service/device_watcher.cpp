// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/device_watcher.h"

#include "service/log.h"

#include <windows.h>
#include <initguid.h>  // defines GUID_DEVINTERFACE_DISK in this file
#include <cfgmgr32.h>
#include <winioctl.h>

#include <vector>

namespace ext4win {

namespace {

DWORD CALLBACK onDeviceEvent(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                             PCM_NOTIFY_EVENT_DATA data, DWORD) {
    auto* self = static_cast<DeviceWatcher*>(context);
    if (data->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) return ERROR_SUCCESS;
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL)
        self->post(data->u.DeviceInterface.SymbolicLink, true);
    else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
        self->post(data->u.DeviceInterface.SymbolicLink, false);
    return ERROR_SUCCESS;
}

std::vector<std::wstring> presentDisks() {
    std::vector<std::wstring> out;
    GUID guid = GUID_DEVINTERFACE_DISK;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, &guid, nullptr,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return out;
    std::vector<wchar_t> buf(len + 1);
    if (CM_Get_Device_Interface_ListW(&guid, nullptr, buf.data(), len,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return out;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}

}  // namespace

DeviceWatcher::~DeviceWatcher() { stop(); }

bool DeviceWatcher::start(Callback cb) {
    callback_ = std::move(cb);
    stopping_ = false;
    worker_ = std::thread([this] { run(); });

    for (const auto& path : presentDisks()) post(path, true);

    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof filter;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_DISK;
    HCMNOTIFICATION h = nullptr;
    CONFIGRET cr = CM_Register_Notification(&filter, this, onDeviceEvent, &h);
    if (cr != CR_SUCCESS) {
        logLine("CM_Register_Notification failed: " + std::to_string(cr));
        return false;
    }
    notification_ = h;
    return true;
}

void DeviceWatcher::stop() {
    if (notification_) {
        CM_Unregister_Notification(static_cast<HCMNOTIFICATION>(notification_));
        notification_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void DeviceWatcher::post(const std::wstring& devicePath, bool arrived) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.emplace_back(devicePath, arrived);
    }
    cv_.notify_one();
}

void DeviceWatcher::run() {
    for (;;) {
        std::pair<std::wstring, bool> ev;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            ev = std::move(queue_.front());
            queue_.pop_front();
        }
        callback_(ev.first, ev.second);
    }
}

}  // namespace ext4win
