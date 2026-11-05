#include "media.h"
#include "log.h"
#include <shcore.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt::Windows::Media::Control;
using winrt::check_hresult;
using winrt::com_ptr;

namespace {

HWND wnd;
UINT message;
GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
GlobalSystemMediaTransportControlsSession session{nullptr};
winrt::event_token propsToken, playToken, timeToken;
winrt::Windows::Storage::Streams::IRandomAccessStreamReference thumbnail{nullptr};

void Changed() { PostMessageW(wnd, message, 0, 0); }  // session events arrive on worker threads

double Seconds(winrt::Windows::Foundation::TimeSpan d) { return std::chrono::duration<double>(d).count(); }

winrt::fire_and_forget Start() {
    try {
        manager = co_await GlobalSystemMediaTransportControlsSessionManager::RequestAsync();  // resumes on the window thread
    } catch (...) {
        co_return;  // media sessions unavailable: the island simply never shows music
    }
    manager.CurrentSessionChanged([](auto&&, auto&&) { PostMessageW(wnd, message, 1, 0); });
    RebindMedia();
}

winrt::fire_and_forget ReadMediaAsync(std::function<void(MediaInfo)> done) {
    MediaInfo m;
    if (auto s = session) {
        try {
            auto props = co_await s.TryGetMediaPropertiesAsync();
            auto time = s.GetTimelineProperties();
            auto info = s.GetPlaybackInfo();
            // A closing player can answer with null objects; calling into one is an access violation, which
            // catch (...) does not catch under /EHsc (two island crash dumps, 2026-11-03 and -04).
            if (!props || !time || !info) {
                Log("media: player gave no %s", !props ? "properties" : !time ? "timeline" : "playback info");
                throw winrt::hresult_error(E_POINTER);
            }
            m.present = true;
            m.playing = info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            m.title = props.Title();
            m.artist = props.Artist();
            thumbnail = props.Thumbnail();
            m.duration = std::max(0.0, Seconds(time.EndTime() - time.StartTime()));
            m.position = Seconds(time.Position() - time.StartTime());
            if (m.playing)  // the position was sampled at LastUpdatedTime
                m.position += std::max(0.0, Seconds(winrt::clock::now() - time.LastUpdatedTime()));
            if (m.duration > 0) m.position = std::clamp(m.position, 0.0, m.duration);
        } catch (...) {
            m = {};  // the player went away mid-read
        }
    }
    done(std::move(m));
}

winrt::fire_and_forget ReadArtAsync(std::function<void(com_ptr<IWICBitmapSource>)> done) {
    com_ptr<IWICBitmapSource> result;
    if (auto ref = thumbnail) {
        try {
            auto stream = co_await ref.OpenReadAsync();
            com_ptr<IStream> s;
            check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(s.put())));
            auto wic = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
            com_ptr<IWICBitmapDecoder> decoder;
            check_hresult(wic->CreateDecoderFromStream(s.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put()));
            com_ptr<IWICBitmapFrameDecode> frame;
            check_hresult(decoder->GetFrame(0, frame.put()));
            com_ptr<IWICFormatConverter> converter;
            check_hresult(wic->CreateFormatConverter(converter.put()));
            check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut));
            com_ptr<IWICBitmap> bitmap;  // decoded once into memory, so redraws after device loss need no stream
            check_hresult(wic->CreateBitmapFromSource(converter.get(), WICBitmapCacheOnLoad, bitmap.put()));
            result = bitmap;
        } catch (...) {
            // no art, or an image WIC cannot read: the plain tile stays
        }
    }
    done(result);
}

}  // namespace

void WatchMedia(HWND hwnd, UINT msg) {
    wnd = hwnd;
    message = msg;
    Start();
}

void RebindMedia() {
    if (!manager) return;
    if (session) {
        session.MediaPropertiesChanged(propsToken);
        session.PlaybackInfoChanged(playToken);
        session.TimelinePropertiesChanged(timeToken);
    }
    session = manager.GetCurrentSession();
    if (session) {
        propsToken = session.MediaPropertiesChanged([](auto&&, auto&&) { Changed(); });
        playToken = session.PlaybackInfoChanged([](auto&&, auto&&) { Changed(); });
        timeToken = session.TimelinePropertiesChanged([](auto&&, auto&&) { Changed(); });
    }
    Changed();
}

void ReadMedia(std::function<void(MediaInfo)> done) { ReadMediaAsync(std::move(done)); }
void ReadArt(std::function<void(com_ptr<IWICBitmapSource>)> done) { ReadArtAsync(std::move(done)); }

void SendMedia(MediaCommand c) {
    if (!session) return;
    try {  // the player answers through the usual session events
        if (c == MediaCommand::Previous) session.TrySkipPreviousAsync();
        else if (c == MediaCommand::Next) session.TrySkipNextAsync();
        else session.TryTogglePlayPauseAsync();
    } catch (...) {
    }
}
