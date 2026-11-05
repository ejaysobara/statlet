#pragma once
// Graphics context and the five drawing pieces every widget builds from: RoundedShape, Text, Icon, Bar, Acrylic.
// Everything sits in a Windows.UI.Composition tree measured in epx (the root visual carries the DPI scale).
// Only Text and the shadow are drawn with Direct2D, and only when their content changes; resizing and animating
// are compositor properties, so an idle or animating island runs no app code per frame.
#include <windows.h>
#include <d2d1_3.h>
#include <d3d11_4.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include "tokens.h"

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

inline winrt::Windows::UI::Color ToColor(Rgba c) { return {c.a, c.r, c.g, c.b}; }

// Window plumbing shared by the shell and the screenshot tests. The window should have WS_EX_NOREDIRECTIONBITMAP.
winrt::Windows::System::DispatcherQueueController CreateDispatcherQueue();
wuc::Desktop::DesktopWindowTarget AttachCompositor(wuc::Compositor const& compositor, HWND hwnd);  // also sets the DWM attributes Acrylic needs

// Owns the Direct3D/Direct2D/DirectWrite devices behind the composition drawing surfaces, and the bundled font.
// On GPU device loss it builds a new device and redraws every registered surface.
class Gfx {
public:
    Gfx(wuc::Compositor const& compositor, const std::wstring& fontPath, float scale);
    Gfx(const Gfx&) = delete;
    ~Gfx();

    wuc::Compositor compositor;
    wuc::CompositionGraphicsDevice graphics{nullptr};
    winrt::com_ptr<ID2D1Factory1> d2dFactory;
    winrt::com_ptr<ID2D1Device> d2d;
    winrt::com_ptr<IDWriteFactory5> dwrite;
    winrt::com_ptr<IDWriteFontCollection1> bundled;  // Manrope, or null when the file failed to load
    float scale;                                     // physical px per epx
    HANDLE deviceLost = nullptr;                     // signaled by Direct3D when the device is removed

    // Surfaces register how to redraw themselves (called after device loss and DPI changes) and forget it when destroyed.
    void OnRedraw(const void* owner, std::function<void()> redraw) { redraws.emplace_back(owner, std::move(redraw)); }
    void Forget(const void* owner) { std::erase_if(redraws, [owner](auto& r) { return r.first == owner; }); }
    void RedrawAll();
    bool RecoverDevice();
    void SetScale(float s) { scale = s; RedrawAll(); }

    // Draws into a surface of w x h physical px with a context whose units are epx. Recovers from device loss.
    void Draw(wuc::CompositionDrawingSurface& surface, UINT w, UINT h, const std::function<void(ID2D1DeviceContext*)>& paint);

private:
    void CreateDevice();
    winrt::com_ptr<ID3D11Device4> d3d;
    DWORD lostCookie = 0;
    bool recovering = false;
    std::vector<std::pair<const void*, std::function<void()>>> redraws;
};

// Piece 1. A rounded rectangle whose X, Y, W, H, R (epx) are scalars in a property set, usually animated.
// `content` is clipped to it with antialiased corners; an inner edge stroke and a soft drop shadow follow it.
// shortcut: the nine-grid shadow is drawn once at maxRadius, so its corners and cut-out differ slightly at other radii.
class RoundedShape {
public:
    RoundedShape(Gfx& gfx, wuc::CompositionPropertySet const& props, const Tokens& t, float maxRadius);
    RoundedShape(const RoundedShape&) = delete;  // registered with Gfx by address
    ~RoundedShape() { gfx.Forget(this); }
    wuc::ContainerVisual root{nullptr};     // add this to the parent
    wuc::ContainerVisual content{nullptr};  // add acrylic and widget visuals here
    static float ShadowSide(const Tokens& t, float maxRadius);                 // the shadow image's size in epx
    static void PaintShadow(ID2D1DeviceContext* ctx, const Tokens& t, float maxRadius);  // also used by the tests

private:
    void DrawShadow();
    Gfx& gfx;
    Tokens tokens;
    float maxRadius;
    wuc::CompositionDrawingSurface shadowSurface{nullptr};
    wuc::CompositionNineGridBrush shadowBrush{nullptr};
};

struct TextStyle {
    float size;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    Rgba color;
    bool tabular = false;   // tabular numerals, for digits that change
    float tracking = 0;     // letter spacing as a fraction of size (Type.Label uses 0.08)
    const wchar_t* family = nullptr;  // null: Manrope with Segoe UI Variable fallback
};

// Piece 2. One line of DirectWrite text in a sprite sized to the text; trims with an ellipsis past maxWidth.
// Redraws only when the text, color or DPI changes.
class Text {
public:
    Text(Gfx& gfx, TextStyle style, float maxWidth = 0);
    Text(const Text&) = delete;  // registered with Gfx by address
    ~Text() { gfx.Forget(this); }
    wuc::SpriteVisual visual{nullptr};
    void Set(std::wstring_view text);
    void SetColor(Rgba color);
    void SetMaxWidth(float w);  // 0: no limit
    float Width() const { return width; }
    float Height() const { return height; }
    void Paint(ID2D1DeviceContext* ctx);  // draws at the origin, in epx; also used by the tests

private:
    void Layout();
    void Redraw();
    Gfx& gfx;
    TextStyle style;
    float maxWidth, width = 0, height = 0;
    std::wstring text;
    winrt::com_ptr<IDWriteTextLayout> layout;
    wuc::CompositionDrawingSurface surface{nullptr};
};

// Piece 3. A Segoe Fluent Icons glyph (Windows 11), drawn by the text piece.
std::unique_ptr<Text> MakeIcon(Gfx& gfx, wchar_t glyph, float size, Rgba color);

// Piece 4. A rounded progress bar: track plus accent fill, vector shapes, so progress changes never redraw.
class Bar {
public:
    Bar(Gfx& gfx, float width, float height, Rgba track, Rgba fill);
    wuc::ShapeVisual visual{nullptr};
    void SetProgress(float p);  // 0..1
    void SetFill(Rgba fill) { fillBrush.Color(ToColor(fill)); }

private:
    float width, height;
    wuc::CompositionRoundedRectangleGeometry fillGeometry{nullptr};
    wuc::CompositionColorBrush fillBrush{nullptr};
};

// A picture with rounded corners (album art), scaled to cover a size x size square. Without a picture it is a plain
// tile in `empty`. Redraws only when the picture or DPI changes.
class Picture {
public:
    Picture(Gfx& gfx, float size, float radius, Rgba empty);
    Picture(const Picture&) = delete;  // registered with Gfx by address
    ~Picture() { gfx.Forget(this); }
    wuc::SpriteVisual visual{nullptr};
    void Set(winrt::com_ptr<IWICBitmapSource> source);

private:
    void Redraw();
    Gfx& gfx;
    float size, radius;
    Rgba empty;
    winrt::com_ptr<IWICBitmapSource> source;
    wuc::CompositionDrawingSurface surface{nullptr};
};

// Piece 5. Dark acrylic: DWM's live blur of what is behind the window under the glass tint, filling its parent.
// The window needs DWMWA_USE_HOSTBACKDROPBRUSH. Falls back to the solid color when effects are off.
// shortcut: the luminosity layer is a plain tint-colored layer, not a luminosity blend, and there is no noise texture
// (both need custom effect graphs); tune Island.Glass opacities by eye.
class Acrylic {
public:
    Acrylic(Gfx& gfx, const Tokens& t);
    wuc::ContainerVisual root{nullptr};
    void SetEffectsEnabled(bool on);

private:
    Tokens tokens;
    wuc::SpriteVisual backdrop{nullptr}, luminosity{nullptr}, tint{nullptr};
    wuc::CompositionColorBrush tintBrush{nullptr};
};
