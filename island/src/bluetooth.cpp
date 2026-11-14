#include "bluetooth.h"
#include <memory>
#include <mutex>
#include <vector>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Enumeration;

namespace {
// One watcher each for paired classic and paired LE devices (the spike's selectors); each has its own tracker.
struct Source {
    DeviceWatcher watcher{nullptr};
    BtTracker tracker;
    DeviceWatcher::Added_revoker added;
    DeviceWatcher::Updated_revoker updated;
    DeviceWatcher::Removed_revoker removed;
    DeviceWatcher::EnumerationCompleted_revoker done;
};
std::mutex lock;  // the callbacks run on thread-pool threads
// shared_ptr: a callback already running on a thread-pool thread keeps its Source alive while StopBluetooth lets go of it.
std::vector<std::shared_ptr<Source>> sources;

bool Connected(winrt::Windows::Foundation::Collections::IMapView<winrt::hstring, winrt::Windows::Foundation::IInspectable> const& props, bool& value) {
    auto v = props.TryLookup(L"System.Devices.Aep.IsConnected");
    if (!v) return false;
    value = winrt::unbox_value_or<bool>(v, false);
    return true;
}
}  // namespace

void WatchBluetooth(HWND hwnd, UINT msg) {
    StopBluetooth();
    try {
        std::lock_guard guard(lock);
        for (auto selector : {BluetoothDevice::GetDeviceSelectorFromPairingState(true), BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)}) {
            auto s = std::make_shared<Source>();
            std::shared_ptr<Source> src = s;  // each handler holds one; revoke() in StopBluetooth releases them
            s->watcher = DeviceInformation::CreateWatcher(selector, {L"System.Devices.Aep.IsConnected"}, DeviceInformationKind::AssociationEndpoint);
            auto changed = [src, hwnd, msg](winrt::hstring const& id, auto const& props) {
                bool connected;
                std::lock_guard guard(lock);
                if (Connected(props, connected) && src->tracker.Update(id.c_str(), connected)) PostMessageW(hwnd, msg, 0, 0);
            };
            s->added = s->watcher.Added(winrt::auto_revoke, [changed](auto&&, DeviceInformation const& d) { changed(d.Id(), d.Properties()); });
            s->updated = s->watcher.Updated(winrt::auto_revoke, [changed](auto&&, DeviceInformationUpdate const& u) { changed(u.Id(), u.Properties()); });
            s->removed = s->watcher.Removed(winrt::auto_revoke, [src](auto&&, DeviceInformationUpdate const& u) {
                std::lock_guard guard(lock);
                src->tracker.Remove(u.Id().c_str());
            });
            s->done = s->watcher.EnumerationCompleted(winrt::auto_revoke, [src](auto&&, auto&&) {
                std::lock_guard guard(lock);
                src->tracker.EnumerationDone();
            });
            s->watcher.Start();
            sources.push_back(std::move(s));
        }
    } catch (...) {  // no Bluetooth radio or the service is off: the icon simply never shows
        StopBluetooth();
    }
}

void StopBluetooth() {
    std::vector<std::shared_ptr<Source>> old;
    {
        std::lock_guard guard(lock);
        old.swap(sources);
    }
    for (auto& s : old) {
        try {
            s->added.revoke(), s->updated.revoke(), s->removed.revoke(), s->done.revoke();  // no callback after this
            auto status = s->watcher.Status();
            if (status == DeviceWatcherStatus::Started || status == DeviceWatcherStatus::EnumerationCompleted) s->watcher.Stop();
        } catch (...) {
        }
    }
}
