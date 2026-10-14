// Per-device profiles: picture shape, scaling, screen filter, auto-crop, Enhance and audio delay are kept for each
// device (a retro console on one card, a modern one on another). Simple mode edits the shown device's profile
// through the usual members (m_scale, m_aspect, ...); multiview tiles, device windows and the mini player draw each
// device with its own. Advanced mode and "no device" use the general values ([Video] / [Audio]), which are also
// where a device without saved values starts.
#include "App.h"
#include "core/Log.h"
#include "devices/TestPattern.h"

namespace cc {

DeviceProfile App::CurrentProfile() const
{
    DeviceProfile p;
    p.scale = int(m_scale);
    p.aspect = m_aspect;
    p.filter = m_screenFilter;
    p.filterLines = m_filterLines;
    p.filterStrength = m_filterStrength;
    p.autoCrop = m_autoCrop;
    p.enhance = m_enhance;
    p.enhanceStrength = m_enhanceStrength;
    p.delayMs = m_delayMs;
    p.xf = m_xf;
    p.hdr = m_hdr;
    return p;
}

void App::UseProfile(const DeviceProfile& p)
{
    m_scale = ScaleMode(p.scale);
    m_aspect = p.aspect;
    m_screenFilter = p.filter;
    m_filterLines = p.filterLines;
    m_filterStrength = p.filterStrength;
    m_autoCrop = p.autoCrop;
    m_enhance = p.enhance;
    m_enhanceStrength = p.enhanceStrength;
    m_delayMs = p.delayMs;
    m_xf = p.xf;
    m_hdr = p.hdr;
    m_audio.avOffsetMs = std::max(0, p.delayMs);   // the speakers follow the shown device (< 0: the picture waits instead)
}

// section empty = the general values. Command-line overrides apply to every device for this run (never saved).
DeviceProfile App::LoadProfile(const std::wstring& section) const
{
    const bool general = section.empty();
    const std::wstring v = general ? L"Video" : section, a = general ? L"Audio" : section;
    const DeviceProfile d = general ? DeviceProfile{} : m_generalProfile;
    DeviceProfile p;
    p.scale = Clamp(m_settings.GetInt(v, L"Scaling", d.scale), 0, 3);
    p.aspect = Clamp(m_settings.GetInt(v, L"Aspect", d.aspect), 0, 3);
    p.filter = Clamp(m_settings.GetInt(v, L"Filter", d.filter), 0, 4);
    p.filterLines = Clamp(m_settings.GetInt(v, L"FilterLines", d.filterLines), 0, 2160);
    p.filterStrength = Clamp(m_settings.GetFloat(v, L"FilterStrength", d.filterStrength), 0.0f, 1.0f);
    p.autoCrop = m_settings.GetBool(v, L"AutoCrop", d.autoCrop);
    p.enhance = Clamp(m_settings.GetInt(v, L"Enhance", d.enhance), 0, 2);
    p.enhanceStrength = Clamp(m_settings.GetFloat(v, L"EnhanceStrength", d.enhanceStrength), 0.0f, 1.0f);
    p.delayMs = Clamp(m_settings.GetInt(a, L"DelayMs", d.delayMs), -kMaxPictureDelayMs, 500);
    const wchar_t* cropKeys[4] = {L"CropLeft", L"CropTop", L"CropRight", L"CropBottom"};
    for (int i = 0; i < 4; ++i) p.xf.crop[i] = Clamp(m_settings.GetFloat(v, cropKeys[i], d.xf.crop[i]), 0.0f, 0.45f);
    p.xf.rotate = Clamp(m_settings.GetInt(v, L"Rotate", d.xf.rotate), 0, 3);
    p.xf.mirror = m_settings.GetBool(v, L"Mirror", d.xf.mirror);
    p.hdr.mode = Clamp(m_settings.GetInt(v, L"HdrMode", d.hdr.mode), 0, 3);
    p.hdr.whiteNits = Clamp(m_settings.GetFloat(v, L"HdrWhite", d.hdr.whiteNits), 80.0f, 400.0f);
    p.hdr.peakNits = Clamp(m_settings.GetFloat(v, L"HdrPeak", d.hdr.peakNits), 400.0f, 10000.0f);
    if (m_cmd.scale >= 0) p.scale = Clamp(m_cmd.scale, 0, 3);
    if (m_cmd.filter >= 0) p.filter = Clamp(m_cmd.filter, 0, 4);
    if (m_cmd.filterLines >= 0) p.filterLines = Clamp(m_cmd.filterLines, 0, 2160);
    if (m_cmd.autoCrop) p.autoCrop = true;
    if (m_cmd.enhance >= 0) p.enhance = Clamp(m_cmd.enhance, 0, 2);
    if (m_cmd.enhanceStrength >= 0) p.enhanceStrength = Clamp(m_cmd.enhanceStrength, 0.0f, 1.0f);
    if (m_cmd.rotate >= 0) p.xf.rotate = Clamp(m_cmd.rotate, 0, 3);
    if (m_cmd.syncMs != 9999) p.delayMs = Clamp(m_cmd.syncMs, -kMaxPictureDelayMs, 500);
    if (m_cmd.hdrMode >= 0) p.hdr.mode = Clamp(m_cmd.hdrMode, 0, 3);
    if (m_cmd.mirror) p.xf.mirror = true;
    for (int i = 0; i < 4; ++i)
        if (m_cmd.crop[i] >= 0) p.xf.crop[i] = Clamp(m_cmd.crop[i], 0.0f, 0.45f);
    return p;
}

void App::SaveProfile(const std::wstring& section, const DeviceProfile& p)
{
    const bool general = section.empty();
    const std::wstring v = general ? L"Video" : section, a = general ? L"Audio" : section;
    if (m_cmd.scale < 0) m_settings.SetInt(v, L"Scaling", p.scale);
    m_settings.SetInt(v, L"Aspect", p.aspect);
    if (m_cmd.filter < 0) m_settings.SetInt(v, L"Filter", p.filter);
    if (m_cmd.filterLines < 0) m_settings.SetInt(v, L"FilterLines", p.filterLines);
    m_settings.SetFloat(v, L"FilterStrength", p.filterStrength);
    if (!m_cmd.autoCrop) m_settings.SetBool(v, L"AutoCrop", p.autoCrop);
    if (m_cmd.enhance < 0) m_settings.SetInt(v, L"Enhance", p.enhance);
    if (m_cmd.enhanceStrength < 0) m_settings.SetFloat(v, L"EnhanceStrength", p.enhanceStrength);
    if (m_cmd.syncMs == 9999) m_settings.SetInt(a, L"DelayMs", p.delayMs);
    const wchar_t* cropKeys[4] = {L"CropLeft", L"CropTop", L"CropRight", L"CropBottom"};
    for (int i = 0; i < 4; ++i)
        if (m_cmd.crop[i] < 0) m_settings.SetFloat(v, cropKeys[i], p.xf.crop[i]);
    if (m_cmd.rotate < 0) m_settings.SetInt(v, L"Rotate", p.xf.rotate);
    if (!m_cmd.mirror) m_settings.SetBool(v, L"Mirror", p.xf.mirror);
    if (m_cmd.hdrMode < 0) m_settings.SetInt(v, L"HdrMode", p.hdr.mode);
    m_settings.SetFloat(v, L"HdrWhite", p.hdr.whiteNits);
    m_settings.SetFloat(v, L"HdrPeak", p.hdr.peakNits);
}

// Every frame (cheap): the members follow the shown device. When it changes, the members are handed back to the
// previous owner and the new device's profile is loaded into them.
void App::SyncProfile()
{
    Session* s = m_advanced ? nullptr : Active();
    const std::wstring key = s ? s->info.Key() : std::wstring();
    const DeviceProfile cur = CurrentProfile();
    if (key == m_profileKey) {
        if (s && !(s->prof.xf == cur.xf)) {   // crop / turn changed: the recording encoder follows a new size (Maintain)
            m_xfChangedLink = s->info.symlink;
            m_xfChangedQpc = QpcNow();
        }
        if (s) s->prof = cur;
        else m_generalProfile = cur;
        return;
    }
    if (m_profileKey.empty()) m_generalProfile = cur;
    else
        for (auto& o : m_sessions)   // a device that closed meanwhile was saved by CloseSession
            if (o->info.Key() == m_profileKey) o->prof = cur;
    if (!s && m_advanced) m_generalProfile.delayMs = std::max(0, m_generalProfile.delayMs);   // the canvas can't wait for late sound
    UseProfile(s ? s->prof : m_generalProfile);
    m_profileKey = key;
    InvalidateUi();
}

// From SaveSettings: the general values, and each open device whose profile changed since it was saved.
void App::SaveProfiles()
{
    SyncProfile();
    SaveProfile(L"", m_generalProfile);
    for (auto& s : m_sessions)
        if (!(s->prof == s->savedProf)) {
            SaveProfile(Settings::DeviceSection(s->info.Key()), s->prof);
            s->savedProf = s->prof;
        }
}

double App::ShownAspect(const VideoSurface& sf, UINT w, UINT h, int aspect)
{
    if (!w || !h) return 1.0;
    const UINT sw = sf.srcW ? sf.srcW : w, sh = sf.srcH ? sf.srcH : h;
    double par = DisplayAspect(sw, sh, aspect) / (double(sw) / double(sh));   // the source's pixel shape
    if (sf.rotated) par = 1.0 / par;
    return double(w) / double(h) * par;
}

bool App::TransformLocked(const Session& s) const
{
    if (m_recorder && m_encoderSymlink == s.info.symlink) return true;
    for (const auto& x : m_extraRecs)
        if (x.link == s.info.symlink) return true;
    return false;
}

// Settings ▸ HDR (experimental): the master switch and every HDR setting. HDR source, brightness and highlights belong to
// the shown device's profile (Simple mode); real HDR on HDR screens and HDR recordings apply to all devices.
void App::DrawHdrSettings(float& y, float cx, float cw)
{
    Ui& u = m_ui;
    const float rowH = u.S(34), labelH = u.S(22), gapS = u.S(10);
    auto note = [&](const std::wstring& t, Color c = theme::TextDim) {
        const float th = std::max(u.S(18), u.MeasureBlock(t, cw, u.S(11.5f)));
        u.TextBlock(t, {cx, y + u.S(1), cw, th + u.S(2)}, u.S(11.5f), c);
        y += th;
    };
    Session* s = Active();
    const bool simple = s && !m_advanced;
    const VideoFormat f = s ? s->video->Current() : VideoFormat{};
    const Transfer reported = f.transfer;

    note(L"Still being tested: these features may change, and some PCs, screens and capture cards may not get on with them yet.");
    y += u.S(6);
    if (u.Toggle("hdrenabled", {cx, y, cw, rowH}, L"HDR features", m_hdrEnabled, false, 0)) m_settingsDirty = true;
    y += rowH;
    if (!HdrOn()) {
        std::wstring t = L"Off: a source that says it sends HDR is tone-mapped to SDR, so it looks right anyway. On adds: choosing how a "
                         L"source's HDR is read (HDR10 / HLG for cards that don't flag it), real HDR on HDR screens, HDR brightness and "
                         L"HDR10 recordings.";
        if (simple && reported != Transfer::Sdr) t += std::wstring(L" The shown source says ") + TransferName(reported) + L": tone-mapped now.";
        note(t);
        y += gapS;
        return;
    }
    if (!m_hdrEnabled) {   // a test flag turned it on for this run
        note(L"On for this run (test option).", theme::Warn);
    }
    y += gapS;

    // ---- HDR source (per device, Simple mode) ----
    const Transfer shown = simple && s->surface.hasImage ? s->surface.transfer : Transfer::Sdr;
    if (simple) {
        std::wstring now = m_hdr.mode == 0 ? (reported != Transfer::Sdr ? std::wstring(TransferName(reported)) + L" (the device says so)"
                                                                        : std::wstring(L"SDR (no HDR flag from the device)"))
                                           : std::wstring(L"set to ") + TransferName(shown);
        u.Text(L"HDR source", {cx, y, cw, labelH}, u.S(13), theme::Text);
        if (m_hdr.mode != 0 && u.ResetIcon("reset:hdr", cx + u.Measure(L"HDR source", u.S(13)) + u.S(4), y, labelH)) {
            m_hdr.mode = 0;
            m_settingsDirty = true;
        }
        u.Text(now, {cx, y, cw, labelH}, u.S(12), theme::TextDim, Align::Right);
        y += labelH + u.S(4);
        if (u.Segmented("hdrmode", {cx, y, cw, rowH}, {L"Auto", L"Off", L"HDR10", L"HLG"}, m_hdr.mode)) m_settingsDirty = true;
        y += rowH;
        std::wstring hint;
        if (shown == Transfer::Sdr)
            hint = m_hdr.mode == 1 ? L"Shown as SDR even if the device says HDR."
                                   : L"Washed-out, grey picture from an HDR console or phone? Its HDR may reach the card without a flag: "
                                     L"pick HDR10 (or HLG).";
        else if (m_renderer.HdrOutput())
            hint = Format(L"Shown in real HDR on this screen (up to %.0f nits). Screen filters and Enhance don't apply to it; sharing and "
                          L"screenshots get the tone-mapped SDR picture.",
                          m_hdrDisplayInfo.peakNits);
        else if (m_hdrDisplayInfo.hdr && !m_hdrScreen)
            hint = L"Tone-mapped to SDR (real HDR on this screen is switched off below).";
        else if (m_hdrDisplayInfo.hdr && m_hdrScreenFailed)
            hint = L"Tone-mapped to SDR: the graphics driver refused HDR output for this window.";
        else
            hint = L"Tone-mapped for this SDR screen (Windows HDR is off for this monitor): recordings, sharing and screenshots get the "
                   L"same picture.";
        hint += L" Saved for " + s->info.name + L", like the brightness below.";
        note(hint);
        y += gapS;
    } else {
        note(m_advanced ? L"Advanced mode composes and records in SDR: HDR sources in a scene are tone-mapped. HDR source and brightness "
                          L"are set per device in Simple mode."
                        : L"No device is shown: HDR source and brightness are set per device once one is.");
        y += gapS;
    }

    // ---- real HDR on HDR screens (all devices) ----
    {
        const bool screenHdr = m_hdrDisplayInfo.hdr || m_renderer.HdrOutput();
        if (u.Toggle("hdrscreen", {cx, y, cw, rowH}, L"Real HDR on HDR screens", m_hdrScreen, !screenHdr, 1)) m_settingsDirty = true;
        y += rowH;
        if (!screenHdr)
            note(L"This screen isn't in HDR mode (Windows Settings ▸ System ▸ Display ▸ Use HDR). HDR pictures are tone-mapped for it.");
        else if (m_profile.low)
            note(L"Low-resource mode shows HDR tone-mapped (real HDR output costs more).");
        else
            note(Format(L"Windows HDR is on for this screen (up to %.0f nits, SDR white %.0f nits).", m_hdrDisplayInfo.peakNits,
                        m_hdrDisplayInfo.sdrWhiteNits));
        y += gapS;
    }

    // ---- brightness (per device, only while an HDR picture is shown) ----
    if (simple && shown != Transfer::Sdr) {
        // Lower "white" = brighter picture; "highlights" = how bright the brightest parts of the source get.
        u.Text(L"HDR brightness", {cx, y, cw, labelH}, u.S(13), theme::Text);
        u.Text(Format(L"white = %.0f nits", m_hdr.whiteNits), {cx, y, cw, labelH}, u.S(12), theme::TextDim, Align::Right);
        y += labelH + u.S(4);
        float bright = 480.0f - m_hdr.whiteNits;   // right = brighter
        if (u.Slider("hdrwhite", {cx, y, cw, rowH}, bright, 80.0f, 400.0f)) {
            m_hdr.whiteNits = std::round(480.0f - bright);
            m_settingsDirty = true;
        }
        y += rowH + gapS;
        u.Text(L"Highlights up to", {cx, y, cw, labelH}, u.S(13), theme::Text);
        u.Text(Format(L"%.0f nits", m_hdr.peakNits), {cx, y, cw, labelH}, u.S(12), theme::TextDim, Align::Right);
        y += labelH + u.S(4);
        float peak = std::log10(m_hdr.peakNits);
        if (u.Slider("hdrpeak", {cx, y, cw, rowH}, peak, std::log10(400.0f), 4.0f)) {
            m_hdr.peakNits = std::round(std::pow(10.0f, peak) / 50.0f) * 50.0f;
            m_settingsDirty = true;
        }
        y += rowH;
        note(L"The brightest the source gets (1000 for most games and films). Brighter parts are shown as white.");
        y += gapS;
    } else if (simple) {
        note(L"HDR brightness and highlights appear here while an HDR picture is shown.");
        y += gapS;
    }

    // ---- HDR recordings (all devices) ----
    {
        // HDR sources: HDR10 files (HEVC 10-bit, BT.2020, PQ) or tone-mapped SDR. Says what happens with what is shown now.
        if (u.Toggle("recordhdr", {cx, y, cw, rowH}, L"HDR recordings", m_recordHdr, false, 1)) m_settingsDirty = true;
        y += rowH;
        std::wstring h;
        if (m_advanced)
            h = L"Advanced mode records the canvas in SDR (HDR sources in it are tone-mapped).";
        else if (!m_recordHdr)
            h = L"Off: HDR sources are recorded tone-mapped, as SDR in the recording format (plays everywhere).";
        else if (m_encoder && m_encoderHdrAsked && !m_encoder->Config().hdr)
            h = L"This PC can't encode 10-bit HEVC: HDR sources are recorded tone-mapped, as SDR.";
        else if (shown != Transfer::Sdr)
            h = L"The shown source is HDR: it's recorded as HDR10 (HEVC 10-bit, whatever the recording format). Players without HDR "
                L"show it washed out; Off records SDR that plays everywhere.";
        else
            h = L"HDR sources (HDR10, HLG) are recorded as HDR10 (HEVC 10-bit). The shown source is SDR: recorded as usual.";
        note(h);
        y += gapS;
    }
}

// Settings ▸ Video ▸ Crop and turn (Simple mode: the shown device's profile).
void App::DrawTransformSettings(float& y, float cx, float cw)
{
    Ui& u = m_ui;
    const float rowH = u.S(34), labelH = u.S(22), gapS = u.S(10);
    Session* s = Active();
    if (!s || m_advanced) return;
    const bool locked = TransformLocked(*s);
    const VideoSurface& sf = s->surface;
    u.Text(L"Crop and turn", {cx, y, cw, labelH}, u.S(13), theme::Text);
    if (!m_xf.Identity() && !locked && u.ResetIcon("reset:xf", cx + u.Measure(L"Crop and turn", u.S(13)) + u.S(4), y, labelH)) {
        m_xf = PictureTransform{};
        m_settingsDirty = true;
    }
    if (sf.srcW) {
        const std::wstring size = sf.width == sf.srcW && sf.height == sf.srcH ? Format(L"%u×%u", sf.width, sf.height)
                                                                                : Format(L"%u×%u of %u×%u", sf.width, sf.height, sf.srcW, sf.srcH);
        u.Text(size, {cx, y, cw, labelH}, u.S(12), theme::TextDim, Align::Right);
    }
    y += labelH + u.S(4);
    if (locked) {
        const bool cropped = m_xf.crop[0] > 0 || m_xf.crop[1] > 0 || m_xf.crop[2] > 0 || m_xf.crop[3] > 0;
        std::wstring now = m_xf.rotate ? Format(L"Turned %d°", m_xf.rotate * 90) : std::wstring();
        if (m_xf.mirror) now += now.empty() ? L"Mirrored" : L", mirrored";
        if (cropped) now += now.empty() ? L"Cropped" : L", cropped";
        now += now.empty() ? L"Off." : L".";
        const std::wstring t = now + L" Stop recording to change it: the recording keeps one picture size.";
        const float th = u.MeasureBlock(t, cw, u.S(11.5f));
        u.TextBlock(t, {cx, y, cw, th + u.S(2)}, u.S(11.5f), theme::TextDim);
        y += th + gapS;
        return;
    }
    if (u.Segmented("rotate", {cx, y, cw, rowH}, {L"0°", L"90°", L"180°", L"270°"}, m_xf.rotate)) m_settingsDirty = true;
    y += rowH + gapS;
    if (u.Toggle("mirror", {cx, y, cw, rowH}, L"Mirror (flip left and right)", m_xf.mirror, false, 0)) m_settingsDirty = true;
    y += rowH + gapS;
    {
        std::wstring why;
        const bool can = CanCropOnPicture(&why);
        if (u.Button("cropedit", {cx, y, cw, rowH}, L"Crop on the picture\u2026", 0, false, !can)) BeginCropEdit();
        y += rowH;
        if (!can) {
            const float th = u.MeasureBlock(why, cw, u.S(11.5f));
            u.TextBlock(why, {cx, y + u.S(2), cw, th + u.S(2)}, u.S(11.5f), theme::TextDim);
            y += th + u.S(2);
        }
        y += gapS;
    }
    // Four crop sliders, two per row; the label says how many source pixels go.
    const wchar_t* names[4] = {L"Left", L"Top", L"Right", L"Bottom"};
    const int order[4] = {0, 2, 1, 3};   // left, right / top, bottom
    const float half = (cw - u.S(16)) / 2.0f;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            const int i = order[row * 2 + col];
            const float x = cx + col * (half + u.S(16));
            const UINT n = (i == 0 || i == 2) ? sf.srcW : sf.srcH;
            const UINT px = UINT(std::lround(m_xf.crop[i] * float(n) / 2.0f)) * 2;
            u.Text(names[i], {x, y, half, labelH}, u.S(12), theme::TextDim);
            u.Text(n ? Format(L"%u px", px) : Format(L"%.0f%%", m_xf.crop[i] * 100.0f), {x, y, half, labelH}, u.S(12), theme::TextDim, Align::Right);
            const std::string id = "crop" + std::to_string(i);
            if (u.Slider(id.c_str(), {x, y + labelH, half, rowH}, m_xf.crop[i], 0.0f, 0.45f)) {
                if (m_xf.crop[i] < 0.002f) m_xf.crop[i] = 0.0f;
                m_settingsDirty = true;
            }
        }
        y += labelH + rowH + u.S(4);
    }
    const wchar_t* hint = L"Shown, recorded, shared and in screenshots like this. Saved for this device.";
    const float th = u.MeasureBlock(hint, cw, u.S(11.5f));
    u.TextBlock(hint, {cx, y + u.S(2), cw, th + u.S(2)}, u.S(11.5f), theme::TextDim);
    y += th + u.S(4) + gapS;
}

// ---------------------------------------------------------------------------------------------
// Device names. Identical cards get keys like "USB3.0 Capture #1a2b3c4d" (DeviceManager); the first time a card
// shows up under such a key it takes over the settings saved under the plain name, so a second card arriving
// doesn't reset the first one's profile, format or window.

void App::NameDevices(std::vector<VideoDeviceInfo>& video)
{
    for (auto& d : video) {
        if (d.key.empty()) continue;
        const std::wstring sec = Settings::DeviceSection(d.key);
        if (d.key != d.autoName && !m_settings.HasSection(sec)) {
            const size_t hash = d.key.rfind(L" #");
            if (hash != std::wstring::npos) {
                const std::wstring plain = Settings::DeviceSection(d.key.substr(0, hash));
                if (m_settings.HasSection(plain)) {
                    m_settings.CopySection(plain, sec);
                    LOGI(L"%s: identical cards connected, settings of %s copied for this one", d.key.c_str(), plain.c_str());
                }
            }
            // Its hidden state and "last device" carry over too.
            if (m_hiddenDevices.contains(d.key.substr(0, d.key.rfind(L" #")))) m_hiddenDevices.insert(d.key);
        }
        const std::wstring custom = m_settings.GetString(sec, L"Name", L"");
        if (!custom.empty()) d.name = custom;
    }
}

std::wstring App::NameForKey(const std::wstring& key) const
{
    for (const auto& d : m_localDevices.video)
        if (d.Key() == key) return d.name;
    const std::wstring custom = m_settings.GetString(Settings::DeviceSection(key), L"Name", L"");
    if (!custom.empty()) return custom;
    const size_t hash = key.rfind(L" #");
    return hash == std::wstring::npos ? key : key.substr(0, hash);
}

void App::RenameDevice(const std::wstring& key, std::wstring name)
{
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    if (name.size() > 60) name.resize(60);
    std::wstring autoName;
    for (const auto& d : m_localDevices.video)
        if (d.Key() == key) autoName = d.autoName;
    if (name == autoName) name.clear();   // typed Windows' name: no custom name
    m_nameEdits[key] = name;
    const std::wstring sec = Settings::DeviceSection(key);
    if (name == m_settings.GetString(sec, L"Name", L"")) return;
    m_settings.SetString(sec, L"Name", name);
    LOGI(L"Device %s renamed to \"%s\"", key.c_str(), name.empty() ? autoName.c_str() : name.c_str());
    // Everywhere at once: device lists, open sessions, device windows.
    for (auto& d : m_localDevices.video)
        if (d.Key() == key) d.name = name.empty() ? d.autoName : name;
    for (auto& d : m_devices.video)
        if (d.Key() == key) d.name = name.empty() ? d.autoName : name;
    for (auto& s : m_sessions)
        if (s->info.Key() == key) s->info.name = name.empty() ? s->info.autoName : name;
    for (auto& w : m_deviceWindows)
        if (w->key == key) {
            w->name = NameForKey(key);
            // Its title, from this thread without ever waiting long on the window thread.
            if (HWND h = w->hwnd.load())
                SendMessageTimeoutW(h, WM_SETTEXT, 0, LPARAM(w->name.c_str()), SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, nullptr);
        }
    InvalidateUi();
}

// Settings ▸ Devices ▸ Copy these settings to: the shown device's whole profile (shape, scaling, filters, Enhance,
// sync, crop / turn) onto another device, e.g. a second identical card. Its format and sound choice stay its own.
void App::CopyProfileTo(const std::wstring& key)
{
    const Session* from = Active();
    if (!from || key == m_profileKey) return;
    const DeviceProfile p = CurrentProfile();
    const bool readOnly = m_cmd.selftestSec > 0 && m_cmd.profileDir.empty();
    if (!readOnly) SaveProfile(Settings::DeviceSection(key), p);
    for (auto& s : m_sessions)
        if (s->info.Key() == key) s->prof = s->savedProf = p;   // open (multiview tile, device window): at once
    LOGI(L"Profile of %s copied to %s", m_profileKey.c_str(), key.c_str());
    Toast(L"Settings of " + from->info.name + L" copied to " + NameForKey(key));
}

// --twin-test (with --device "Twin card (2)"): 30 % rename the shown twin, 60 % back to its automatic name.
void App::TwinSelftest(double t)
{
    static int step = 0;
    if (step == 0) {
        for (const auto& d : m_localDevices.video)
            if (d.symlink.rfind(L"test:twin", 0) == 0)
                LOGI(L"[selftest] twin %s: name \"%s\", key \"%s\", filter %d", d.symlink.c_str(), d.name.c_str(), d.Key().c_str(),
                     m_settings.GetInt(Settings::DeviceSection(d.Key()), L"Filter", -1));
        step = 1;
    }
    Session* s = Active();
    if (!s || step >= 3 || t < m_cmd.selftestSec * (step == 1 ? 0.3 : 0.6)) return;
    const std::wstring key = s->info.Key();
    if (step++ == 1) {
        RenameDevice(key, L"  Second card ");
        const bool ok = s->info.name == L"Second card" && s->info.Key() == key && NameForKey(key) == L"Second card";
        LOGI(L"[selftest] twin rename %s: \"%s\" key \"%s\"", ok ? L"PASS" : L"FAIL", s->info.name.c_str(), key.c_str());
    } else {
        RenameDevice(key, L"");
        const bool ok = s->info.name == s->info.autoName && s->info.Key() == key;
        LOGI(L"[selftest] twin rename back %s: \"%s\"", ok ? L"PASS" : L"FAIL", s->info.name.c_str());
    }
}

// --profile-test (needs --test-pattern and another shown device, e.g. --device USB3.0): 25 % the shown device gets CRT, 4:3 and a
// 120 ms picture delay; 45 % switch to another device (its own profile must load); 70 % back (the changes must return).
void App::ProfileSelftest(double t)
{
    static int step = 0;
    static std::wstring first;
    static DeviceProfile other;
    const double at[] = {0.25, 0.45, 0.55, 0.70, 0.85};
    if (step >= 5 || t < m_cmd.selftestSec * at[step]) return;
    Session* s = Active();
    if (!s) return;
    auto describe = [](const DeviceProfile& p) {
        return Format(L"scale %d aspect %d filter %d lines %d autocrop %d enhance %d delay %d", p.scale, p.aspect, p.filter, p.filterLines,
                      p.autoCrop ? 1 : 0, p.enhance, p.delayMs);
    };
    auto switchTo = [&](bool toFirst) {
        for (size_t i = 0; i < m_devices.video.size(); ++i)
            if (ShownInSimple(m_devices.video[i]) && (m_devices.video[i].Key() == first) == toFirst &&
                (toFirst || TestPattern::IsTestLink(m_devices.video[i].symlink))) {   // the other one: the test pattern only
                SwitchTo(i);
                return true;
            }
        return false;
    };
    switch (step++) {
    case 0:
        first = s->info.Key();
        LOGI(L"[selftest] profile of %s before: %s", first.c_str(), describe(CurrentProfile()).c_str());
        m_screenFilter = 3;
        m_aspect = 1;
        m_delayMs = -120;
        m_settingsDirty = true;
        break;
    case 1:
        if (!switchTo(false)) LOGW(L"[selftest] profile FAIL: no second device to switch to");
        break;
    case 2:
        other = CurrentProfile();
        LOGI(L"[selftest] profile of %s: %s, speakers delay %d", s->info.Key().c_str(), describe(other).c_str(), m_audio.avOffsetMs.load());
        LOGI(L"[selftest] profile %s: the second device has its own values", other.filter != 3 || other.aspect != 1 || other.delayMs != -120 ? L"PASS" : L"FAIL");
        break;
    case 3:
        switchTo(true);
        break;
    case 4: {
        const DeviceProfile p = CurrentProfile();
        const bool ok = s->info.Key() == first && p.filter == 3 && p.aspect == 1 && p.delayMs == -120 && m_audio.avOffsetMs == 0;
        LOGI(L"[selftest] profile of %s back: %s, picture delay ring %s (%.0f MB)", s->info.Key().c_str(), describe(p).c_str(),
             s->pictureDelay.Empty() ? L"empty" : L"filled", double(s->pictureDelay.Bytes()) / (1024.0 * 1024.0));
        LOGI(L"[selftest] profile %s: the first device's changes came back", ok && !s->pictureDelay.Empty() ? L"PASS" : L"FAIL");
        break;
    }
    }
}

} // namespace cc
