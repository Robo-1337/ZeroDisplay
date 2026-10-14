// Trim a recording: pick an MP4, choose start/end with a preview, save a copy without re-encoding.
#include "App.h"
#include "core/Log.h"

#include <shlobj.h>
#include <shobjidl.h>

namespace cc {

void App::PickTrimFile()
{
    const std::wstring folder = RecordFolder();
    m_graveyard.push_back(std::async(std::launch::async, [this, folder] {
        ComInit com(COINIT_APARTMENTTHREADED);
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        COMDLG_FILTERSPEC spec[2] = {{L"Videos (.mp4)", L"*.mp4;*.m4v;*.mov"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, spec);
        dlg->SetTitle(L"Choose a recording to trim");
        ComPtr<IShellItem> start;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&start)))) dlg->SetFolder(start.Get());
        if (FAILED(dlg->Show(m_hwnd))) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
        {
            std::lock_guard lock(m_pickMutex);
            m_trimPicked = path;
        }
        CoTaskMemFree(path);
        SetEvent(m_wake);
    }));
}

void App::OpenTrim(const std::wstring& path)
{
    auto t = std::make_unique<TrimState>();
    t->path = path;
    t->preview = std::make_unique<TrimPreview>();
    if (!t->preview->Open(path)) {
        Toast(L"That file can't be opened");
        return;
    }
    t->duration = t->preview->Duration();
    t->end = float(t->duration);
    t->preview->Request(0.0);
    m_trim = std::move(t);
    m_settingsOpen = false;
    LOGI(L"Trim: %s (%.1f s)", path.c_str(), m_trim->duration);
}

void App::UpdateTrim()
{
    {
        std::wstring picked;
        {
            std::lock_guard lock(m_pickMutex);
            picked.swap(m_trimPicked);
        }
        if (!picked.empty()) OpenTrim(picked);
    }
    if (!m_trim) return;
    TrimState& t = *m_trim;
    std::vector<uint8_t> img;
    UINT w = 0, h = 0;
    if (t.preview->Take(img, w, h) && w && h) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{img.data(), w * 4, 0};
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(m_renderer.Device()->CreateTexture2D(&td, &init, &tex))) {
            t.srv.Reset();
            m_renderer.Device()->CreateShaderResourceView(tex.Get(), nullptr, &t.srv);
            t.imgW = w;
            t.imgH = h;
        }
    }
    // Ask for a new preview once the slider has been still for a moment.
    if (t.wantPreview >= 0 && QpcElapsedMs(t.changedAt) > 120) {
        t.preview->Request(t.wantPreview);
        t.wantPreview = -1;
    }
}

App::TrimLayout App::LayoutTrim() const
{
    const float W = m_ui.Width(), H = m_ui.Height();
    TrimLayout l;
    const float cw = std::min(W - m_ui.S(32), m_ui.S(720));
    const float pw = cw - m_ui.S(40), ph = pw * 9.0f / 16.0f;
    const float ch = m_ui.S(60) + ph + m_ui.S(200);
    l.card = {(W - cw) * 0.5f, std::max(m_ui.S(8), (H - ch) * 0.5f), cw, ch};
    l.preview = {l.card.x + m_ui.S(20), l.card.y + m_ui.S(56), pw, ph};
    return l;
}

void App::DrawTrimPreview()
{
    if (!m_trim || !m_trim->srv) return;
    const TrimLayout l = LayoutTrim();
    m_renderer.DrawTexture(m_trim->srv.Get(), Renderer::FitRect(m_trim->imgW, m_trim->imgH, l.preview, ScaleMode::Fit), l.preview, true);
}

static std::wstring Clock(double s)
{
    const int ms = int(s * 1000.0 + 0.5);
    return Format(L"%d:%02d.%d", ms / 60000, (ms / 1000) % 60, (ms / 100) % 10);
}

void App::DrawTrimDialog()
{
    if (!m_trim) return;
    Ui& u = m_ui;
    TrimState& t = *m_trim;
    const TrimLayout l = LayoutTrim();
    const float W = u.Width(), H = u.Height();
    // Dim everything around the card (the preview itself is drawn by the GPU underneath the UI).
    const Color dim{0, 0, 0, 0.6f};
    u.Fill({0, 0, W, l.card.y}, dim);
    u.Fill({0, l.card.Bottom(), W, H - l.card.Bottom()}, dim);
    u.Fill({0, l.card.y, l.card.x, l.card.h}, dim);
    u.Fill({l.card.Right(), l.card.y, W - l.card.Right(), l.card.h}, dim);
    // Card, with a hole for the preview.
    const RectF img = t.srv ? Renderer::FitRect(t.imgW, t.imgH, l.preview, ScaleMode::Fit) : RectF{l.preview.x, l.preview.y, 0, 0};
    u.Fill({l.card.x, l.card.y, l.card.w, l.preview.y - l.card.y}, theme::Panel);
    u.Fill({l.card.x, l.preview.Bottom(), l.card.w, l.card.Bottom() - l.preview.Bottom()}, theme::Panel);
    u.Fill({l.card.x, l.preview.y, l.preview.x - l.card.x, l.preview.h}, theme::Panel);
    u.Fill({l.preview.Right(), l.preview.y, l.card.Right() - l.preview.Right(), l.preview.h}, theme::Panel);
    if (t.srv) {   // letterbox around the image
        u.Fill({l.preview.x, l.preview.y, l.preview.w, img.y - l.preview.y}, {0, 0, 0, 1});
        u.Fill({l.preview.x, img.Bottom(), l.preview.w, l.preview.Bottom() - img.Bottom()}, {0, 0, 0, 1});
        u.Fill({l.preview.x, img.y, img.x - l.preview.x, img.h}, {0, 0, 0, 1});
        u.Fill({img.Right(), img.y, l.preview.Right() - img.Right(), img.h}, {0, 0, 0, 1});
    } else {
        u.Fill(l.preview, {0, 0, 0, 1});
        u.Text(L"Loading preview…", l.preview, u.S(13), theme::OnBlackDim, Align::Center);   // on the black preview
    }
    const float cx = l.card.x + u.S(20), cw = l.card.w - u.S(40);
    const std::wstring name = t.path.substr(t.path.find_last_of(L"\\/") + 1);
    u.Text(L"Trim: " + name, {cx, l.card.y + u.S(14), cw, u.S(28)}, u.S(16), theme::Text, Align::Left, true);

    float y = l.preview.Bottom() + u.S(14);
    const float rowH = u.S(34), lw = u.S(130);
    u.Text(L"Start  " + Clock(t.start), {cx, y, lw, rowH}, u.S(13), theme::Text);
    if (u.Slider("trim_start", {cx + lw, y, cw - lw, rowH}, t.start, 0.0f, float(t.duration))) {
        t.start = std::min(t.start, std::max(0.0f, t.end - 0.5f));
        t.wantPreview = t.start;
        t.changedAt = QpcNow();
    }
    y += rowH + u.S(8);
    u.Text(L"End    " + Clock(t.end), {cx, y, lw, rowH}, u.S(13), theme::Text);
    if (u.Slider("trim_end", {cx + lw, y, cw - lw, rowH}, t.end, 0.0f, float(t.duration))) {
        t.end = std::max(t.end, std::min(float(t.duration), t.start + 0.5f));
        t.wantPreview = t.end;
        t.changedAt = QpcNow();
    }
    y += rowH + u.S(8);
    u.Text(Format(L"Length %s  •  starts on the nearest keyframe before Start (no quality loss)", Clock(t.end - t.start).c_str()),
           {cx, y, cw, u.S(20)}, u.S(11.5f), theme::TextDim);
    y += u.S(30);
    const float bw = u.S(190);
    if (u.Button("trim_save", {l.card.Right() - u.S(20) - bw, y, bw, rowH}, t.saving ? L"Saving…" : L"Save trimmed copy", glyph::Cut, true) &&
        !t.saving) {
        t.saving = true;
        std::wstring out = t.path;
        const size_t dot = out.find_last_of(L'.');
        out = (dot == std::wstring::npos ? out : out.substr(0, dot)) + L"_trim.mp4";
        for (int n = 2; GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES; ++n)
            out = (dot == std::wstring::npos ? t.path : t.path.substr(0, dot)) + Format(L"_trim%d.mp4", n);
        const std::wstring in = t.path;
        const double a = t.start, b = t.end;
        m_graveyard.push_back(std::async(std::launch::async, [this, in, out, a, b] {
            std::wstring err;
            double actual = a;
            const bool ok = TrimFile(in, out, a, b, err, &actual);
            if (ok) ToastFile(Format(L"Trimmed copy saved (starts at %s): ", Clock(actual).c_str()) + out, out);
            else Toast(L"Trim failed: " + err);
        }));
        m_trim.reset();
        return;
    }
    if (u.Button("trim_cancel", {l.card.Right() - u.S(28) - bw - u.S(110), y, u.S(110), rowH}, L"Cancel")) m_trim.reset();
}

} // namespace cc
