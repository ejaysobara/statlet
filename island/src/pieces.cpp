// The five drawing pieces declared in gfx.h.
#include "gfx.h"
#include <d2d1effects.h>
#include <algorithm>
#include <cmath>

using winrt::check_hresult;
using winrt::com_ptr;

static D2D1_COLOR_F D2DColor(Rgba c) { return {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f}; }

static wuc::ExpressionAnimation Expr(Gfx& gfx, const wchar_t* expression, wuc::CompositionPropertySet const& props) {
    auto animation = gfx.compositor.CreateExpressionAnimation(expression);
    animation.SetReferenceParameter(L"p", props);
    return animation;
}

RoundedShape::RoundedShape(Gfx& g, wuc::CompositionPropertySet const& props, const Tokens& t, float maxR) : gfx(g), tokens(t), maxRadius(maxR) {
    auto c = gfx.compositor;
    root = c.CreateContainerVisual();
    root.RelativeSizeAdjustment({1, 1});

    // Shadow: a blurred rounded rect with the shape cut out, nine-grid stretched to the shape plus the blur on each side.
    shadowBrush = c.CreateNineGridBrush();
    shadowBrush.IsCenterHollow(true);  // the middle is under the glass
    auto shadow = c.CreateSpriteVisual();
    shadow.Brush(shadowBrush);
    auto offset = Expr(gfx, L"Vector3(p.X - pad, p.Y + dy - pad, 0)", props);
    offset.SetScalarParameter(L"pad", t.shadowBlur);
    offset.SetScalarParameter(L"dy", t.shadowY);
    shadow.StartAnimation(L"Offset", offset);
    auto size = Expr(gfx, L"Vector2(p.W + 2 * pad, p.H + 2 * pad)", props);
    size.SetScalarParameter(L"pad", t.shadowBlur);
    shadow.StartAnimation(L"Size", size);
    root.Children().InsertAtTop(shadow);
    gfx.OnRedraw(this, [this] { DrawShadow(); });
    DrawShadow();

    content = c.CreateContainerVisual();
    content.RelativeSizeAdjustment({1, 1});
    auto clip = c.CreateRoundedRectangleGeometry();
    clip.StartAnimation(L"Offset", Expr(gfx, L"Vector2(p.X, p.Y)", props));
    clip.StartAnimation(L"Size", Expr(gfx, L"Vector2(p.W, p.H)", props));
    clip.StartAnimation(L"CornerRadius", Expr(gfx, L"Vector2(p.R, p.R)", props));
    content.Clip(c.CreateGeometricClip(clip));
    root.Children().InsertAtTop(content);

    // Inner edge, above the content and inset by half its thickness so the whole stroke sits inside the shape.
    auto edge = c.CreateRoundedRectangleGeometry();
    const std::pair<const wchar_t*, const wchar_t*> edgeExpressions[] = {
        {L"Offset", L"Vector2(p.X + h, p.Y + h)"},
        {L"Size", L"Vector2(p.W - 2 * h, p.H - 2 * h)"},
        {L"CornerRadius", L"Vector2(Max(p.R - h, 0), Max(p.R - h, 0))"},
    };
    for (auto [property, expression] : edgeExpressions) {
        auto animation = Expr(gfx, expression, props);
        animation.SetScalarParameter(L"h", t.edgeThickness / 2);
        edge.StartAnimation(property, animation);
    }
    auto stroke = c.CreateSpriteShape(edge);
    stroke.StrokeBrush(c.CreateColorBrush(ToColor(t.edge)));
    stroke.StrokeThickness(t.edgeThickness);
    auto edgeVisual = c.CreateShapeVisual();
    edgeVisual.RelativeSizeAdjustment({1, 1});
    edgeVisual.Shapes().Append(stroke);
    root.Children().InsertAtTop(edgeVisual);
}

// Corner patches of blur + radius, and a middle long enough that the blur reaches a steady edge before it is stretched.
float RoundedShape::ShadowSide(const Tokens& t, float maxRadius) { return 2 * (t.shadowBlur + maxRadius) + t.shadowBlur; }

void RoundedShape::PaintShadow(ID2D1DeviceContext* ctx, const Tokens& t, float r) {
    float pad = t.shadowBlur, side = ShadowSide(t, r);
    com_ptr<ID2D1CommandList> shape;
    com_ptr<ID2D1Image> target;
    com_ptr<ID2D1SolidColorBrush> brush;
    D2D1_MATRIX_3X2_F transform;
    check_hresult(ctx->CreateCommandList(shape.put()));
    ctx->GetTarget(target.put());
    ctx->GetTransform(&transform);
    ctx->SetTarget(shape.get());
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());  // DrawImage applies the transform; record without it
    check_hresult(ctx->CreateSolidColorBrush(D2DColor(t.shadow), brush.put()));
    ctx->FillRoundedRectangle({{pad, pad, side - pad, side - pad}, r, r}, brush.get());
    check_hresult(shape->Close());
    ctx->SetTarget(target.get());
    ctx->SetTransform(transform);
    com_ptr<ID2D1Effect> blur;
    check_hresult(ctx->CreateEffect(CLSID_D2D1GaussianBlur, blur.put()));
    blur->SetInput(0, shape.get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, pad / 2);  // a CSS-style blur radius is two standard deviations
    ctx->DrawImage(blur.get());
    // No cut-out for the shape: the glass's backdrop layer is opaque and hides the shadow under it. A cut-out drawn at
    // maxRadius left an unshadowed band below small shapes, where the nine-grid squeezes its patches.
}

void RoundedShape::DrawShadow() {
    UINT px = UINT(std::ceil(ShadowSide(tokens, maxRadius) * gfx.scale));
    bool first = !shadowSurface;
    gfx.Draw(shadowSurface, px, px, [this](ID2D1DeviceContext* ctx) { PaintShadow(ctx, tokens, maxRadius); });
    if (first) {
        auto source = gfx.compositor.CreateSurfaceBrush(shadowSurface);
        source.Stretch(wuc::CompositionStretch::Fill);  // the nine-grid only stretches its source with Fill
        shadowBrush.Source(source);
    }
    shadowBrush.SetInsets((tokens.shadowBlur + maxRadius) * gfx.scale);  // insets are in source pixels; the scale maps them back to epx
    shadowBrush.SetInsetScales(1 / gfx.scale);
}

Text::Text(Gfx& g, TextStyle s, float maxW) : gfx(g), style(s), maxWidth(maxW) {
    visual = gfx.compositor.CreateSpriteVisual();
    gfx.OnRedraw(this, [this] { Redraw(); });
}

void Text::Set(std::wstring_view value) {
    if (layout && value == text) return;
    text = value;
    Layout();
    Redraw();
}

void Text::SetColor(Rgba color) {
    style.color = color;
    Redraw();
}

void Text::SetMaxWidth(float w) {
    if (w == maxWidth) return;
    maxWidth = w;
    if (!layout) return;
    Layout();
    Redraw();
}

void Text::Layout() {
    bool own = !style.family && gfx.bundled;
    const wchar_t* family = style.family ? style.family : own ? L"Manrope" : L"Segoe UI Variable Text";
    com_ptr<IDWriteTextFormat> format;
    check_hresult(gfx.dwrite->CreateTextFormat(family, own ? gfx.bundled.get() : nullptr, style.weight, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, style.size, L"en-us", format.put()));
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    com_ptr<IDWriteInlineObject> ellipsis;
    check_hresult(gfx.dwrite->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()));
    format->SetTrimming(&trimming, ellipsis.get());

    float limit = maxWidth > 0 ? maxWidth : 4096;
    UINT32 length = UINT32(text.size());
    layout = nullptr;
    check_hresult(gfx.dwrite->CreateTextLayout(text.data(), length, format.get(), limit, 1024, layout.put()));
    if (style.tabular) {
        com_ptr<IDWriteTypography> typography;
        check_hresult(gfx.dwrite->CreateTypography(typography.put()));
        typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
        layout->SetTypography(typography.get(), {0, length});
    }
    if (style.tracking) layout.as<IDWriteTextLayout1>()->SetCharacterSpacing(0, style.tracking * style.size, 0, {0, length});
    DWRITE_TEXT_METRICS m;
    check_hresult(layout->GetMetrics(&m));
    width = std::min(m.widthIncludingTrailingWhitespace, limit);
    height = m.height;
}

void Text::Redraw() {
    if (!layout) return;
    UINT w = UINT(std::ceil(width * gfx.scale)), h = UINT(std::ceil(height * gfx.scale));
    bool first = !surface;
    gfx.Draw(surface, w, h, [this](ID2D1DeviceContext* ctx) { Paint(ctx); });
    if (first) {
        auto brush = gfx.compositor.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::Fill);
        brush.SnapToPixels(true);  // keeps glyphs crisp at fractional offsets
        visual.Brush(brush);
    }
    visual.Size({w / gfx.scale, h / gfx.scale});
}

void Text::Paint(ID2D1DeviceContext* ctx) {
    com_ptr<ID2D1SolidColorBrush> brush;
    check_hresult(ctx->CreateSolidColorBrush(D2DColor(style.color), brush.put()));
    ctx->DrawTextLayout({0, 0}, layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

std::unique_ptr<Text> MakeIcon(Gfx& gfx, wchar_t glyph, float size, Rgba color) {
    // Segoe Fluent Icons ships with Windows 11; Segoe MDL2 Assets (Windows 10) has the same code points for common glyphs.
    com_ptr<IDWriteFontCollection> system;
    UINT32 index;
    BOOL fluent = FALSE;
    if (SUCCEEDED(gfx.dwrite->GetSystemFontCollection(system.put()))) system->FindFamilyName(L"Segoe Fluent Icons", &index, &fluent);
    auto family = fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    auto icon = std::make_unique<Text>(gfx, TextStyle{size, DWRITE_FONT_WEIGHT_NORMAL, color, false, 0, family});
    icon->Set({&glyph, 1});
    return icon;
}

Bar::Bar(Gfx& gfx, float w, float h, Rgba track, Rgba fill) : width(w), height(h) {
    auto c = gfx.compositor;
    visual = c.CreateShapeVisual();
    visual.Size({w, h});
    auto trackGeometry = c.CreateRoundedRectangleGeometry();
    trackGeometry.Size({w, h});
    trackGeometry.CornerRadius({h / 2, h / 2});
    auto trackShape = c.CreateSpriteShape(trackGeometry);
    trackShape.FillBrush(c.CreateColorBrush(ToColor(track)));
    fillGeometry = c.CreateRoundedRectangleGeometry();
    fillGeometry.CornerRadius({h / 2, h / 2});
    fillBrush = c.CreateColorBrush(ToColor(fill));
    auto fillShape = c.CreateSpriteShape(fillGeometry);
    fillShape.FillBrush(fillBrush);
    visual.Shapes().Append(trackShape);
    visual.Shapes().Append(fillShape);
    SetProgress(0);
}

void Bar::SetProgress(float p) { fillGeometry.Size({width * std::clamp(p, 0.f, 1.f), height}); }

Picture::Picture(Gfx& g, float s, float r, Rgba e) : gfx(g), size(s), radius(r), empty(e) {
    visual = gfx.compositor.CreateSpriteVisual();
    visual.Size({size, size});
    gfx.OnRedraw(this, [this] { Redraw(); });
    Redraw();
}

void Picture::Set(com_ptr<IWICBitmapSource> s) {
    source = std::move(s);
    Redraw();
}

void Picture::Redraw() {
    UINT px = UINT(std::ceil(size * gfx.scale));
    bool first = !surface;
    gfx.Draw(surface, px, px, [this](ID2D1DeviceContext* ctx) {
        D2D1_ROUNDED_RECT shape{{0, 0, size, size}, radius, radius};
        com_ptr<ID2D1Bitmap> bitmap;
        if (!source || FAILED(ctx->CreateBitmapFromWicBitmap(source.get(), bitmap.put()))) {
            com_ptr<ID2D1SolidColorBrush> brush;
            check_hresult(ctx->CreateSolidColorBrush(D2DColor(empty), brush.put()));
            ctx->FillRoundedRectangle(shape, brush.get());
            return;
        }
        auto b = bitmap->GetSize();
        float k = size / std::min(b.width, b.height);  // cover: the shorter side fills, the longer one is cropped evenly
        com_ptr<ID2D1BitmapBrush1> brush;
        D2D1_BITMAP_BRUSH_PROPERTIES1 props{D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC};
        check_hresult(ctx->CreateBitmapBrush(bitmap.get(), &props, nullptr, brush.put()));
        brush->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation((size - b.width * k) / 2, (size - b.height * k) / 2));
        ctx->FillRoundedRectangle(shape, brush.get());
    });
    if (first) {
        auto brush = gfx.compositor.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::Fill);
        visual.Brush(brush);
    }
}

Acrylic::Acrylic(Gfx& gfx, const Tokens& t) : tokens(t) {
    auto c = gfx.compositor;
    root = c.CreateContainerVisual();
    root.RelativeSizeAdjustment({1, 1});
    backdrop = c.CreateSpriteVisual();
    backdrop.RelativeSizeAdjustment({1, 1});
    backdrop.Brush(c.CreateHostBackdropBrush());
    Rgba lum = t.glassTint;
    lum.a = uint8_t(std::lround(t.luminosityOpacity * 255));
    luminosity = c.CreateSpriteVisual();
    luminosity.RelativeSizeAdjustment({1, 1});
    luminosity.Brush(c.CreateColorBrush(ToColor(lum)));
    tintBrush = c.CreateColorBrush();
    tint = c.CreateSpriteVisual();
    tint.RelativeSizeAdjustment({1, 1});
    tint.Brush(tintBrush);
    for (auto& layer : {backdrop, luminosity, tint}) root.Children().InsertAtTop(layer);
    SetEffectsEnabled(true);
}

void Acrylic::SetEffectsEnabled(bool on) {
    backdrop.IsVisible(on);
    luminosity.IsVisible(on);
    Rgba glass = tokens.glassTint;
    glass.a = uint8_t(std::lround(tokens.tintOpacity * 255));
    tintBrush.Color(ToColor(on ? glass : tokens.fallback));
}
