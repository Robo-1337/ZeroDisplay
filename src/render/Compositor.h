#pragma once
#include "render/Renderer.h"
#include "scene/Scene.h"

#include <wincodec.h>

#include <functional>
#include <map>

namespace cc {

// Renders scenes into render targets on the GPU. Owns the per-item resources that aren't video:
// image textures (WIC) and text textures (Direct2D/DirectWrite).
class Compositor {
public:
    struct DeviceFrame {
        ID3D11ShaderResourceView* srv = nullptr;
        UINT width = 0, height = 0;
    };
    using DeviceLookup = std::function<bool(const SceneItem&, DeviceFrame&)>;

    bool Init(Renderer& renderer);
    void Reset();   // drop GPU resources (renderer re-created)

    void Render(RenderTarget& target, const Scene& scene, const DeviceLookup& devices, float backgroundGray = 0.0f);
    // Native size of an item's content (video, image, rendered text); false if not known yet.
    bool ContentSize(const SceneItem& item, const DeviceLookup& devices, UINT& w, UINT& h);
    void Forget(uint32_t itemId);   // item deleted: free its resources
    void KeepOnly(const std::vector<uint32_t>& itemIds);   // Memory saver: free images / text of items not shown (reloaded when shown)

private:
    struct Texture {
        std::wstring key;   // path or text parameters the texture was made from
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        UINT width = 0, height = 0;
        bool failed = false;
    };

    Texture* ImageFor(const SceneItem& item);
    Texture* TextFor(const SceneItem& item);
    bool LoadImage(const std::wstring& path, Texture& out);
    bool RenderText(const SceneItem& item, Texture& out);

    Renderer* m_renderer = nullptr;
    ComPtr<ID2D1DeviceContext> m_dc;
    ComPtr<IWICImagingFactory> m_wic;
    std::map<uint32_t, Texture> m_images, m_texts;
};

} // namespace cc
