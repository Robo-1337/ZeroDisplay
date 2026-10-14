// Advanced mode: scenes, sources, canvas editing, program output.
#include "App.h"
#include "core/Log.h"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>

namespace cc {

static const wchar_t* kCanvasLink = L"#canvas";

static float PanelWidth(const Ui& u) { return std::min(u.S(360), u.Width() * 0.4f); }

SceneItem* App::SelectedItem()
{
    Scene* s = EditScene();
    return s ? m_scenes.Find(*s, m_selectedItem) : nullptr;
}

Session* App::SessionForItem(const SceneItem& item)
{
    if (item.kind != SourceKind::Device) return nullptr;
    for (auto& s : m_sessions)
        if (s->info.symlink == item.link) return s.get();
    for (auto& s : m_sessions)
        if (!item.deviceName.empty() && s->info.Key() == item.deviceName) return s.get();
    return nullptr;
}

Compositor::DeviceLookup App::Devices()
{
    return [this](const SceneItem& it, Compositor::DeviceFrame& f) {
        Session* s = SessionForItem(it);
        if (s && s->standby) {
            f.srv = s->surface.lastGoodSrv.Get();
            f.width = s->surface.lastGoodW;
            f.height = s->surface.lastGoodH;
            return true;
        }
        if (!s || !s->surface.hasImage) return false;
        f.srv = s->surface.rgbSrv.Get();
        f.width = s->surface.width;
        f.height = s->surface.height;
        return true;
    };
}

RectF App::CanvasArea() const
{
    const float W = m_ui.Width(), H = m_ui.Height(), barH = BarHeight();
    return {0, barH, std::max(1.0f, W - PanelWidth(m_ui)), std::max(1.0f, H - barH)};
}

RectF App::CanvasRect() const
{
    if (!m_studio) return Renderer::FitRect(m_scenes.width, m_scenes.height, CanvasArea().Inset(m_ui.S(12)), ScaleMode::Fit);
    const RectF a = CanvasArea();
    const RectF right{a.x + a.w / 2, a.y + m_ui.S(36), a.w / 2, a.h - m_ui.S(36) - m_ui.S(70)};
    return Renderer::FitRect(m_scenes.width, m_scenes.height, right.Inset(m_ui.S(10)), ScaleMode::Fit);
}

RectF App::ProgramRect() const { return CanvasRect(); }

RectF App::EditRect() const
{
    if (!m_studio) return CanvasRect();
    const RectF a = CanvasArea();
    const RectF left{a.x, a.y + m_ui.S(36), a.w / 2, a.h - m_ui.S(36) - m_ui.S(70)};
    return Renderer::FitRect(m_scenes.width, m_scenes.height, left.Inset(m_ui.S(10)), ScaleMode::Fit);
}

void App::StartTransition(int toScene)
{
    if (toScene < 0 || toScene >= int(m_scenes.scenes.size()) || toScene == m_scenes.program) return;
    m_transitionFrom = m_scenes.program;
    m_scenes.program = toScene;
    m_transitioning = m_transitionMode != 0 && m_transitionMs > 0;
    m_transitionStart = QpcNow();
    ScenesChanged();
    SyncSceneSessions();
}

void App::EnsureDefaultScene()
{
    if (!m_scenes.scenes.empty()) return;
    m_scenes.scenes.push_back(Scene{L"Scene 1", {}});
    m_scenes.program = m_scenes.preview = 0;
    if (Session* a = Active()) {
        SceneItem& it = m_scenes.Add(m_scenes.scenes[0], SourceKind::Device, a->info.name);
        it.link = a->info.symlink;
        it.deviceName = a->info.Key();
        it.rect = m_scenes.FitToCanvas(float(a->surface.width ? a->surface.width : 1920), float(a->surface.height ? a->surface.height : 1080));
    }
    ScenesChanged();
}

void App::BuildDemoScene()
{
    Scene sc{L"Demo", {}};
    SceneItem& bg = m_scenes.Add(sc, SourceKind::Color, L"Background");
    bg.color = m_cmd.demoBlack ? 0xFF000000 : 0xFF1E2A44;
    if (Session* a = Active()) {
        SceneItem& d = m_scenes.Add(sc, SourceKind::Device, a->info.name);
        d.link = a->info.symlink;
        d.deviceName = a->info.Key();
        d.rect = {80, 80, 1120, 630};
    }
    SceneItem& img = m_scenes.Add(sc, SourceKind::Image, L"Wallpaper");
    img.path = L"C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
    img.rect = {1240, 80, 600, 338};
    SceneItem& key = m_scenes.Add(sc, SourceKind::Color, L"Keyed green (should be invisible)");
    key.color = 0xFF00FF00;
    key.rect = {1240, 460, 600, 120};
    key.filters.key.enabled = true;
    SceneItem& mag = m_scenes.Add(sc, SourceKind::Color, L"Magenta box");
    mag.color = 0xC0FF00FF;
    mag.rect = {1240, 600, 600, 110};
    SceneItem& txt = m_scenes.Add(sc, SourceKind::Text, L"Title");
    txt.text = L"ZeroDisplay \u2022 Advanced mode";
    if (!m_cmd.demoFont.empty()) {
        txt.fontFamily = m_cmd.demoFont;
        txt.fontWeight = 700;
        txt.italic = true;
    }
    txt.fontSize = 64;
    txt.rect = {80, 760, 0, 90};
    m_scenes.scenes.push_back(std::move(sc));
    m_scenes.program = int(m_scenes.scenes.size()) - 1;
    SyncSceneSessions();
}

void App::SetAdvanced(bool on)
{
    if (on == m_advanced) return;
    if (on) m_wirelessView = false;
    StopFlashback();
    StopRecording(L"Recording ended: Simple and Advanced record different pictures");
    DestroyEncoder();   // also detaches the replay buffer (it used to keep a pointer to the freed encoder)
    DestroyShareEncoder();   // sharing switches between the device and the program
    m_advanced = on;
    if (!m_cmd.advanced) m_settingsDirty = true;
    if (on) {
        if (m_multiview) SetMultiview(false);
        if (Session* a = Active()) m_simpleDevice = a->info;
        m_mixer.Start();
        EnsureDefaultScene();
        SyncSceneSessions();
    } else {
        m_mixer.Stop();
        // Back to the device Simple mode showed before (or the usual pick), not whatever the scene used.
        int want = m_simpleDevice.symlink.empty() ? -1 : DeviceIndex(m_simpleDevice);
        if (want < 0 && !m_devices.video.empty()) want = int(PickInitialDevice());
        if (want >= 0) {
            SwitchTo(size_t(want));   // single view: closes every other session
        } else {
            for (size_t i = m_sessions.size(); i-- > 1;) CloseSession(i);
            m_active = 0;
        }
    }
}

void App::SyncSceneSessions()
{
    if (!m_advanced) return;
    std::vector<Scene*> scenes;
    for (int idx : {m_scenes.program, m_studio ? m_scenes.preview : -1, m_transitioning ? m_transitionFrom : -1})
        if (Scene* s = m_scenes.Get(idx)) scenes.push_back(s);
    for (Scene* sc : scenes)
        for (const auto& it : sc->items) {
            if (it.kind != SourceKind::Device || SessionForItem(it)) continue;
            for (const auto& d : m_devices.video)
                if (d.symlink == it.link || (!it.deviceName.empty() && d.Key() == it.deviceName)) {
                    OpenSession(d);
                    break;
                }
        }
    for (size_t i = m_sessions.size(); i-- > 0;) {
        bool used = false;
        for (Scene* sc : scenes)
            for (const auto& it : sc->items)
                if (SessionForItem(it) == m_sessions[i].get()) used = true;
        if (!used && !KeepOpen(*m_sessions[i])) CloseSession(i);
    }
    m_active = 0;
}

bool App::EnsureCanvasEncoder()
{
    if (m_encoder && m_encoder->Alive() && m_encoderSymlink == kCanvasLink && (m_recorder || EncoderUpToDate())) return true;
    if (m_recorder && m_encoder) return false;   // never swap the encoder under a running recording
    EncoderConfig cfg;
    cfg.width = m_scenes.width;
    cfg.height = m_scenes.height;
    cfg.fpsNum = m_scenes.fpsNum;
    cfg.fpsDen = m_scenes.fpsDen;
    cfg.bitrateKbps = BitrateKbps(m_quality);
    cfg.gopFrames = 2 * m_scenes.fpsNum / std::max(1u, m_scenes.fpsDen);
    return StartEncoder(cfg, kCanvasLink);
}

const std::vector<std::wstring>& App::FontFamilies()
{
    if (!m_fontFamilies.empty()) return m_fontFamilies;
    ComPtr<IDWriteFontCollection> fonts;
    if (m_renderer.DWrite() && SUCCEEDED(m_renderer.DWrite()->GetSystemFontCollection(&fonts))) {
        for (UINT32 i = 0; i < fonts->GetFontFamilyCount(); ++i) {
            ComPtr<IDWriteFontFamily> family;
            ComPtr<IDWriteLocalizedStrings> names;
            if (FAILED(fonts->GetFontFamily(i, &family)) || FAILED(family->GetFamilyNames(&names))) continue;
            UINT32 index = 0;
            BOOL exists = FALSE;
            names->FindLocaleName(L"en-us", &index, &exists);
            if (!exists) index = 0;
            UINT32 len = 0;
            if (FAILED(names->GetStringLength(index, &len))) continue;
            std::wstring name(len + 1, L'\0');
            if (FAILED(names->GetString(index, name.data(), len + 1))) continue;
            name.resize(len);
            if (!name.empty() && name[0] != L'@') m_fontFamilies.push_back(name);   // '@' = vertical CJK variants
        }
    }
    std::sort(m_fontFamilies.begin(), m_fontFamilies.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    m_fontFamilies.erase(std::unique(m_fontFamilies.begin(), m_fontFamilies.end()), m_fontFamilies.end());
    if (m_fontFamilies.empty()) m_fontFamilies.push_back(L"Segoe UI");
    return m_fontFamilies;
}

// ---------------------------------------------------------------------------------------------
// Undo / redo. Edits mark the scenes as changed; once the mouse is up and nothing changed for a moment
// the previous state becomes one undo step (a whole drag or slider move = one step). Switching the scene
// on air is not an edit: snapshots compare the scenes' content only.

static bool SameContent(const SceneCollection& a, const SceneCollection& b)
{
    return a.width == b.width && a.height == b.height && a.fpsNum == b.fpsNum && a.fpsDen == b.fpsDen && a.scenes == b.scenes;
}

void App::UndoTick(const UiInput& in)
{
    if (!m_undoInit) {
        m_undoBase = m_scenes;
        m_undoInit = true;
        m_undoPending = false;
    }
    if (m_undoPending && !in.down && QpcToMs(QpcNow() - m_lastSceneEdit) > 400.0) CommitUndo();
}

void App::CommitUndo()
{
    if (!m_undoPending) return;
    m_undoPending = false;
    if (SameContent(m_undoBase, m_scenes)) return;
    m_undo.push_back(std::move(m_undoBase));
    if (m_undo.size() > 50) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_undoBase = m_scenes;
}

void App::RestoreScenes(SceneCollection s)
{
    const int program = m_scenes.program, preview = m_scenes.preview;   // what is on air stays on air
    m_scenes = std::move(s);
    const int n = int(m_scenes.scenes.size());
    m_scenes.program = program < n ? program : std::max(0, n - 1);
    m_scenes.preview = preview < n ? preview : std::max(0, n - 1);
    m_undoBase = m_scenes;
    m_undoPending = false;
    m_scenesDirty = true;
    m_settingsDirty = true;
    if (Scene* sc = EditScene())
        if (!m_scenes.Find(*sc, m_selectedItem)) m_selectedItem = 0;
    SyncSceneSessions();
}

// Self-test: three edits, undo all, compare with the original; redo all, compare with the edited state.
void App::UndoSelftest(double t)
{
    static int step = 0;
    static SceneCollection original, edited;
    Scene* sc = EditScene();
    if (!sc || step < 0) return;
    auto commit = [&] {
        m_lastSceneEdit = 0;   // the "gesture" is over
        CommitUndo();
    };
    if (step == 0 && t >= 2.0) {
        CommitUndo();
        original = m_scenes;
        AddSource(SourceKind::Text, nullptr);   // edit 1: add
        commit();
        if (!sc->items.empty()) {               // edit 2: move
            sc->items.front().rect.x += 100;
            ScenesChanged();
            commit();
        }
        if (sc->items.size() > 1) {             // edit 3: delete
            sc->items.erase(sc->items.begin() + 1);
            ScenesChanged();
            commit();
        }
        edited = m_scenes;
        step = 1;
    } else if (step == 1 && t >= 3.0) {
        CommitUndo();
        edited = m_scenes;   // as rendered since (text boxes size themselves to their text)
        for (int i = 0; i < 3; ++i) Undo();
        LOGI(L"[selftest] undo x3: %s (undo %zu, redo %zu)", SameContent(m_scenes, original) ? L"PASS, scenes equal the original" : L"FAIL",
             m_undo.size(), m_redo.size());
        for (int i = 0; i < 3; ++i) Redo();
        LOGI(L"[selftest] redo x3: %s", SameContent(m_scenes, edited) ? L"PASS, scenes equal the edited state" : L"FAIL");
        step = -1;
    }
}

void App::StudioSelftest(double t)
{
    // 8 transitions (Cut / Fade / Slide / Wipe at 300 and 1000 ms) between the two scenes, each timed; then the
    // number keys pick the preview, and an edit in the preview scene must leave the program scene untouched.
    static int step = 0;
    static int64_t started = 0;
    static double nextAt = 2.0;
    static int target = -1;
    static const int modes[] = {0, 1, 2, 3, 0, 1, 2, 3};
    static const float durations[] = {300, 300, 300, 300, 1000, 1000, 1000, 1000};
    static const wchar_t* names[] = {L"Cut", L"Fade", L"Slide", L"Wipe"};
    if (step < 0 || t < nextAt || !m_studio) return;
    if (m_scenes.scenes.size() < 2) {
        m_scenes.scenes.push_back(Scene{L"Scene B", {}});
        AddSource(SourceKind::Color, nullptr);
        ScenesChanged();
    }
    const int other = m_scenes.program == 0 ? 1 : 0;
    if (started) {   // the transition started last: wait for its end, then check it
        if (m_transitioning) return;
        const double ms = QpcElapsedMs(started);
        const bool ok = m_scenes.program == target && (modes[step - 1] == 0 ? ms < 100 : ms >= durations[step - 1] - 20 && ms < durations[step - 1] + 150);
        LOGI(L"[selftest] studio %s %.0f ms: program = scene %d, took %.0f ms: %s", names[modes[step - 1]], double(durations[step - 1]),
             m_scenes.program, ms, ok ? L"PASS" : L"FAIL");
        started = 0;
        nextAt = t + 0.5;
        return;
    }
    if (step < 8) {
        m_transitionMode = modes[step];
        m_transitionMs = durations[step];
        m_scenes.preview = other;
        target = other;
        started = QpcNow();
        StartTransition(other);
        ++step;
        return;
    }
    if (step == 8) {
        {
            std::lock_guard lock(m_inputMutex);
            m_input.keys.push_back('1');   // studio: number keys pick the preview, the program stays
        }
        target = m_scenes.program;
        step = 9;
        nextAt = t + 0.5;
        return;
    }
    if (step == 9) {
        LOGI(L"[selftest] studio key 1: preview = scene %d, program = scene %d (unchanged): %s", m_scenes.preview, m_scenes.program,
             m_scenes.preview == 0 && m_scenes.program == target ? L"PASS" : L"FAIL");
        // Edit the preview scene (when it differs from the program): the program must not change.
        m_scenes.preview = m_scenes.program == 0 ? 1 : 0;
        const Scene programBefore = m_scenes.scenes[size_t(m_scenes.program)];
        Scene* pv = m_scenes.Get(m_scenes.preview);
        if (pv && !pv->items.empty()) {
            pv->items.front().rect.x += 50;
            ScenesChanged();
        }
        LOGI(L"[selftest] studio edit in preview: program scene %s", m_scenes.scenes[size_t(m_scenes.program)] == programBefore ? L"unchanged: PASS" : L"changed: FAIL");
        step = -1;
    }
}

void App::DeleteSelectedItem()
{
    Scene* sc = EditScene();
    if (!sc) return;
    for (size_t k = 0; k < sc->items.size(); ++k)
        if (sc->items[k].id == m_selectedItem) {
            LOGI(L"Advanced: removed %s", sc->items[k].name.c_str());
            m_compositor.Forget(m_selectedItem);
            sc->items.erase(sc->items.begin() + ptrdiff_t(k));
            m_selectedItem = 0;
            ScenesChanged();
            SyncSceneSessions();
            return;
        }
}

void App::DuplicateSelectedItem()
{
    Scene* sc = EditScene();
    SceneItem* it = SelectedItem();
    if (!sc || !it) return;
    SceneItem copy = *it;
    copy.id = m_scenes.nextId++;
    copy.name = it->name + L" (copy)";
    copy.rect.x += 24;
    copy.rect.y += 24;
    sc->items.push_back(std::move(copy));
    m_selectedItem = sc->items.back().id;
    LOGI(L"Advanced: duplicated as %s (%zu sources)", sc->items.back().name.c_str(), sc->items.size());
    ScenesChanged();
    SyncSceneSessions();
}

void App::NudgeSelectedItem(float dx, float dy)
{
    if (SceneItem* it = SelectedItem()) {
        it->rect.x += dx;
        it->rect.y += dy;
        LOGI(L"Advanced: %s moved to %.0f, %.0f", it->name.c_str(), it->rect.x, it->rect.y);
        ScenesChanged();
    }
}

void App::DuplicateEditScene()
{
    int& edit = m_studio ? m_scenes.preview : m_scenes.program;
    Scene* src = m_scenes.Get(edit);
    if (!src) return;
    Scene copy = *src;
    copy.name = src->name + L" (copy)";
    for (auto& it : copy.items) it.id = m_scenes.nextId++;   // items are their own (images, text are drawn per id)
    m_scenes.scenes.push_back(std::move(copy));
    if (m_studio) m_scenes.preview = int(m_scenes.scenes.size()) - 1;   // on air stays as it is
    m_selectedItem = 0;
    ScenesChanged();
    SyncSceneSessions();
    Toast(L"Scene copied: " + m_scenes.scenes.back().name);
}

void App::SetCanvas(UINT w, UINT h, UINT fps)
{
    if (m_recorder) return;
    const UINT oldW = m_scenes.width, oldH = m_scenes.height;
    if (w == oldW && h == oldH && fps == m_scenes.fpsNum / std::max(1u, m_scenes.fpsDen)) return;
    // Sources keep their place and size relative to the canvas.
    const float sx = float(w) / float(std::max(1u, oldW)), sy = float(h) / float(std::max(1u, oldH));
    for (auto& sc : m_scenes.scenes)
        for (auto& it : sc.items) {
            it.rect = {it.rect.x * sx, it.rect.y * sy, it.rect.w * sx, it.rect.h * sy};
            if (it.kind == SourceKind::Text) it.fontSize *= sy;
        }
    m_scenes.width = w;
    m_scenes.height = h;
    m_scenes.fpsNum = fps;
    m_scenes.fpsDen = 1;
    m_program = RenderTarget{};
    m_preview = RenderTarget{};
    ScenesChanged();
    LOGI(L"Canvas: %ux%u at %u fps", w, h, fps);
}

void App::Undo()
{
    CommitUndo();
    if (m_undo.empty() || m_transitioning) return;
    m_redo.push_back(m_scenes);
    SceneCollection s = std::move(m_undo.back());
    m_undo.pop_back();
    RestoreScenes(std::move(s));
    Toast(L"Undo");
}

void App::Redo()
{
    CommitUndo();
    if (m_redo.empty() || m_transitioning) return;
    m_undo.push_back(m_scenes);
    SceneCollection s = std::move(m_redo.back());
    m_redo.pop_back();
    RestoreScenes(std::move(s));
    Toast(L"Redo");
}

std::shared_ptr<AudioSource> App::ProgramAudioSource()
{
    if (m_mixer.Running()) return m_mixer.Output();
    // Until the program audio mixer exists: the first audible device in the program scene.
    if (Scene* sc = ProgramScene())
        for (const auto& it : sc->items)
            if (it.kind == SourceKind::Device && it.visible && !it.muted)
                if (Session* s = SessionForItem(it))
                    if (s->audioCap || s->video->OwnAudio()) return s->audioSrc;
    return nullptr;
}

void App::RenderProgram(int64_t now)
{
    Scene* sc = ProgramScene();
    const UINT cw = m_scenes.width, ch = m_scenes.height;
    if (!sc || !m_renderer.EnsureTarget(m_program, cw, ch)) return;
    // Text keeps its aspect ratio: width follows the rendered text.
    for (auto& scene : m_scenes.scenes)
        for (auto& it : scene.items) {
            if (it.kind != SourceKind::Text) continue;
            UINT w = 0, h = 0;
            if (m_compositor.ContentSize(it, Devices(), w, h) && h) it.rect.w = it.rect.h * float(w) / float(h);
        }
    Scene* from = m_scenes.Get(m_transitionFrom);
    const float t = m_transitioning ? float(QpcElapsedMs(m_transitionStart) / std::max(1.0f, m_transitionMs)) : 1.0f;
    if (m_transitioning && from && t < 1.0f && m_renderer.EnsureTarget(m_transA, cw, ch) && m_renderer.EnsureTarget(m_transB, cw, ch)) {
        m_compositor.Render(m_transA, *from, Devices());
        m_compositor.Render(m_transB, *sc, Devices());
        const float eased = t * t * (3.0f - 2.0f * t);
        m_renderer.DrawTransition(m_program, m_transA, m_transB, eased, m_transitionMode == 2 ? 1 : (m_transitionMode == 3 ? 2 : 0));
    } else {
        if (m_transitioning) {
            m_transitioning = false;
            SyncSceneSessions();   // the outgoing scene's devices can close now
        }
        m_compositor.Render(m_program, *sc, Devices());
    }
    // Battery saver: the studio preview (not on air) is drawn at half rate.
    if (m_studio && (!(m_profile.battery || m_profile.low) || (++m_studioTick & 1) == 0))
        if (Scene* pv = m_scenes.Get(m_scenes.preview))
            if (m_renderer.EnsureTarget(m_preview, cw, ch)) m_compositor.Render(m_preview, *pv, Devices());
    if (EncodesLink(kCanvasLink)) {
        // Paced by an accumulator (exact average rate); the main loop wakes up for each deadline.
        const int64_t period = QpcFrequency() * m_scenes.fpsDen / std::max(1u, m_scenes.fpsNum);
        // Behind by more than half a frame (a busy GPU): drop frames rather than catch up with a burst, burst frames
        // got nearly the same time stamp (equal DTS in the file). Submissions stay at least half a period apart.
        if (now - m_lastCanvasSubmit > period + period / 2) m_lastCanvasSubmit = now - period;
        if (now - m_lastCanvasSubmit >= period - QpcFrequency() / 1000) {
            m_lastCanvasSubmit += period;
            SubmitFrame(kCanvasLink, m_program.tex.Get(), QpcTo100ns(now), now);
        }
    }
}

void App::AddSource(SourceKind kind, const VideoDeviceInfo* device)
{
    Scene* sc = EditScene();
    if (!sc) return;
    const std::wstring name = device ? device->name : std::wstring(SourceKindName(kind));
    SceneItem& it = m_scenes.Add(*sc, kind, name);
    switch (kind) {
    case SourceKind::Device:
        if (device) {
            it.link = device->symlink;
            it.deviceName = device->Key();
        }
        it.rect = m_scenes.FitToCanvas(1920, 1080);
        break;
    case SourceKind::Text:
        it.text = L"Your text";
        it.rect = {100, 100, 0, 100};
        break;
    case SourceKind::Image:
        it.rect = {float(m_scenes.width) / 4, float(m_scenes.height) / 4, float(m_scenes.width) / 2, float(m_scenes.height) / 2};
        break;
    case SourceKind::Color: break;
    }
    m_selectedItem = it.id;
    const uint32_t id = it.id;
    ScenesChanged();
    SyncSceneSessions();
    if (kind == SourceKind::Image) PickImage(id);
}

void App::PickImage(uint32_t itemId)
{
    m_graveyard.push_back(std::async(std::launch::async, [this, itemId] {
        ComInit com(COINIT_APARTMENTTHREADED);
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        COMDLG_FILTERSPEC spec[2] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, spec);
        dlg->SetTitle(L"Choose an image");
        if (FAILED(dlg->Show(m_hwnd))) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
        {
            std::lock_guard lock(m_pickMutex);
            m_picked.push_back({itemId, path});
        }
        CoTaskMemFree(path);
        SetEvent(m_wake);
    }));
}

void App::ApplyPickedFiles()
{
    std::vector<Picked> picked;
    {
        std::lock_guard lock(m_pickMutex);
        picked.swap(m_picked);
    }
    for (const auto& p : picked)
        for (auto& sc : m_scenes.scenes)
            if (SceneItem* it = m_scenes.Find(sc, p.item)) {
                it->path = p.path;
                UINT w = 0, h = 0;
                if (m_compositor.ContentSize(*it, Devices(), w, h)) {
                    RectF r = m_scenes.FitToCanvas(float(w), float(h));
                    it->rect = {r.x + r.w * 0.2f, r.y + r.h * 0.2f, r.w * 0.6f, r.h * 0.6f};
                }
                ScenesChanged();
            }
}

// ---------------------------------------------------------------------------------------------
// UI

void App::HandleCanvasMouse(const UiInput& in)
{
    Ui& u = m_ui;
    const RectF cr = EditRect();
    const float s = cr.w / float(m_scenes.width);
    if (s <= 0) return;
    if (m_drag != Drag::None) {
        SceneItem* it = SelectedItem();
        if (!in.down || !it) {
            m_drag = Drag::None;
            ScenesChanged();
            return;
        }
        const float dx = (in.mx - m_dragStartX) / s, dy = (in.my - m_dragStartY) / s;
        const float snap = 10.0f / s, CW = float(m_scenes.width), CH = float(m_scenes.height);
        RectF r = m_dragStartRect;
        if (m_drag == Drag::Move) {
            r.x += dx;
            r.y += dy;
            auto snapAxis = [&](float& p, float size, float total) {
                if (std::fabs(p) < snap) p = 0;
                else if (std::fabs(p + size - total) < snap) p = total - size;
                else if (std::fabs(p + size / 2 - total / 2) < snap) p = total / 2 - size / 2;
            };
            snapAxis(r.x, r.w, CW);
            snapAxis(r.y, r.h, CH);
        } else {
            // Opposite corner stays put; aspect ratio kept unless Shift is held.
            const bool free = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const float ax = (m_dragCorner == 0 || m_dragCorner == 3) ? r.Right() : r.x;
            const float ay = (m_dragCorner == 0 || m_dragCorner == 1) ? r.Bottom() : r.y;
            const float cx0 = (m_dragCorner == 0 || m_dragCorner == 3) ? r.x : r.Right();
            const float cy0 = (m_dragCorner == 0 || m_dragCorner == 1) ? r.y : r.Bottom();
            float w = std::max(16.0f, std::fabs(cx0 + dx - ax)), h = std::max(16.0f, std::fabs(cy0 + dy - ay));
            if (!free && m_dragStartRect.h > 0) h = w * m_dragStartRect.h / m_dragStartRect.w;
            r.w = w;
            r.h = h;
            r.x = (m_dragCorner == 0 || m_dragCorner == 3) ? ax - w : ax;
            r.y = (m_dragCorner == 0 || m_dragCorner == 1) ? ay - h : ay;
        }
        it->rect = r;
        return;
    }
    if (!in.pressed || m_settingsOpen || !u.Hot(cr)) return;
    auto toScreen = [&](const RectF& r) { return RectF{cr.x + r.x * s, cr.y + r.y * s, r.w * s, r.h * s}; };
    if (SceneItem* it = SelectedItem()) {
        RectF r = toScreen(it->rect);
        const float px[4] = {r.x, r.Right(), r.Right(), r.x}, py[4] = {r.y, r.y, r.Bottom(), r.Bottom()};
        for (int c = 0; c < 4; ++c)
            if (std::fabs(in.mx - px[c]) < u.S(10) && std::fabs(in.my - py[c]) < u.S(10)) {
                m_drag = Drag::Resize;
                m_dragCorner = c;
                m_dragStartX = in.mx;
                m_dragStartY = in.my;
                m_dragStartRect = it->rect;
                return;
            }
    }
    m_selectedItem = 0;
    if (Scene* sc = EditScene())
        for (auto i = sc->items.rbegin(); i != sc->items.rend(); ++i)
            if (i->visible && toScreen(i->rect).Contains(in.mx, in.my)) {
                m_selectedItem = i->id;
                m_drag = Drag::Move;
                m_dragStartX = in.mx;
                m_dragStartY = in.my;
                m_dragStartRect = i->rect;
                break;
            }
}

void App::DrawAdvanced(const UiInput& in)
{
    UndoTick(in);
    Ui& u = m_ui;
    const RectF cr = EditRect();
    u.Stroke(cr.Inset(-1), theme::Border, 1);
    if (m_studio) {
        const RectF pr = ProgramRect();
        u.Stroke(pr.Inset(-1), theme::Bad.A(0.8f), u.S(2));
        u.Text(L"PREVIEW", {cr.x, cr.y - u.S(30), cr.w, u.S(26)}, u.S(12), theme::OnBlackAccent, Align::Left, true);
        u.Text(L"PROGRAM", {pr.x, pr.y - u.S(30), pr.w, u.S(26)}, u.S(12), theme::OnBlackBad, Align::Left, true);
        if (Scene* pv = m_scenes.Get(m_scenes.preview)) u.Text(pv->name, {cr.x, cr.y - u.S(30), cr.w, u.S(26)}, u.S(12), theme::OnBlackDim, Align::Right);
        if (Scene* pg = ProgramScene()) u.Text(pg->name, {pr.x, pr.y - u.S(30), pr.w, u.S(26)}, u.S(12), theme::OnBlackDim, Align::Right);
        const RectF area = CanvasArea();
        const float bw = u.S(200), bh = u.S(40);
        RectF go{area.x + (area.w - bw) / 2, std::max(cr.Bottom(), pr.Bottom()) + u.S(16), bw, bh};
        if (u.Button("transition", go, m_transitionMode == 0 ? L"Cut to program" : L"Transition", glyph::Share, true))
            StartTransition(m_scenes.preview);
    }
    const float s = cr.w / float(m_scenes.width);
    Scene* sc = EditScene();
    if (sc && sc->items.empty())
        u.Text(L"This scene is empty, add a source in the panel on the right.", cr, u.S(14), theme::TextDim, Align::Center);
    if (SceneItem* it = SelectedItem()) {
        RectF r{cr.x + it->rect.x * s, cr.y + it->rect.y * s, it->rect.w * s, it->rect.h * s};
        u.Stroke(r, theme::Accent2, u.S(2));
        const float hs = u.S(8);
        for (auto [hx, hy] : {std::pair{r.x, r.y}, {r.Right(), r.y}, {r.Right(), r.Bottom()}, {r.x, r.Bottom()}})
            u.Fill({hx - hs / 2, hy - hs / 2, hs, hs}, theme::Accent2);
    }
    HandleCanvasMouse(in);
    DrawScenePanel();
}

void App::DrawScenePanel()
{
    Ui& u = m_ui;
    const float W = u.Width(), H = u.Height(), barH = BarHeight(), pw = PanelWidth(u);
    RectF panel{W - pw, barH, pw, H - barH};
    u.Fill(panel, theme::Panel);
    u.Fill({panel.x, panel.y, 1, panel.h}, theme::Border);
    u.PushClip(panel);
    const float cx = panel.x + u.S(14), cw = pw - u.S(28), rowH = u.S(30);
    float y = panel.y + u.S(12) - m_panelScroll;

    {
        const float bw = (cw - u.S(8)) / 2;
        if (u.Button("undo", {cx, y, bw, rowH}, m_undo.empty() && !m_undoPending ? L"Undo" : L"Undo (Ctrl+Z)")) Undo();
        if (u.Button("redo", {cx + bw + u.S(8), y, bw, rowH}, m_redo.empty() ? L"Redo" : L"Redo (Ctrl+Y)")) Redo();
        y += rowH + u.S(8);
        bool studio = m_studio;
        const wchar_t* studioFull = L"Studio mode (preview + program)";   // the short one when the panel is narrow
        const wchar_t* studioLabel = u.Measure(studioFull, u.S(13)) <= cw - u.S(64) ? studioFull : L"Studio mode";
        if (u.Toggle("studio", {cx, y, cw, rowH}, studioLabel, studio)) {
            if (studio && m_profile.low) {
                Toast(L"Studio mode is off in Low-resource mode");
                studio = false;
            }
            m_studio = studio;
            m_scenes.preview = m_scenes.program;
            m_settingsDirty = true;
            SyncSceneSessions();
        }
        y += rowH + u.S(4);
        if (u.Segmented("transmode", {cx, y, cw, rowH}, {L"Cut", L"Fade", L"Slide", L"Wipe"}, m_transitionMode)) m_settingsDirty = true;
        y += rowH + u.S(2);
        if (m_transitionMode != 0) {
            u.Text(L"Duration", {cx, y, cw * 0.4f, rowH}, u.S(12.5f), theme::Text);
            u.Text(Format(L"%.0f ms", m_transitionMs), {cx, y, cw, rowH}, u.S(12), theme::TextDim, Align::Right);
            if (u.Slider("transms", {cx + cw * 0.4f, y, cw * 0.4f, rowH}, m_transitionMs, 100.0f, 2000.0f)) m_settingsDirty = true;
            y += rowH;
        }
        y += u.S(12);
    }

    if (PanelSection("a.canvas", L"Canvas", y, cx, cw)) {
        // What recordings and sharing of Advanced mode get. Not while recording: the file keeps its size.
        static const UINT sizes[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        int sizeSel = -1;
        for (int i = 0; i < 4; ++i)
            if (sizes[i][0] == m_scenes.width && sizes[i][1] == m_scenes.height) sizeSel = i;
        const bool locked = m_recorder != nullptr;
        if (u.Segmented("canvassize", {cx, y, cw, rowH}, {L"720p", L"1080p", L"1440p", L"4K"}, sizeSel, locked ? 0xFu : 0u))
            SetCanvas(sizes[sizeSel][0], sizes[sizeSel][1], m_scenes.fpsNum / std::max(1u, m_scenes.fpsDen));
        y += rowH + u.S(4);
        int fpsSel = m_scenes.fpsNum / std::max(1u, m_scenes.fpsDen) <= 30 ? 0 : 1;
        if (u.Segmented("canvasfps", {cx, y, cw, rowH}, {L"30 fps", L"60 fps"}, fpsSel, locked ? 0x3u : 0u))
            SetCanvas(m_scenes.width, m_scenes.height, fpsSel == 0 ? 30u : 60u);
        y += rowH;
        const std::wstring note = locked ? std::wstring(L"Stop the recording to change the canvas.")
                                         : Format(L"%u×%u, %u fps: what Advanced records and shares.", m_scenes.width, m_scenes.height,
                                                  m_scenes.fpsNum / std::max(1u, m_scenes.fpsDen));
        const float nh = u.MeasureBlock(note, cw, u.S(11.5f));   // wraps in a narrow panel
        u.TextBlock(note, {cx, y + u.S(2), cw, nh + u.S(2)}, u.S(11.5f), locked ? theme::Warn : theme::TextDim);
        y += nh + u.S(10);
    }
    if (PanelSection("a.scenes", L"Scenes", y, cx, cw)) {
        for (size_t i = 0; i < m_scenes.scenes.size(); ++i) {
            RectF row{cx, y, cw, rowH};
            const bool sel = int(i) == (m_studio ? m_scenes.preview : m_scenes.program);
            const bool onAir = int(i) == m_scenes.program;
            if (sel) u.Fill(row, theme::Accent.A(0.35f), u.S(5));
            else if (u.Hot(row)) u.Fill(row, theme::ControlHover, u.S(5));
            if (onAir) u.Fill({row.x + u.S(3), row.y + u.S(8), u.S(4), row.h - u.S(16)}, theme::Bad, u.S(2));
            u.Text(Format(L"%zu  ", i + 1) + m_scenes.scenes[i].name, row.Inset(u.S(12), 0), u.S(13), theme::Text);
            if (u.Hot(row) && u.Input().pressed && !sel) {
                m_selectedItem = 0;
                if (m_studio) {
                    m_scenes.preview = int(i);
                    ScenesChanged();
                    SyncSceneSessions();
                } else {
                    StartTransition(int(i));
                }
            }
            y += rowH + u.S(2);
        }
        if (Scene* sc = EditScene()) {
            y += u.S(4);
            if (u.TextBox("scenename", {cx, y, cw, rowH}, sc->name, L"Scene name")) ScenesChanged();
            y += rowH + u.S(6);
        }
        // One row of five buttons, or (narrow panel) Add / Copy / Delete and Up / Down, so no label is cut.
        const float bw = (cw - u.S(16)) / 5;
        const bool twoRows = bw < std::max(u.Measure(L"Delete", u.S(13)), u.Measure(L"Add", u.S(13)) + u.S(22)) + u.S(14);   // as wide as the labels need
        RectF btn[5];
        for (int i = 0; i < 5; ++i) {
            if (!twoRows) btn[i] = {cx + float(i) * (bw + u.S(4)), y, bw, rowH};
            else if (i < 3) btn[i] = {cx + float(i) * ((cw - u.S(8)) / 3 + u.S(4)), y, (cw - u.S(8)) / 3, rowH};
            else btn[i] = {cx + float(i - 3) * ((cw - u.S(4)) / 2 + u.S(4)), y + rowH + u.S(4), (cw - u.S(4)) / 2, rowH};
        }
        // Scene buttons act on the scene being edited (the preview scene in studio mode).
        int& edit = m_studio ? m_scenes.preview : m_scenes.program;
        auto remap = [&](int& idx, int a, int b) {   // keep program/preview pointing at the same scenes after a swap
            if (idx == a) idx = b;
            else if (idx == b) idx = a;
        };
        if (u.Button("addscene", btn[0], L"Add", glyph::Add)) {
            if (m_scenes.scenes.size() >= size_t(SceneCollection::kMaxScenes)) {
                Toast(Format(L"At most %d scenes", SceneCollection::kMaxScenes));
            } else {
                m_scenes.scenes.push_back(Scene{Format(L"Scene %zu", m_scenes.scenes.size() + 1), {}});
                edit = int(m_scenes.scenes.size()) - 1;
                ScenesChanged();
                SyncSceneSessions();
            }
        }
        if (u.Button("copyscene", btn[1], L"Copy")) DuplicateEditScene();
        if (u.Button("delscene", btn[2], L"Delete") && m_scenes.scenes.size() > 1) {
            const int gone = edit;
            m_scenes.scenes.erase(m_scenes.scenes.begin() + gone);
            for (int* idx : {&m_scenes.program, &m_scenes.preview, &m_transitionFrom})
                if (*idx > gone || *idx >= int(m_scenes.scenes.size())) *idx = std::max(0, *idx - 1);
            m_transitioning = false;
            m_selectedItem = 0;
            ScenesChanged();
            SyncSceneSessions();
        }
        if (u.Button("sceneup", btn[3], L"Up") && edit > 0) {
            const int a = edit, b = edit - 1;
            std::swap(m_scenes.scenes[size_t(a)], m_scenes.scenes[size_t(b)]);
            remap(m_scenes.program, a, b);
            if (m_studio) remap(m_scenes.preview, a, b);
            ScenesChanged();
        }
        if (u.Button("scenedown", btn[4], L"Down") && edit + 1 < int(m_scenes.scenes.size())) {
            const int a = edit, b = edit + 1;
            std::swap(m_scenes.scenes[size_t(a)], m_scenes.scenes[size_t(b)]);
            remap(m_scenes.program, a, b);
            if (m_studio) remap(m_scenes.preview, a, b);
            ScenesChanged();
        }
        y += (twoRows ? 2 * rowH + u.S(4) : rowH) + u.S(16);
    }
    DrawSourcesPanel(y, cx, cw);
    DrawPropertiesPanel(y, cx, cw);
    y += u.S(16);

    u.PopClip();
    const float content = y + m_panelScroll - panel.y;
    if (u.Hot(panel) && u.Input().wheel != 0 && !u.PopupOpen()) m_panelScroll -= u.Input().wheel * u.S(60);
    m_panelScroll = Clamp(m_panelScroll, 0.0f, std::max(0.0f, content - panel.h));
}

void App::DrawSourcesPanel(float& y, float cx, float cw)
{
    Ui& u = m_ui;
    Scene* sc = EditScene();
    if (!sc) return;
    const float rowH = u.S(30);
    if (!PanelSection("a.sources", L"Sources", y, cx, cw)) return;
    for (auto i = sc->items.rbegin(); i != sc->items.rend(); ++i) {
        SceneItem& it = *i;
        RectF row{cx, y, cw, rowH};
        const bool sel = it.id == m_selectedItem;
        if (sel) u.Fill(row, theme::Accent.A(0.35f), u.S(5));
        const std::string eyeId = "eye" + std::to_string(it.id);
        if (u.IconButton(eyeId.c_str(), {row.x, row.y, rowH, rowH}, it.visible ? glyph::Eye : glyph::EyeOff, L"Show / hide")) {
            it.visible = !it.visible;
            ScenesChanged();
        }
        RectF label{row.x + rowH + u.S(4), row.y, row.w - rowH - u.S(4), rowH};
        u.Text(it.name, label, u.S(13), it.visible ? theme::Text : theme::TextDim);
        if (u.Hot(label) && u.Input().pressed) m_selectedItem = it.id;
        y += rowH + u.S(2);
    }
    std::vector<std::wstring> add;
    for (const auto& d : m_devices.video) add.push_back(L"Device: " + d.name);
    add.push_back(L"Image\u2026");
    add.push_back(L"Text");
    add.push_back(L"Color");
    int pick = -1;
    y += u.S(4);
    if (u.Dropdown("addsource", {cx, y, cw, rowH}, add, pick, glyph::Add, L"Add source\u2026")) {
        const size_t nd = m_devices.video.size();
        if (size_t(pick) < nd) AddSource(SourceKind::Device, &m_devices.video[size_t(pick)]);
        else AddSource(SourceKind(1 + (size_t(pick) - nd)), nullptr);
    }
    y += rowH + u.S(6);
    if (SelectedItem()) {
        const float bw = (cw - u.S(8)) / 3;
        auto idx = [&]() -> int {
            for (size_t k = 0; k < sc->items.size(); ++k)
                if (sc->items[k].id == m_selectedItem) return int(k);
            return -1;
        };
        if (u.Button("delsource", {cx, y, bw, rowH}, L"Delete", bw >= u.S(96) ? glyph::Delete : 0)) DeleteSelectedItem();   // no icon when narrow
        if (u.Button("srcup", {cx + bw + u.S(4), y, bw, rowH}, L"Forward")) {
            int k = idx();
            if (k >= 0 && k + 1 < int(sc->items.size())) std::swap(sc->items[size_t(k)], sc->items[size_t(k + 1)]), ScenesChanged();
        }
        if (u.Button("srcdown", {cx + 2 * (bw + u.S(4)), y, bw, rowH}, L"Back")) {
            int k = idx();
            if (k > 0) std::swap(sc->items[size_t(k)], sc->items[size_t(k - 1)]), ScenesChanged();
        }
        y += rowH + u.S(16);
    }
}

void App::DrawPropertiesPanel(float& y, float cx, float cw)
{
    Ui& u = m_ui;
    SceneItem* it = SelectedItem();
    const float rowH = u.S(30);
    if (!PanelSection("a.properties", L"Properties", y, cx, cw)) return;
    if (!it) {
        u.Text(L"Select a source to edit it.", {cx, y, cw, rowH}, u.S(12.5f), theme::TextDim);
        y += rowH;
        return;
    }
    auto slider = [&](const char* id, const wchar_t* label, float& v, float lo, float hi) {
        u.Text(label, {cx, y, cw * 0.4f, rowH}, u.S(12.5f), theme::Text);
        u.Text(Format(L"%.2f", v), {cx, y, cw, rowH}, u.S(12), theme::TextDim, Align::Right);
        if (u.Slider(id, {cx + cw * 0.4f, y, cw * 0.45f, rowH}, v, lo, hi)) ScenesChanged();
        y += rowH;
    };
    if (u.TextBox("itemname", {cx, y, cw, rowH}, it->name, L"Name")) ScenesChanged();
    y += rowH + u.S(4);
    u.Text(Format(L"%.0f, %.0f   %.0f \u00D7 %.0f", it->rect.x, it->rect.y, it->rect.w, it->rect.h), {cx, y, cw, rowH}, u.S(12), theme::TextDim);
    y += rowH;
    const float bw = (cw - u.S(8)) / 3;
    if (u.Button("fit", {cx, y, bw, rowH}, L"Fit")) {
        UINT w = 0, h = 0;
        it->rect = m_compositor.ContentSize(*it, Devices(), w, h) ? m_scenes.FitToCanvas(float(w), float(h))
                                                                  : RectF{0, 0, float(m_scenes.width), float(m_scenes.height)};
        ScenesChanged();
    }
    if (u.Button("fill", {cx + bw + u.S(4), y, bw, rowH}, L"Full")) {
        it->rect = {0, 0, float(m_scenes.width), float(m_scenes.height)};
        ScenesChanged();
    }
    if (u.Button("center", {cx + 2 * (bw + u.S(4)), y, bw, rowH}, L"Center")) {
        it->rect.x = (float(m_scenes.width) - it->rect.w) / 2;
        it->rect.y = (float(m_scenes.height) - it->rect.h) / 2;
        ScenesChanged();
    }
    y += rowH + u.S(6);
    slider("opacity", L"Opacity", it->opacity, 0, 1);
    slider("cropl", L"Crop left", it->cropL, 0, 0.45f);
    slider("cropt", L"Crop top", it->cropT, 0, 0.45f);
    slider("cropr", L"Crop right", it->cropR, 0, 0.45f);
    slider("cropb", L"Crop bottom", it->cropB, 0, 0.45f);
    if (it->kind == SourceKind::Device) {
        slider("volume", L"Volume", it->volume, 0, 1);
        if (u.Toggle("mutesrc", {cx, y, cw, rowH}, L"Mute", it->muted)) ScenesChanged();
        y += rowH;
        if (u.Toggle("gate", {cx, y, cw, rowH}, L"Noise gate", it->gateOn)) ScenesChanged();
        y += rowH;
        if (it->gateOn) slider("gatedb", L"Gate (dB)", it->gateDb, -70, -10);
        if (u.Toggle("comp", {cx, y, cw, rowH}, L"Compressor", it->compOn)) ScenesChanged();
        y += rowH;
        if (it->compOn) {
            slider("compdb", L"Threshold (dB)", it->compDb, -40, 0);
            slider("compratio", L"Ratio", it->compRatio, 1, 20);
        }
    } else if (it->kind == SourceKind::Text) {
        if (u.TextBox("text", {cx, y, cw, rowH}, it->text, L"Text")) ScenesChanged();
        y += rowH + u.S(4);
        const float before = it->fontSize;
        slider("fontsize", L"Font size", it->fontSize, 12, 240);
        if (it->fontSize != before) {
            // Keep the on-screen scale: grow/shrink the box with the font.
            it->rect.h *= it->fontSize / std::max(1.0f, before);
        }
        {
            const auto& fonts = FontFamilies();
            int sel = -1;
            for (size_t k = 0; k < fonts.size(); ++k)
                if (_wcsicmp(fonts[k].c_str(), it->fontFamily.c_str()) == 0) sel = int(k);
            if (u.Dropdown("font", {cx, y, cw, rowH}, fonts, sel, 0, it->fontFamily) && sel >= 0) {
                it->fontFamily = fonts[size_t(sel)];
                ScenesChanged();
            }
            y += rowH + u.S(4);
            int weight = it->fontWeight >= 700 ? 2 : (it->fontWeight >= 600 ? 1 : 0);
            const bool narrow = cw < u.S(330);   // the italic switch gets its own row, so no label is cut
            if (u.Segmented("fontweight", {cx, y, narrow ? cw : cw * 0.7f, rowH}, {L"Regular", L"Semibold", L"Bold"}, weight)) {
                it->fontWeight = weight == 2 ? 700 : (weight == 1 ? 600 : 400);
                ScenesChanged();
            }
            if (narrow) y += rowH + u.S(4);
            if (u.Toggle("italic", narrow ? RectF{cx, y, cw, rowH} : RectF{cx + cw * 0.74f, y, cw * 0.26f, rowH}, L"Italic", it->italic)) ScenesChanged();
            y += rowH + u.S(6);
        }
        u.Text(L"Color", {cx, y, cw, u.S(22)}, u.S(12.5f), theme::Text);
        y += u.S(24);
        if (u.ColorPicker("textcolor", {cx, y, cw, u.ColorPickerHeight()}, it->textColor)) ScenesChanged();
        y += u.ColorPickerHeight() + u.S(8);
    } else if (it->kind == SourceKind::Color) {
        if (u.ColorPicker("color", {cx, y, cw, u.ColorPickerHeight()}, it->color)) ScenesChanged();
        y += u.ColorPickerHeight() + u.S(8);
    } else if (it->kind == SourceKind::Image) {
        if (u.Button("chooseimg", {cx, y, cw, rowH}, L"Choose image\u2026", glyph::Picture)) PickImage(it->id);
        y += rowH + u.S(4);
    }
    y += u.S(10);
    if (!PanelSection("a.filters", L"Filters", y, cx, cw)) return;
    auto& f = it->filters;
    if (u.Toggle("chroma", {cx, y, cw, rowH}, L"Chroma key", f.key.enabled)) ScenesChanged();
    y += rowH;
    if (f.key.enabled) {
        slider("ksim", L"Similarity", f.key.similarity, 0, 1);
        slider("ksmooth", L"Smoothness", f.key.smoothness, 0, 0.5f);
        slider("kspill", L"Spill", f.key.spill, 0, 1);
    }
    if (u.Toggle("cc", {cx, y, cw, rowH}, L"Color correction", f.color.enabled)) ScenesChanged();
    y += rowH;
    if (f.color.enabled) {
        slider("bright", L"Brightness", f.color.brightness, -0.5f, 0.5f);
        slider("contrast", L"Contrast", f.color.contrast, 0.5f, 2.0f);
        slider("sat", L"Saturation", f.color.saturation, 0, 2);
        slider("gamma", L"Gamma", f.color.gamma, 0.4f, 2.5f);
        slider("hue", L"Hue", f.color.hue, -180, 180);
    }
    slider("sharpen", L"Sharpen", f.sharpen, 0, 1);
}

} // namespace cc
