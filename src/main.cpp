#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/config/values/types/Vec2Value.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/SharedDefs.hpp>

#include <cairo/cairo.h>
#include <filesystem>
#include <wordexp.h>

inline HANDLE g_pHandle = nullptr;

namespace cv = Config::Values;

static SP<cv::CVec2Value>   g_cvSize;
static SP<cv::CVec2Value>   g_cvPosition;
static SP<cv::CVec2Value>   g_cvOrigin;
static SP<cv::CColorValue>  g_cvColor;
static SP<cv::CFloatValue>  g_cvRounding;
static SP<cv::CStringValue> g_cvImage;

static CHyprSignalListener  g_listenerActive;
static CHyprSignalListener  g_listenerRenderStage;

static bool                 g_hasFocus = true;

static SP<Render::ITexture> g_texture;
static std::string          g_loadedImagePath;

static std::string          expandPath(const std::string& in) {
    if (in.empty())
        return in;
    wordexp_t p;
    if (wordexp(in.c_str(), &p, 0) != 0)
        return in;
    std::string out = (p.we_wordc > 0) ? p.we_wordv[0] : in;
    wordfree(&p);
    return out;
}

static void loadImageIfNeeded(const std::string& path) {
    if (path == g_loadedImagePath)
        return;

    g_loadedImagePath = path;
    g_texture.reset();

    if (path.empty())
        return;

    const auto resolved = expandPath(path);
    if (!std::filesystem::exists(resolved)) {
        HyprlandAPI::addNotification(g_pHandle, "[hyprspot] image not found: " + resolved, CHyprColor{1.0, 0.2, 0.2, 1.0}, 3000);
        return;
    }

    cairo_surface_t* surface = cairo_image_surface_create_from_png(resolved.c_str());
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        HyprlandAPI::addNotification(g_pHandle, "[hyprspot] failed to load png: " + resolved, CHyprColor{1.0, 0.2, 0.2, 1.0}, 3000);
        return;
    }

    g_texture = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
}

static void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_LAST_MOMENT)
        return;

    if (!g_hasFocus)
        return;

    auto pMonitor = g_pHyprRenderer->renderData().pMonitor.lock();
    if (!pMonitor)
        return;

    const auto vSize = g_cvSize->value();
    const auto vPos  = g_cvPosition->value();
    const auto vOrig = g_cvOrigin->value();

    loadImageIfNeeded(g_cvImage->value());

    const Vector2D size{vSize.x, vSize.y};
    const Vector2D pos{vPos.x, vPos.y};
    const Vector2D orig{vOrig.x, vOrig.y};

    const Vector2D monSize = pMonitor->m_size;
    const Vector2D anchor  = monSize * Vector2D{(pos.x + 1.0) * 0.5, (pos.y + 1.0) * 0.5};
    const Vector2D offset  = size * Vector2D{(orig.x + 1.0) * 0.5, (orig.y + 1.0) * 0.5};
    const Vector2D topLeft = anchor - offset;

    CBox box{topLeft.x, topLeft.y, size.x, size.y};

    if (g_texture) {
        CTexPassElement::SRenderData data;
        data.tex   = g_texture;
        data.box   = box;
        data.a     = 1.0f;
        data.round = static_cast<int>(g_cvRounding->value());
        g_pHyprRenderer->draw(data);
    } else {
        CRectPassElement::SRectData data;
        data.box   = box;
        data.color = CHyprColor{static_cast<uint64_t>(g_cvColor->value())};
        data.round = static_cast<int>(g_cvRounding->value());
        g_pHyprRenderer->draw(data);
    }

    g_pHyprRenderer->damageBox(box);
}

static void onActiveWindow(PHLWINDOW window, Desktop::eFocusReason reason) {
    g_hasFocus = window != nullptr;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// Force emission of the inline `__hyprland_api_get_client_hash()` so Hyprland
// can dlsym it on plugin load to verify ABI compatibility. The actual symbol is
// declared in PluginAPI.hpp with default visibility; we just need at least one
// non-inlined reference to make the linker keep it.
APICALL EXPORT const char* hyprspot_get_client_hash() {
    return __hyprland_api_get_client_hash();
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pHandle = handle;

    const std::string HASH = __hyprland_api_get_hash();
    if (HASH != GIT_COMMIT_HASH) {
        HyprlandAPI::addNotification(handle, "[hyprspot] mismatched hyprland version!", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprspot] version mismatch");
    }

    g_cvSize     = makeShared<cv::CVec2Value>("plugin:hyprspot:size", "Dot size (w, h) in logical pixels", Config::VEC2{8.f, 8.f});
    g_cvPosition = makeShared<cv::CVec2Value>("plugin:hyprspot:position", "Screen position in [-1,1]^2 (-1,-1 = top-left)", Config::VEC2{1.f, 1.f});
    g_cvOrigin   = makeShared<cv::CVec2Value>("plugin:hyprspot:origin", "Pivot inside the dot in [-1,1]^2", Config::VEC2{1.f, 1.f});
    g_cvColor    = makeShared<cv::CColorValue>("plugin:hyprspot:color", "Dot color (RGBA hex 0xRRGGBBAA)", Config::INTEGER{0xFFFFFFFF});
    g_cvRounding = makeShared<cv::CFloatValue>("plugin:hyprspot:rounding", "Corner rounding in pixels", 0.0f);
    g_cvImage    = makeShared<cv::CStringValue>("plugin:hyprspot:image", "Path to a PNG to draw instead of the dot (empty = solid color)", std::string{""});

    HyprlandAPI::addConfigValueV2(handle, g_cvSize);
    HyprlandAPI::addConfigValueV2(handle, g_cvPosition);
    HyprlandAPI::addConfigValueV2(handle, g_cvOrigin);
    HyprlandAPI::addConfigValueV2(handle, g_cvColor);
    HyprlandAPI::addConfigValueV2(handle, g_cvRounding);
    HyprlandAPI::addConfigValueV2(handle, g_cvImage);

    g_listenerActive      = Event::bus()->m_events.window.active.listen(onActiveWindow);
    g_listenerRenderStage = Event::bus()->m_events.render.stage.listen(onRenderStage);

    HyprlandAPI::reloadConfig();

    return {"hyprspot", "Focus indicator dot/PNG in a screen corner", "whax", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listenerActive.reset();
    g_listenerRenderStage.reset();
    g_texture.reset();
    g_loadedImagePath.clear();
    g_cvSize.reset();
    g_cvPosition.reset();
    g_cvOrigin.reset();
    g_cvColor.reset();
    g_cvRounding.reset();
    g_cvImage.reset();
}
