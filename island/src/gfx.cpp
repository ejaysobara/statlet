#include "gfx.h"
#include "log.h"
#include <d2d1_3helper.h>
#include <dxgi1_6.h>
#include <windows.ui.composition.interop.h>
#include <algorithm>
#include <cmath>
#include <DispatcherQueue.h>
#include <dwmapi.h>

namespace abi = ABI::Windows::UI::Composition;
using winrt::check_hresult;
using winrt::com_ptr;

Gfx::Gfx(wuc::Compositor const& c, const std::wstring& fontPath, float s) : compositor(c), scale(s) {
    check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, d2dFactory.put_void()));
    check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(dwrite.put())));

    // Bundled Manrope (a variable font; its named instances cover weights 200 to 800). A missing or bad file leaves
    // `bundled` null and text falls back to Segoe UI Variable.
    com_ptr<IDWriteFontSetBuilder1> builder;
    com_ptr<IDWriteFontFile> file;
    com_ptr<IDWriteFontSet> set;
    if (SUCCEEDED(dwrite->CreateFontSetBuilder(builder.put())) && SUCCEEDED(dwrite->CreateFontFileReference(fontPath.c_str(), nullptr, file.put())) &&
        SUCCEEDED(builder->AddFontFile(file.get())) && SUCCEEDED(builder->CreateFontSet(set.put())))
        dwrite->CreateFontCollectionFromFontSet(set.get(), bundled.put());

    deviceLost = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CreateDevice();
}

Gfx::~Gfx() {
    if (d3d) d3d->UnregisterDeviceRemoved(lostCookie);
    CloseHandle(deviceLost);
}

void Gfx::CreateDevice() {
    // The island draws rarely, so it prefers the low-power GPU: faster to create (75-150 ms vs ~225 ms on the dGPU here)
    // and it leaves a discrete GPU asleep. WARP keeps the island alive on machines with no usable GPU.
    com_ptr<IDXGIFactory6> factory;
    com_ptr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), factory.put_void())))
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_MINIMUM_POWER, __uuidof(IDXGIAdapter1), adapter.put_void());
    com_ptr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
    if (FAILED(hr))
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                        D3D11_SDK_VERSION, device.put(), nullptr, nullptr));
    if (d3d) d3d->UnregisterDeviceRemoved(lostCookie);
    d3d = device.as<ID3D11Device4>();
    check_hresult(d3d->RegisterDeviceRemovedEvent(deviceLost, &lostCookie));

    d2d = nullptr;
    check_hresult(d2dFactory->CreateDevice(d3d.as<IDXGIDevice>().get(), d2d.put()));
    if (!graphics) {
        check_hresult(compositor.as<abi::ICompositorInterop>()->CreateGraphicsDevice(
            d2d.get(), reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(graphics))));
        graphics.RenderingDeviceReplaced([](auto&&, auto&&) { Log("gfx: rendering device replaced"); });
    } else
        check_hresult(graphics.as<abi::ICompositionGraphicsDeviceInterop>()->SetRenderingDevice(d2d.get()));
}

void Gfx::RedrawAll() {
    for (auto& [owner, redraw] : redraws) redraw();
}

// Returns false if the GPU is still resetting; the caller retries later.
bool Gfx::RecoverDevice() {
    recovering = true;
    bool ok = true;
    try {
        CreateDevice();
        RedrawAll();
    } catch (winrt::hresult_error const& e) {
        Log("gfx: recovery failed 0x%08X", unsigned(e.code()));
        ok = false;
    }
    recovering = false;
    return ok;
}

void Gfx::Draw(wuc::CompositionDrawingSurface& surface, UINT w, UINT h, const std::function<void(ID2D1DeviceContext*)>& paint) {
    w = std::max(w, 1u), h = std::max(h, 1u);
    if (!surface)
        surface = graphics.CreateDrawingSurface({float(w), float(h)}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    auto interop = surface.as<abi::ICompositionDrawingSurfaceInterop>();
    auto size = surface.SizeInt32();
    HRESULT hr = UINT(size.Width) != w || UINT(size.Height) != h ? interop->Resize({LONG(w), LONG(h)}) : S_OK;

    com_ptr<ID2D1DeviceContext> ctx;
    POINT offset{};
    if (SUCCEEDED(hr)) hr = interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), ctx.put_void(), &offset);
    if (SUCCEEDED(hr)) {
        ctx->SetDpi(96 * scale, 96 * scale);
        ctx->SetTransform(D2D1::Matrix3x2F::Translation(offset.x / scale, offset.y / scale));
        ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType needs an opaque target
        ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
        try {
            paint(ctx.get());
        } catch (winrt::hresult_error const& e) {
            hr = e.code();  // usually device removal mid-draw; EndDraw must still run
        }
        HRESULT end = interop->EndDraw();
        if (SUCCEEDED(hr)) hr = end;
    }
    // A lost device redraws every registered surface, this one included, once the owner calls RecoverDevice.
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET) {
        Log("gfx: draw lost the device 0x%08X%s", unsigned(hr), recovering ? " during recovery" : "");
        if (!recovering) SetEvent(deviceLost);
    }
    else
        check_hresult(hr);
}

winrt::Windows::System::DispatcherQueueController CreateDispatcherQueue() {
    DispatcherQueueOptions options{sizeof options, DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    winrt::Windows::System::DispatcherQueueController controller{nullptr};
    check_hresult(CreateDispatcherQueueController(options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller))));
    return controller;
}

wuc::Desktop::DesktopWindowTarget AttachCompositor(wuc::Compositor const& compositor, HWND hwnd) {
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof on);       // lets Acrylic sample what is behind
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof on);    // no fade when shown
    COLORREF none = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &none, sizeof none);
    DWM_WINDOW_CORNER_PREFERENCE square = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &square, sizeof square);

    wuc::Desktop::DesktopWindowTarget target{nullptr};
    check_hresult(compositor.as<abi::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
        hwnd, TRUE, reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
    return target;
}
