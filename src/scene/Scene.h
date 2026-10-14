#pragma once
#include "core/Common.h"
#include "core/Settings.h"

namespace cc {

enum class SourceKind { Device = 0, Image = 1, Text = 2, Color = 3 };

struct ChromaKey {
    bool enabled = false;
    uint32_t color = 0xFF00FF00;   // ARGB key colour (green)
    float similarity = 0.40f;      // 0..1
    float smoothness = 0.08f;      // 0..1
    float spill = 0.10f;           // 0..1
    bool operator==(const ChromaKey&) const = default;
};

struct ColorAdjust {
    bool enabled = false;
    float brightness = 0.0f;   // -1..1
    float contrast = 1.0f;     // 0..2
    float saturation = 1.0f;   // 0..3
    float gamma = 1.0f;        // 0.2..3
    float hue = 0.0f;          // degrees, -180..180
    bool operator==(const ColorAdjust&) const = default;
};

struct ItemFilters {
    ChromaKey key;
    ColorAdjust color;
    float sharpen = 0.0f;        // 0..1
    bool operator==(const ItemFilters&) const = default;
};

// One layer in a scene. Rect is in canvas pixels; crop is a fraction of the source on each side.
struct SceneItem {
    uint32_t id = 0;
    std::wstring name;
    SourceKind kind = SourceKind::Device;
    std::wstring link;         // device symlink (Device)
    std::wstring deviceName;   // the device's settings key (VideoDeviceInfo::Key), to find it again if its symlink changes
    std::wstring path;         // Image
    std::wstring text;         // Text
    float fontSize = 72.0f;
    uint32_t textColor = 0xFFFFFFFF;
    std::wstring fontFamily = L"Segoe UI";
    int fontWeight = 600;      // DWRITE_FONT_WEIGHT: 400 regular, 600 semibold, 700 bold
    bool italic = false;
    uint32_t color = 0xFF1E2430;   // Color source, ARGB
    RectF rect;
    float cropL = 0, cropT = 0, cropR = 0, cropB = 0;
    bool visible = true;
    float opacity = 1.0f;
    float volume = 1.0f;   // device audio in the program mix
    bool muted = false;
    bool gateOn = false;          // noise gate (program mix only)
    float gateDb = -45.0f;
    bool compOn = false;          // compressor (program mix only)
    float compDb = -18.0f;
    float compRatio = 4.0f;
    ItemFilters filters;
    bool operator==(const SceneItem&) const = default;
};

struct Scene {
    std::wstring name;
    std::vector<SceneItem> items;
    bool operator==(const Scene&) const = default;
};

struct SceneCollection {
    // Limits (also for a damaged scenes.ini: a count of a million would take minutes to read).
    static constexpr int kMaxScenes = 200, kMaxItems = 200;
    UINT width = 1920, height = 1080;
    UINT fpsNum = 60, fpsDen = 1;
    std::vector<Scene> scenes;
    int program = 0;   // scene on air
    int preview = 0;   // scene being edited (studio mode)
    uint32_t nextId = 1;

    bool Load(Settings& ini);
    void Save(Settings& ini) const;

    Scene* Get(int index) { return index >= 0 && index < int(scenes.size()) ? &scenes[size_t(index)] : nullptr; }
    SceneItem* Find(Scene& s, uint32_t id);
    SceneItem& Add(Scene& s, SourceKind kind, const std::wstring& name);
    // Fits a source of the given size into the canvas (letterboxed, centred).
    RectF FitToCanvas(float srcW, float srcH) const;
};

const wchar_t* SourceKindName(SourceKind k);

} // namespace cc
