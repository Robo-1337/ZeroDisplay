#include "scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace cc {

const wchar_t* SourceKindName(SourceKind k)
{
    switch (k) {
    case SourceKind::Device: return L"Device";
    case SourceKind::Image: return L"Image";
    case SourceKind::Text: return L"Text";
    case SourceKind::Color: return L"Color";
    }
    return L"Source";
}

SceneItem* SceneCollection::Find(Scene& s, uint32_t id)
{
    for (auto& it : s.items)
        if (it.id == id) return &it;
    return nullptr;
}

SceneItem& SceneCollection::Add(Scene& s, SourceKind kind, const std::wstring& name)
{
    SceneItem it;
    it.id = nextId++;
    it.kind = kind;
    it.name = name;
    it.rect = {0, 0, float(width), float(height)};
    s.items.push_back(std::move(it));
    return s.items.back();
}

RectF SceneCollection::FitToCanvas(float srcW, float srcH) const
{
    if (srcW <= 0 || srcH <= 0) return {0, 0, float(width), float(height)};
    float s = std::min(float(width) / srcW, float(height) / srcH);
    float w = std::round(srcW * s), h = std::round(srcH * s);
    return {std::round((float(width) - w) * 0.5f), std::round((float(height) - h) * 0.5f), w, h};
}

static std::wstring Hex(uint32_t v) { return Format(L"%08X", v); }
static uint32_t ParseHex(const std::wstring& s, uint32_t def) { return s.empty() ? def : uint32_t(wcstoul(s.c_str(), nullptr, 16)); }

bool SceneCollection::Load(Settings& ini)
{
    const std::wstring c = L"Collection";
    const int count = std::min(ini.GetInt(c, L"Scenes", 0), kMaxScenes);
    if (count <= 0) return false;
    width = UINT(Clamp(ini.GetInt(c, L"Width", 1920), 320, 7680));
    height = UINT(Clamp(ini.GetInt(c, L"Height", 1080), 180, 4320));
    fpsNum = UINT(Clamp(ini.GetInt(c, L"FpsNum", 60), 1, 240000));
    fpsDen = UINT(Clamp(ini.GetInt(c, L"FpsDen", 1), 1, 1001));
    if (fpsNum / fpsDen > 240 || fpsNum / fpsDen < 1) fpsNum = 60, fpsDen = 1;
    nextId = uint32_t(std::max(1, ini.GetInt(c, L"NextId", 1)));
    scenes.clear();
    for (int i = 0; i < count; ++i) {
        const std::wstring ss = Format(L"Scene.%d", i);
        Scene sc;
        sc.name = ini.GetString(ss, L"Name", Format(L"Scene %d", i + 1).c_str());
        const int items = Clamp(ini.GetInt(ss, L"Items", 0), 0, kMaxItems);
        for (int j = 0; j < items; ++j) {
            const std::wstring is = Format(L"Scene.%d.Item.%d", i, j);
            SceneItem it;
            it.id = uint32_t(ini.GetInt(is, L"Id", int(nextId)));
            nextId = std::max(nextId, it.id + 1);
            it.name = ini.GetString(is, L"Name", L"Source");
            it.kind = SourceKind(Clamp(ini.GetInt(is, L"Kind", 0), 0, 3));
            it.link = ini.GetString(is, L"Link");
            it.deviceName = ini.GetString(is, L"DeviceName");
            it.path = ini.GetString(is, L"Path");
            it.text = ini.GetString(is, L"Text");
            // The profile API trims values; newlines are stored escaped.
            for (size_t p; (p = it.text.find(L"\\n")) != std::wstring::npos;) it.text.replace(p, 2, L"\n");
            it.fontSize = ini.GetFloat(is, L"FontSize", 72.0f);
            it.textColor = ParseHex(ini.GetString(is, L"TextColor"), 0xFFFFFFFF);
            it.fontFamily = ini.GetString(is, L"Font", L"Segoe UI");
            if (it.fontFamily.empty()) it.fontFamily = L"Segoe UI";
            it.fontWeight = Clamp(ini.GetInt(is, L"FontWeight", 600), 100, 950);
            it.italic = ini.GetBool(is, L"Italic", false);
            it.color = ParseHex(ini.GetString(is, L"Color"), 0xFF1E2430);
            const float lim = 8.0f * float(std::max(width, height));   // well off the canvas is fine, astronomically far isn't
            it.rect = {Clamp(ini.GetFloat(is, L"X", 0), -lim, lim), Clamp(ini.GetFloat(is, L"Y", 0), -lim, lim),
                       Clamp(ini.GetFloat(is, L"W", float(width)), 1.0f, lim), Clamp(ini.GetFloat(is, L"H", float(height)), 1.0f, lim)};
            it.cropL = Clamp(ini.GetFloat(is, L"CropL", 0), 0.0f, 0.95f);
            it.cropT = Clamp(ini.GetFloat(is, L"CropT", 0), 0.0f, 0.95f);
            it.cropR = Clamp(ini.GetFloat(is, L"CropR", 0), 0.0f, 0.95f);
            it.cropB = Clamp(ini.GetFloat(is, L"CropB", 0), 0.0f, 0.95f);
            it.visible = ini.GetBool(is, L"Visible", true);
            it.opacity = Clamp(ini.GetFloat(is, L"Opacity", 1.0f), 0.0f, 1.0f);
            it.volume = ini.GetFloat(is, L"Volume", 1.0f);
            it.muted = ini.GetBool(is, L"Muted", false);
            it.gateOn = ini.GetBool(is, L"GateOn", false);
            it.gateDb = ini.GetFloat(is, L"GateDb", -45.0f);
            it.compOn = ini.GetBool(is, L"CompOn", false);
            it.compDb = ini.GetFloat(is, L"CompDb", -18.0f);
            it.compRatio = ini.GetFloat(is, L"CompRatio", 4.0f);
            auto& f = it.filters;
            f.key.enabled = ini.GetBool(is, L"KeyOn", false);
            f.key.color = ParseHex(ini.GetString(is, L"KeyColor"), 0xFF00FF00);
            f.key.similarity = ini.GetFloat(is, L"KeySimilarity", 0.40f);
            f.key.smoothness = ini.GetFloat(is, L"KeySmoothness", 0.08f);
            f.key.spill = ini.GetFloat(is, L"KeySpill", 0.10f);
            f.color.enabled = ini.GetBool(is, L"ColorOn", false);
            f.color.brightness = ini.GetFloat(is, L"Brightness", 0.0f);
            f.color.contrast = ini.GetFloat(is, L"Contrast", 1.0f);
            f.color.saturation = ini.GetFloat(is, L"Saturation", 1.0f);
            f.color.gamma = ini.GetFloat(is, L"Gamma", 1.0f);
            f.color.hue = ini.GetFloat(is, L"Hue", 0.0f);
            f.sharpen = ini.GetFloat(is, L"Sharpen", 0.0f);   // Lut= / LutAmount= from older versions are ignored
            sc.items.push_back(std::move(it));
        }
        scenes.push_back(std::move(sc));
    }
    program = Clamp(ini.GetInt(c, L"Program", 0), 0, int(scenes.size()) - 1);
    preview = Clamp(ini.GetInt(c, L"Preview", 0), 0, int(scenes.size()) - 1);
    return true;
}

void SceneCollection::Save(Settings& ini) const
{
    const std::wstring c = L"Collection";
    // Remove sections of scenes/items that no longer exist.
    const int oldScenes = Clamp(ini.GetInt(c, L"Scenes", 0), 0, kMaxScenes);
    for (int i = 0; i < oldScenes; ++i) {
        const std::wstring ss = Format(L"Scene.%d", i);
        const int oldItems = Clamp(ini.GetInt(ss, L"Items", 0), 0, kMaxItems);
        for (int j = 0; j < oldItems; ++j) ini.ClearSection(Format(L"Scene.%d.Item.%d", i, j));
        ini.ClearSection(ss);
    }
    ini.SetInt(c, L"Scenes", int(scenes.size()));
    ini.SetInt(c, L"Width", int(width));
    ini.SetInt(c, L"Height", int(height));
    ini.SetInt(c, L"FpsNum", int(fpsNum));
    ini.SetInt(c, L"FpsDen", int(fpsDen));
    ini.SetInt(c, L"Program", program);
    ini.SetInt(c, L"Preview", preview);
    ini.SetInt(c, L"NextId", int(nextId));
    for (size_t i = 0; i < scenes.size(); ++i) {
        const Scene& sc = scenes[i];
        const std::wstring ss = Format(L"Scene.%zu", i);
        ini.SetString(ss, L"Name", sc.name);
        ini.SetInt(ss, L"Items", int(sc.items.size()));
        for (size_t j = 0; j < sc.items.size(); ++j) {
            const SceneItem& it = sc.items[j];
            const std::wstring is = Format(L"Scene.%zu.Item.%zu", i, j);
            ini.SetInt(is, L"Id", int(it.id));
            ini.SetString(is, L"Name", it.name);
            ini.SetInt(is, L"Kind", int(it.kind));
            if (!it.link.empty()) ini.SetString(is, L"Link", it.link);
            if (!it.deviceName.empty()) ini.SetString(is, L"DeviceName", it.deviceName);
            if (!it.path.empty()) ini.SetString(is, L"Path", it.path);
            if (it.kind == SourceKind::Text) {
                std::wstring t = it.text;
                for (size_t p; (p = t.find(L'\n')) != std::wstring::npos;) t.replace(p, 1, L"\\n");
                ini.SetString(is, L"Text", t);
                ini.SetFloat(is, L"FontSize", it.fontSize);
                ini.SetString(is, L"TextColor", Hex(it.textColor));
                ini.SetString(is, L"Font", it.fontFamily);
                ini.SetInt(is, L"FontWeight", it.fontWeight);
                ini.SetBool(is, L"Italic", it.italic);
            }
            if (it.kind == SourceKind::Color) ini.SetString(is, L"Color", Hex(it.color));
            ini.SetFloat(is, L"X", it.rect.x);
            ini.SetFloat(is, L"Y", it.rect.y);
            ini.SetFloat(is, L"W", it.rect.w);
            ini.SetFloat(is, L"H", it.rect.h);
            ini.SetFloat(is, L"CropL", it.cropL);
            ini.SetFloat(is, L"CropT", it.cropT);
            ini.SetFloat(is, L"CropR", it.cropR);
            ini.SetFloat(is, L"CropB", it.cropB);
            ini.SetBool(is, L"Visible", it.visible);
            ini.SetFloat(is, L"Opacity", it.opacity);
            ini.SetFloat(is, L"Volume", it.volume);
            ini.SetBool(is, L"Muted", it.muted);
            if (it.kind == SourceKind::Device) {
                ini.SetBool(is, L"GateOn", it.gateOn);
                ini.SetFloat(is, L"GateDb", it.gateDb);
                ini.SetBool(is, L"CompOn", it.compOn);
                ini.SetFloat(is, L"CompDb", it.compDb);
                ini.SetFloat(is, L"CompRatio", it.compRatio);
            }
            const auto& f = it.filters;
            ini.SetBool(is, L"KeyOn", f.key.enabled);
            ini.SetString(is, L"KeyColor", Hex(f.key.color));
            ini.SetFloat(is, L"KeySimilarity", f.key.similarity);
            ini.SetFloat(is, L"KeySmoothness", f.key.smoothness);
            ini.SetFloat(is, L"KeySpill", f.key.spill);
            ini.SetBool(is, L"ColorOn", f.color.enabled);
            ini.SetFloat(is, L"Brightness", f.color.brightness);
            ini.SetFloat(is, L"Contrast", f.color.contrast);
            ini.SetFloat(is, L"Saturation", f.color.saturation);
            ini.SetFloat(is, L"Gamma", f.color.gamma);
            ini.SetFloat(is, L"Hue", f.color.hue);
            ini.SetFloat(is, L"Sharpen", f.sharpen);
        }
    }
}

} // namespace cc
