#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cmath>
#include <winrt/base.h>
#include "events.h"

namespace {

struct AudioWatch : winrt::implements<AudioWatch, IAudioEndpointVolumeCallback, IMMNotificationClient> {
    AudioWatch(HWND hwnd, UINT msg) : hwnd(hwnd), msg(msg) {
        enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        enumerator->RegisterEndpointNotificationCallback(this);
    }
    ~AudioWatch() {
        Detach();
        enumerator->UnregisterEndpointNotificationCallback(this);
    }

    void Attach() {
        Detach();
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put()))) return;  // no output device: nothing to watch
        winrt::com_ptr<IPropertyStore> store;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.put())) && SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            name = v.pwszVal;
        PropVariantClear(&v);
        if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, endpoint.put_void()))) return;
        endpoint->RegisterControlChangeNotify(this);
    }
    void Detach() {
        if (endpoint) endpoint->UnregisterControlChangeNotify(this);
        endpoint = nullptr;
    }

    // IAudioEndpointVolumeCallback
    HRESULT __stdcall OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        PostMessageW(hwnd, msg, WPARAM(std::lround(d->fMasterVolume * 100)), d->bMuted ? 1 : 0);
        return S_OK;
    }
    // IMMNotificationClient
    HRESULT __stdcall OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eMultimedia) PostMessageW(hwnd, msg, WPARAM(UINT_MAX), 0);
        return S_OK;
    }
    HRESULT __stdcall OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT __stdcall OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT __stdcall OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT __stdcall OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    HWND hwnd;
    UINT msg;
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    winrt::com_ptr<IAudioEndpointVolume> endpoint;
    std::wstring name;  // the default output's friendly name, e.g. "Speakers (Realtek(R) Audio)"
};

winrt::com_ptr<AudioWatch> audio;
HWND netWnd;
UINT netMsg;

bool Online(const NL_NETWORK_CONNECTIVITY_HINT& h) {
    return h.ConnectivityLevel == NetworkConnectivityLevelHintInternetAccess || h.ConnectivityLevel == NetworkConnectivityLevelHintConstrainedInternetAccess;
}

void CALLBACK OnHint(PVOID, NL_NETWORK_CONNECTIVITY_HINT hint) { PostMessageW(netWnd, netMsg, Online(hint), 0); }

}  // namespace

void WatchVolume(HWND hwnd, UINT msg) {
    audio = winrt::make_self<AudioWatch>(hwnd, msg);
    audio->Attach();
}

void RebindVolume() {
    if (audio) audio->Attach();
}

std::wstring OutputDeviceName() { return audio ? audio->name : std::wstring(); }

void WatchNetwork(HWND hwnd, UINT msg) {
    netWnd = hwnd;
    netMsg = msg;
    HANDLE handle;
    NotifyNetworkConnectivityHintChange(OnHint, nullptr, FALSE, &handle);  // lives as long as the process
}

bool InternetOnline() {
    NL_NETWORK_CONNECTIVITY_HINT hint{};
    return GetNetworkConnectivityHint(&hint) != NO_ERROR || Online(hint);  // unknown counts as online: never raise a false alarm
}
