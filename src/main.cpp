#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/config/values/types/Vec2Value.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/SharedDefs.hpp>

#include <cairo/cairo.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <wordexp.h>

inline HANDLE g_pHandle = nullptr;

// Force emission of the inline __hyprland_api_get_client_hash so Hyprland can
// dlsym it on plugin load for ABI verification.
APICALL EXPORT const char* hyprspot_get_client_hash() {
    return __hyprland_api_get_client_hash();
}

namespace cv = Config::Values;

static SP<cv::CVec2Value>   g_cvSize;
static SP<cv::CStringValue> g_cvAnchor;
static SP<cv::CVec2Value>   g_cvPosition;
static SP<cv::CVec2Value>   g_cvOrigin;
static SP<cv::CVec2Value>   g_cvOffset;
static SP<cv::CColorValue>  g_cvColor;
static SP<cv::CFloatValue>  g_cvRounding;
static SP<cv::CStringValue> g_cvImage;

// --- Focus pulse (transient ring on focus change) ---
static SP<cv::CFloatValue> g_cvPulseDuration;  // ms, 0 disables
static SP<cv::CColorValue> g_cvPulseColor;
static SP<cv::CFloatValue> g_cvPulseThickness; // px
static SP<cv::CFloatValue> g_cvPulseExpand;    // px the ring grows outward over its life
static SP<cv::CFloatValue> g_cvPulseRounding;  // px

static PHLWINDOWREF                          g_pulseWindow;
static std::chrono::steady_clock::time_point g_pulseStart;

// Returns {position, origin} for a named anchor, or std::nullopt if name is empty/unknown.
static std::optional<std::pair<Vector2D, Vector2D>> anchorFromName(const std::string& name) {
    static const std::unordered_map<std::string, Vector2D> NAMED = {
        {"top-left",      {-1.0, -1.0}}, {"top",          { 0.0, -1.0}}, {"top-center",    { 0.0, -1.0}}, {"top-right",   { 1.0, -1.0}},
        {"left",          {-1.0,  0.0}}, {"center-left",  {-1.0,  0.0}}, {"center",        { 0.0,  0.0}}, {"middle",      { 0.0,  0.0}},
        {"right",         { 1.0,  0.0}}, {"center-right", { 1.0,  0.0}},
        {"bottom-left",   {-1.0,  1.0}}, {"bottom",       { 0.0,  1.0}}, {"bottom-center", { 0.0,  1.0}}, {"bottom-right",{ 1.0,  1.0}},
    };
    if (name.empty())
        return std::nullopt;
    auto it = NAMED.find(name);
    if (it == NAMED.end())
        return std::nullopt;
    return std::make_pair(it->second, it->second);
}

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
    if (!std::filesystem::exists(resolved))
        return;

    cairo_surface_t* surface = cairo_image_surface_create_from_png(resolved.c_str());
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return;
    }

    g_texture = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
}

// Compute the dot box anchored to the focused window's geometry.
// If pMonitor is provided, the box is returned in MONITOR-LOCAL coords (what
// the render pass expects). Otherwise it's in GLOBAL coords (used by damageBox).
static CBox computeBox(PHLWINDOW pWindow, PHLMONITOR pMonitor = nullptr) {
    const auto     vSize    = g_cvSize->value();
    const auto     vPadding = g_cvOffset->value();
    const Vector2D size{vSize.x, vSize.y};
    const Vector2D padding{vPadding.x, vPadding.y};

    Vector2D pos, orig;
    if (auto named = anchorFromName(g_cvAnchor->value())) {
        pos  = named->first;
        orig = named->second;
    } else {
        const auto vPos  = g_cvPosition->value();
        const auto vOrig = g_cvOrigin->value();
        pos              = {vPos.x, vPos.y};
        orig             = {vOrig.x, vOrig.y};
    }

    Vector2D       winPos  = pWindow->m_realPosition->value();
    const Vector2D winSize = pWindow->m_realSize->value();

    if (pMonitor)
        winPos = winPos - pMonitor->m_position;

    const Vector2D anchor{winPos.x + winSize.x * (pos.x + 1.0) * 0.5,
                          winPos.y + winSize.y * (pos.y + 1.0) * 0.5};
    const Vector2D pivotOffset{size.x * (orig.x + 1.0) * 0.5,
                               size.y * (orig.y + 1.0) * 0.5};

    // Padding pushes the dot inward, away from whichever edge it's anchored to.
    // pos.x == -1 (left)  => +padding.x ; pos.x == +1 (right) => -padding.x.
    const Vector2D inset{-pos.x * padding.x, -pos.y * padding.y};

    const Vector2D topLeft = anchor - pivotOffset + inset;
    return CBox{topLeft.x, topLeft.y, size.x, size.y};
}

class CSpotDecoration : public IHyprWindowDecoration {
  public:
    CSpotDecoration(PHLWINDOW window) : IHyprWindowDecoration(window), m_window(window) {}
    virtual ~CSpotDecoration() = default;

    virtual SDecorationPositioningInfo getPositioningInfo() override {
        SDecorationPositioningInfo info;
        info.policy         = DECORATION_POSITION_ABSOLUTE;
        info.edges          = 0;
        info.priority       = 10;
        info.desiredExtents = SBoxExtents{};
        info.reserved       = false;
        return info;
    }

    virtual void onPositioningReply(const SDecorationPositioningReply&) override {}

    virtual void draw(PHLMONITOR pMonitor, float const& a) override {
        auto pWindow = m_window.lock();
        if (!pWindow || !pMonitor)
            return;
        if (!g_pCompositor->isWindowActive(pWindow))
            return;

        loadImageIfNeeded(g_cvImage->value());

        const CBox box   = computeBox(pWindow, pMonitor);
        const int  round = static_cast<int>(g_cvRounding->value());

        if (g_texture) {
            CTexPassElement::SRenderData data;
            data.tex   = g_texture;
            data.box   = box;
            data.a     = a;
            data.round = round;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
        } else {
            CRectPassElement::SRectData data;
            data.box   = box;
            data.color = CHyprColor{static_cast<uint64_t>(g_cvColor->value())}.modifyA(a);
            data.round = round;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
        }

        drawPulse(pWindow, pMonitor, a);
    }

    // Transient ring that expands outward and fades over pulse_duration ms,
    // retriggered every time this window becomes focused. Self-damages each
    // frame so the animation keeps ticking without Hyprland's anim system.
    static void drawPulse(PHLWINDOW pWindow, PHLMONITOR pMonitor, float a) {
        const float durMs = g_cvPulseDuration->value();
        if (durMs <= 0.f)
            return;

        auto pulsed = g_pulseWindow.lock();
        if (!pulsed || pulsed != pWindow)
            return;

        const auto  now     = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float, std::milli>(now - g_pulseStart).count();
        const float p       = elapsed / durMs;
        if (p >= 1.f)
            return;

        const float ease  = 1.f - p;
        const float alpha = ease * ease;                 // quadratic fade-out
        const float grow  = g_cvPulseExpand->value() * p; // expand outward over life

        const Vector2D winPos  = pWindow->m_realPosition->value() - pMonitor->m_position;
        const Vector2D winSize = pWindow->m_realSize->value();

        CBorderPassElement::SBorderData bd;
        bd.box        = CBox{winPos.x - grow, winPos.y - grow, winSize.x + 2 * grow, winSize.y + 2 * grow};
        bd.grad1      = Config::CGradientValueData(CHyprColor{static_cast<uint64_t>(g_cvPulseColor->value())});
        bd.a          = alpha * a;
        bd.borderSize = static_cast<int>(g_cvPulseThickness->value());
        bd.round      = static_cast<int>(g_cvPulseRounding->value() + grow);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(bd));

        // Keep the animation alive until alpha reaches 0.
        g_pHyprRenderer->damageMonitor(pMonitor);
    }

    virtual eDecorationType  getDecorationType() override { return DECORATION_CUSTOM; }
    virtual eDecorationLayer getDecorationLayer() override { return DECORATION_LAYER_OVERLAY; }
    virtual uint64_t         getDecorationFlags() override { return DECORATION_NON_SOLID; }
    virtual std::string      getDisplayName() override { return "Spot Indicator"; }
    virtual void             updateWindow(PHLWINDOW) override {}

    virtual void damageEntire() override {
        auto pWindow = m_window.lock();
        if (!pWindow)
            return;
        g_pHyprRenderer->damageBox(computeBox(pWindow));
    }

  private:
    PHLWINDOWREF m_window;
};

static CHyprSignalListener g_listenerOpen;
static CHyprSignalListener g_listenerActive;

static void attachDecoration(PHLWINDOW window) {
    if (!window)
        return;
    HyprlandAPI::addWindowDecoration(g_pHandle, window, makeUnique<CSpotDecoration>(window));
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pHandle = handle;

    g_cvSize     = makeShared<cv::CVec2Value>("plugin:hyprspot:size", "Dot size (w, h) in logical pixels", Config::VEC2{12.f, 12.f});
    g_cvAnchor   = makeShared<cv::CStringValue>("plugin:hyprspot:anchor", "Named corner: top-left, top, top-right, left, center, right, bottom-left, bottom, bottom-right. Overrides position+origin when set.", std::string{""});
    g_cvPosition = makeShared<cv::CVec2Value>("plugin:hyprspot:position", "(advanced) Position inside the focused window in [-1,1]^2 (-1,-1 = top-left)", Config::VEC2{1.f, 1.f});
    g_cvOrigin   = makeShared<cv::CVec2Value>("plugin:hyprspot:origin", "(advanced) Pivot inside the dot in [-1,1]^2", Config::VEC2{1.f, 1.f});
    g_cvOffset   = makeShared<cv::CVec2Value>("plugin:hyprspot:offset", "Padding (px) from the anchored edges, towards the window center", Config::VEC2{0.f, 0.f});
    g_cvColor    = makeShared<cv::CColorValue>("plugin:hyprspot:color", "Dot color (hex 0xAARRGGBB)", Config::INTEGER{0xFFFFFFFF});
    g_cvRounding = makeShared<cv::CFloatValue>("plugin:hyprspot:rounding", "Corner rounding in pixels", 6.0f);
    g_cvImage    = makeShared<cv::CStringValue>("plugin:hyprspot:image", "Path to a PNG to draw instead of the dot (empty = solid color)", std::string{""});

    g_cvPulseDuration  = makeShared<cv::CFloatValue>("plugin:hyprspot:pulse_duration", "Focus pulse duration in ms (0 disables the pulse)", 400.0f);
    g_cvPulseColor     = makeShared<cv::CColorValue>("plugin:hyprspot:pulse_color", "Focus pulse ring color (hex 0xAARRGGBB)", Config::INTEGER{0xFFAB66FF});
    g_cvPulseThickness = makeShared<cv::CFloatValue>("plugin:hyprspot:pulse_thickness", "Focus pulse ring thickness in px", 3.0f);
    g_cvPulseExpand    = makeShared<cv::CFloatValue>("plugin:hyprspot:pulse_expand", "How far the ring expands outward over its life, in px", 12.0f);
    g_cvPulseRounding  = makeShared<cv::CFloatValue>("plugin:hyprspot:pulse_rounding", "Base ring corner rounding in px", 10.0f);

    HyprlandAPI::addConfigValueV2(handle, g_cvSize);
    HyprlandAPI::addConfigValueV2(handle, g_cvAnchor);
    HyprlandAPI::addConfigValueV2(handle, g_cvPosition);
    HyprlandAPI::addConfigValueV2(handle, g_cvOrigin);
    HyprlandAPI::addConfigValueV2(handle, g_cvOffset);
    HyprlandAPI::addConfigValueV2(handle, g_cvColor);
    HyprlandAPI::addConfigValueV2(handle, g_cvRounding);
    HyprlandAPI::addConfigValueV2(handle, g_cvImage);
    HyprlandAPI::addConfigValueV2(handle, g_cvPulseDuration);
    HyprlandAPI::addConfigValueV2(handle, g_cvPulseColor);
    HyprlandAPI::addConfigValueV2(handle, g_cvPulseThickness);
    HyprlandAPI::addConfigValueV2(handle, g_cvPulseExpand);
    HyprlandAPI::addConfigValueV2(handle, g_cvPulseRounding);

    g_listenerOpen = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) {
        attachDecoration(w);
    });

    g_listenerActive = Event::bus()->m_events.window.active.listen([](PHLWINDOW w, Desktop::eFocusReason) {
        g_pulseWindow = w;
        g_pulseStart  = std::chrono::steady_clock::now();
        if (g_pCompositor) {
            for (auto& m : g_pCompositor->m_monitors)
                g_pHyprRenderer->damageMonitor(m);
        }
    });

    if (g_pCompositor) {
        for (const auto& w : g_pCompositor->m_windows)
            attachDecoration(w);
    }

    HyprlandAPI::reloadConfig();

    return {"hyprspot", "Focus indicator dot/PNG in the corner of the focused window", "whax", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listenerOpen.reset();
    g_listenerActive.reset();
    g_texture.reset();
    g_loadedImagePath.clear();
    g_cvSize.reset();
    g_cvAnchor.reset();
    g_cvPosition.reset();
    g_cvOrigin.reset();
    g_cvOffset.reset();
    g_cvColor.reset();
    g_cvRounding.reset();
    g_cvImage.reset();
    g_cvPulseDuration.reset();
    g_cvPulseColor.reset();
    g_cvPulseThickness.reset();
    g_cvPulseExpand.reset();
    g_cvPulseRounding.reset();
    g_pulseWindow.reset();
}
