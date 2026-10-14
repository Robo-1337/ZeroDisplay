#pragma once
#include <set>
#include <shobjidl.h>
#include "audio/AudioCapture.h"
#include "audio/AudioEngine.h"
#include "audio/ProgramMixer.h"
#include "core/Settings.h"
#include "devices/DeviceManager.h"
#include "devices/VideoCapture.h"
#include "encode/FileTrimmer.h"
#include "encode/Flashback.h"
#include "encode/Recorder.h"
#include "encode/ReplayBuffer.h"
#include "encode/VideoEncoder.h"
#include "net/Discovery.h"
#include "net/StreamServer.h"
#include "net/WebServer.h"
#include "ui/QrCode.h"
#include "core/Diagnostics.h"
#include "render/Compositor.h"
#include "render/FrameDelay.h"
#include "render/GpuProfiler.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "ui/Ui.h"

#include <future>
#include <optional>
#include <map>
#include <memory>
#include <thread>

namespace cc {

struct CommandLine {
    double selftestSec = 0;    // run for N seconds, log statistics, then exit
    bool cycle = false;        // during selftest: cycle through capture formats
    bool multiview = false;
    std::wstring shot;         // during selftest: save a screenshot here at the end
    std::wstring device;       // open the first device whose name contains this
    bool exclusive = false;    // force exclusive audio output
    std::wstring uiShot;       // during selftest: save the whole window (with UI) here at the end
    bool record = false;       // during selftest: record from t=2 s until the end
    bool share = false;        // share the active device on the LAN
    std::wstring connect;      // open a network stream from this address (ip[:port])
    std::wstring pin;          // sharing PIN for this run (not saved): this PC's when sharing, the one sent with --connect
    bool pinTest = false;      // testing: the sharing PIN's wrong-guess limits (PASS / FAIL in the log)
    bool advanced = false;     // start in Advanced mode (not saved)
    bool demoScene = false;    // build a demo scene for testing (not saved)
    bool studio = false;       // selftest: studio mode + a transition just before the end
    bool replay = false;       // selftest: replay buffer on, saved just before the end (not saved to settings)
    bool clean = false;        // start in clean output mode
    int gpu = -3;              // -3 = use the setting, -2 = software (WARP), -1 = automatic, >= 0 = adapter index
    bool softwareEncoder = false;   // testing: never use hardware encoders
    std::wstring mic;          // use the first microphone whose name contains this (not saved)
    bool hevc = false;         // record HEVC (not saved)
    int splitSec = 0;          // split recordings every N seconds (not saved)
    int limitSec = 0;          // stop recordings after N seconds (not saved)
    bool tracks = false;       // separate audio tracks (not saved)
    double audioSkew = 0;      // testing: sources deliver this percentage fewer frames
    bool standbyTest = false;  // selftest: simulate unplugging the active device halfway through
    bool autoCrop = false;     // auto-crop black bars (not saved)
    bool demoBlack = false;    // demo scene on a black background (tests auto-crop)
    bool flashbackTest = false;   // selftest: open Flashback at 60 % and play
    bool mini = false;            // open the pop-out mini player at start (not saved)
    int lagSim = -1;              // selftest: run the lag check with a simulated chain of N ms
    bool modeSwitch = false;      // selftest: switch Simple <-> Advanced at 35 % and back at 65 %
    double netLoss = 0;           // testing: drop this percentage of received video packets
    int streamQuality = -1;       // stream quality override (not saved): 0 smart, 1..4 fixed
    bool encoderStallTest = false;   // testing: the first encoder produces nothing (watchdog must restart it)
    int encoderSpeed = -1;           // recording encoder preset override (not saved): 0 auto, 1 fast, 2 balanced, 3 best
    int encodeHw = -1;               // encoding hardware override (not saved): 0 auto, 1 hybrid, 2 display GPU, 3 software
    UINT encodeAlign = 0;            // testing: code frame sizes rounded up to this (as some AMD encoders need)
    bool hybridSlowTest = false;     // testing: the second GPU's encoder can't keep up (Auto must move to the display GPU)
    bool displaySlowTest = false;    // testing: the display GPU's hardware encoders can't keep up either (Auto must move back)
    float settingsScroll = 0;        // testing: scroll the settings / Advanced panel this far for --ui-shot
    bool wireless = false;           // start the Miracast receiver (not saved)
    bool wirelessView = false;       // start in the Wireless tab (not saved)
    bool hotspot = false;            // start the app's own Wi-Fi network (not saved)
    int power = -1;                  // testing: -1 real, 0 plugged in, 1 on battery
    bool lowMemory = false;          // testing: act as if Windows were low on memory
    bool replayResizeTest = false;
    bool aiUpscale = false;
    int allFormatsFrom = 0;           // --all-formats-from N: start at format N
    double allFormatsDwell = 2.5;     // seconds per format
    bool allFormats = false;         // testing: every format of the device for 2.5 s each, PASS / FAIL per format          // AI upscaling on for this run (not saved)   // testing: change the replay length (10 <-> 30 s) at 60 % of the run (buffer kept)
    bool lowResource = false;        // Low-resource mode for this run (not saved)
    bool preview30 = false;          // limit the preview to 30 fps for this run (not saved)
    bool audioSim = false;           // testing: sound buffer / clock drift simulation + resampler quality (AudioSim.cpp)
    bool portalTest = false;         // testing: the welcome page's DNS parser against malformed / random packets
    bool flipLog = false;            // testing: log when the active source's picture turns bright / dark (QPC ms)
    bool recordCast = false;         // selftest: start recording when a cast shows its first picture
    bool hotspotToggleTest = false;  // selftest: the network goes off at 35 % and on at 65 % (the receiver must follow)
    bool noWireless = false;         // wireless features off for this run (not saved)
    bool wirelessToggleTest = false; // selftest: wireless features off at 35 %, on again at 65 %
    bool profileTest = false;        // selftest: change the shown device's profile, switch devices and back (AppProfiles.cpp)
    bool soundStopTest = false;      // testing: the device's sound stops arriving at 30 %, comes back at 75 %
    bool twinTest = false;           // testing: two identical "cards" (test patterns named "Twin card"); renamed at 30 %, back at 60 %
    bool testPattern = false;        // testing: offer a synthetic 1080p60 source ("Test pattern", --device "Test pattern")
    bool encodeConvertVp = false;    // testing: encoder colour conversion with the video processor, not our shader
    bool studioTest = false;         // testing (Advanced, studio): see StudioSelftest
    bool undoTest = false;           // testing (Advanced): edit, undo, redo, compare
    std::wstring demoFont;           // testing: the demo scene title in this font, bold italic
    int uiSelect = -1;               // testing (--ui-shot, Advanced): select the first source of this kind (0 device, 2 text, 3 colour)
    std::wstring multiviewDevices;   // testing: multiview opens only devices whose name contains one of these ("a|b")
    std::wstring qrTest;             // testing: log the QR code of this text (rows of 0/1)
    std::wstring superresSim;        // testing: AI upscaling as on another graphics card, "vendor|name|a.b.c.d|on|off|rejected"
    std::wstring deviceWindow;       // testing: open the device whose name contains this in its own window
    int keepTestMb = 0;              // testing: "keep recordings under" counts megabytes instead of gigabytes
    std::wstring exportSettings, importSettings;   // testing: export at the start / import while closing (no restart)
    bool resetSettings = false;                     // testing: reset everything while closing (no restart)
    bool welcomeTest = false;        // testing: show the first-start note during a self-test
    bool crashTest = false;          // testing: crash halfway through the self-test (the crash report must be written)
    bool barTest = false;            // testing: the top bar auto-hides during the self-test too; fade times are logged
    std::wstring uiSteps;            // testing: input fed to the UI, "t:click:x,y;t:move:x,y;t:key:vk" (t = seconds, client pixels)
    std::wstring mediaTest;          // testing: a video file played like a cast ("Wireless: test file")
    std::wstring profileDir;         // testing: settings folder instead of %APPDATA%\ZeroDisplay
    bool portableToggleTest = false; // testing (needs --profile-dir): turn Portable on, then off
    std::wstring trim;            // selftest: open this file in the trimmer
    int filter = -1;           // screen filter override (not saved)
    int rotate = -1;           // testing: turn every device this many quarter turns (not saved)
    int syncMs = 9999;         // testing: sync on screen for every device, ms (< 0 the picture waits; not saved)
    int hdrMode = -1;          // testing: HDR source mode for every device (0 auto, 1 off, 2 HDR10, 3 HLG; not saved)
    bool testHdr = false;      // testing: the test pattern sends 10-bit HDR10 (P010, PQ, BT.2020) with known brightness steps
    bool hdrOn = false;        // testing: the HDR features on for this run (not saved)
    bool hdrOutputTest = false;   // testing: act as if the monitor had Windows HDR on (peak 1000, SDR white 200 nits)
    bool mirror = false;       // testing: mirror every device (not saved)
    float crop[4] = {-1, -1, -1, -1};   // testing: crop every device, parts of the source "l,t,r,b" (not saved)
    int enhance = -1;          // Enhance override (not saved): 0 off, 1 sharpen, 2 upscale + sharpen
    int scale = -1;            // scaling override (not saved): 0 fit, 1 fill, 2 stretch, 3 integer
    float enhanceStrength = -1;
    int filterLines = -1;
    static CommandLine Parse();
};

// Keys travel with their modifiers (UiInput::keys): Ctrl, Shift, Alt.
inline constexpr UINT KEY_CTRL = 0x40000, KEY_SHIFT = 0x80000, KEY_ALT = 0x100000;

// Window-thread messages for device windows (AppWindows.cpp).
inline constexpr UINT WM_APP_DEVWIN_CREATE = WM_APP + 20;    // lp = DeviceWindow*
inline constexpr UINT WM_APP_DEVWIN_DESTROY = WM_APP + 21;   // lp = HWND
inline constexpr UINT WM_APP_THEME = WM_APP + 8;            // light / dark changed: restyle the title bars

inline constexpr int kMaxPictureDelayMs = 300;   // Settings ▸ Audio ▸ Sync: how long the picture may wait for late sound

// How one device's picture and sound are shown, saved in its [Device.<key>] section (AppProfiles.cpp). A device
// without saved values starts from the general [Video] / [Audio] ones (also used by the Advanced canvas).
struct DeviceProfile {
    int scale = 0;                 // ScaleMode
    int aspect = 0;                // picture shape: 0 auto, 1 4:3, 2 16:9, 3 source pixels
    int filter = 0;                // screen filter: 0 off, 1 sharp pixels, 2 scanlines, 3 CRT, 4 LCD grid
    int filterLines = 0;           // original resolution for the filter: 0 = source, else 240 / 480 / 720
    float filterStrength = 0.8f;
    bool autoCrop = false;
    int enhance = 0;               // 0 off, 1 sharpen, 2 upscale + sharpen
    float enhanceStrength = 0.5f;
    int delayMs = 0;               // sync on screen: > 0 the sound waits, < 0 the picture waits (late Bluetooth sound)
    PictureTransform xf;           // crop, mirror, turn: shown, recorded and shared like this
    HdrIn hdr;                     // HDR source: auto / off / HDR10 / HLG, white level, highlight peak
    bool operator==(const DeviceProfile&) const = default;
};

// One open capture device with its paired audio.
struct Session {
    VideoDeviceInfo info;
    std::unique_ptr<VideoSource> video;   // capture device or network stream
    VideoSurface surface;
    std::shared_ptr<AudioSource> audioSrc;
    std::unique_ptr<AudioCapture> audioCap;
    std::wstring audioChoice = L"auto";   // "auto", "none" or an endpoint id
    std::wstring audioId;                 // endpoint actually captured
    DeviceProfile prof;                   // shape, scaling, filters, Enhance, audio delay of this device
    DeviceProfile savedProf;              // what its settings section holds (written when they differ)
    FrameDelay pictureDelay;              // recent pictures while the picture waits for late sound (prof.delayMs < 0)
    bool present = true;                  // device currently enumerated
    int64_t openedQpc = 0;
    int errorRetries = 0;                 // automatic reopen attempts after errors

    // display statistics
    bool hasPending = false;
    int64_t pendingArrival = 0;
    int64_t pendingSampleTime = 0;
    double latArrivalMs = 0;
    double latCaptureMs = 0;
    bool captureLatValid = false;
    uint64_t shownFrames = 0;
    uint64_t uploadFailures = 0;
    double shownFps = 0;
    int64_t lastProbe = 0;
    int64_t blackSince = 0;   // picture has been completely black since (0 = not black)
    bool standby = false;     // showing the last good picture (signal lost / unplugged)
    int64_t lastStandbySubmit = 0;
    bool testUnplugged = false;
    bool usbWarned = false;   // "connected at USB 2 speed" shown (again after the next USB 3 connection)
    // Sound that stopped arriving (App::CheckSound): restarted once on its own, then a warning.
    int64_t audioStartQpc = 0;   // the sound capture (re)started
    int soundRestarts = 0;
    int64_t soundRestartQpc = 0;
    bool soundWarned = false;
    uint64_t pictureGen = 0;   // picture-control generation whose saved values were applied
    // auto-crop
    float crop[4] = {0, 0, 1, 1};       // applied content area (u0, v0, u1, v1)
    float cropCand[4] = {0, 0, 1, 1};   // candidate waiting for confirmation
    int cropVotes = 0;
    uint64_t barsSeen = 0;
    int64_t lastBars = 0;
    uint64_t fpsCount = 0;
    int64_t fpsStart = 0;
};

class App {
public:
    App();
    ~App();
    int Run(HINSTANCE instance, const CommandLine& cmd);

private:
    // ---- main (window) thread ----
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK MiniProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK DeviceWindowProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    struct DeviceWindow;
    LRESULT DeviceWindowProc(DeviceWindow& w, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void CreateDeviceWindow(DeviceWindow* w);   // window thread
    LRESULT MiniProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void CreateMiniWindow();   // window thread
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void ApplyFullscreen(bool on);
    void SaveWindowPlacement();

    // ---- app thread ----
    void AppThread();
    void LoadSettings();
    void SaveSettings();
    void RequestRescan(bool defaultOutputChanged);
    void PollRescan();
    void ApplyDevices(DeviceList list, bool initial);
    int DeviceIndex(const VideoDeviceInfo& info) const;
    int SessionIndex(const VideoDeviceInfo& info) const;
    Session* OpenSession(const VideoDeviceInfo& info);
    void CloseSession(size_t index);
    void SwitchTo(size_t deviceIndex);
    void SetMultiview(bool on);
    void UpdateAudioRouting(Session& s);
    void Maintain();
    Session* Active();
    size_t PickInitialDevice() const;

    void Frame();
    UiInput TakeInput();
    void HandleKeys(const UiInput& in);
    void DrawUi(const UiInput& in);
    void DrawTopBar(float alpha);
    void DrawSettings();
    void DrawStats();
    void DrawStatus(const Session* s, const RectF& area, bool compact);
    void DrawToast();
    std::vector<RectF> TileRects() const;   // one per session; casts get an empty rect (they live in the Wireless tab)
    // Casts belong to the Wireless tab (and Advanced scenes): Simple's menu, number keys and multiview leave them out.
    static bool ShownInSimple(const VideoDeviceInfo& d);
    size_t SimpleSessionCount() const;
    float BarHeight() const { return m_ui.S(52); }
    // Light / dark: follows Windows (mode 0) or the user's pick (1 light, 2 dark). The render thread switches the
    // colours at the start of a frame; the window thread restyles the title bars (StyleFrame).
    void UpdateTheme();
    void StyleFrame(HWND hwnd) const;   // dark or light title bar, Mica caption, accent border
    void Toast(const std::wstring& text);
    void ToggleFullscreen();
    void ToggleMute();
    void SetClean(bool on);   // clean output: only the picture, no controls
    void SaveScreenshot(const std::wstring& path, bool wait);
    void HandleDeviceLost();
    std::wstring StatsText(const Session& s) const;
    void SelftestTick();
    bool InStandby(const Session& s) const;
    void ApplySavedPicture(Session& s);   // re-applies saved device picture adjustments after (re)open
    void DrawPictureControls(float& y, float cx, float cw);
    void UpdateCrop(Session& s, int64_t now);
    // Flashback (AppFlashback.cpp)
    void StartFlashback(double secondsBack);
    void StopFlashback();
    void FlashbackJump(double delta);
    void UpdateFlashback();
    void DrawFlashbackBar(const UiInput& in);
    RectF FlashbackArea() const;
    void SaveFlashbackClip();
    // Pop-out mini player (AppMini.cpp)
    void SetMini(bool on);
    void RenderMini();
    // Lag check (AppLag.cpp)
    void StartLagCheck();
    void StopLagCheck(bool showResult);
    void UpdateLagCheck(Session* s, bool newFrame, int64_t frameQpc);
    void CreateFlashWindow(int monitor);   // window thread
    static LRESULT CALLBACK FlashProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    // Trim a recording (AppTrim.cpp)
    struct TrimState {
        std::wstring path;
        std::unique_ptr<TrimPreview> preview;
        double duration = 0;
        float start = 0, end = 0;
        float wantPreview = -1;
        int64_t changedAt = 0;
        ComPtr<ID3D11ShaderResourceView> srv;
        UINT imgW = 0, imgH = 0;
        bool saving = false;
    };
    struct TrimLayout { RectF card, preview; };
    void PickTrimFile();
    void OpenTrim(const std::wstring& path);
    void UpdateTrim();
    TrimLayout LayoutTrim() const;
    void DrawTrimPreview();   // GPU pass (before the UI)
    void DrawTrimDialog();
    // Live, standby or cropped picture with the session's own profile; forceScale >= 0 overrides its scaling.
    void DrawSession(const Session& s, const RectF& area, bool smooth, int forceScale = -1);
    DWORD MsUntilNextEncode() const;   // frames the encoder needs without a capture frame (canvas, standby)   // show (and encode) the last good picture instead

    // recording
    bool EnsureEncoder(Session& s);
    void ReleaseEncoderIfIdle();
    void DestroyEncoder();   // the only way to drop the encoder: recording and replay let go first
    // Sharing has its own H.264 encoder, so viewers get a bitrate that suits the network (AppStream.cpp).
    void DestroyShareEncoder();
    void SubmitFrame(const std::wstring& link, ID3D11Texture2D* tex, int64_t time100ns, int64_t qpc, ID3D11Texture2D* hdrTex = nullptr);   // to every encoder on `link`
    bool EncodesLink(const std::wstring& link) const;
    void FlushSubmits();   // after Present
    struct PendingSubmit {
        std::wstring link;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11Texture2D> hdrTex;   // the HDR picture's linear copy (HDR recordings), or null
        int64_t time = 0, qpc = 0;
    };
    std::vector<PendingSubmit> m_submits;
    void UpdateStreamRate();      // smart bitrate, once a second
    void ChangeStreamBitrate(UINT kbps);   // starts a replacement encoder in the background, swapped in when ready
    UINT StreamKbps() const { return m_shareEncoder ? m_shareEncoder->Config().bitrateKbps : 0; }
    UINT StreamStartKbps() const;
    static UINT StreamPresetKbps(int quality);
    int StreamQuality() const { return m_cmd.streamQuality >= 0 ? m_cmd.streamQuality : m_streamQuality; }
    void StartRecording();
    void StopRecording(const std::wstring& reason = L"");
    void ToggleRecording();
    void UpdateReplay();
    void SaveReplay();
    void RegisterHotkeys(bool on);   // window thread
    std::wstring RecordFolder() const;
    std::wstring ScreenshotFolder() const;   // portable mode: next to the exe
    bool SetPortableMode(bool on);           // Settings > General > Portable: moves settings.ini / scenes.ini
    static UINT BitrateKbps(int preset);
    // What the replay buffer holds in memory at its length and the recording quality (video + sound + some slack).
    double ReplayMegabytes() const { return m_replaySeconds * (BitrateKbps(m_quality) + 320.0) / 8.0 / 1000.0 * 1.1; }

    // LAN sharing
    void StartSharing();
    void StopSharing();
    void ToggleSharing();
    void UpdateSharingSource();
    void DetachSharing();
    int ShareViewers() const;
    DeviceList WithNetworkSources(const DeviceList& local) const;

    // Advanced mode (AppAdvanced.cpp)
    void SetAdvanced(bool on);
    void EnsureDefaultScene();
    void BuildDemoScene();
    void SyncSceneSessions();
    Session* SessionForItem(const SceneItem& item);
    Compositor::DeviceLookup Devices();
    void RenderProgram(int64_t now);
    RectF CanvasArea() const;
    RectF CanvasRect() const;   // where the canvas is drawn on screen
    void DrawAdvanced(const UiInput& in);
    void DrawScenePanel();
    void DrawSourcesPanel(float& y, float cx, float cw);
    // Collapsible section header (Settings, Wireless and Advanced panels); returns whether the section is open.
    bool PanelSection(const char* id, const wchar_t* title, float& y, float cx, float cw);
    // [Devices] Hidden: names of cameras / cards left out of every list (e.g. a laptop's webcam). Never opened.
    std::set<std::wstring> m_hiddenDevices;
    bool IsHidden(const VideoDeviceInfo& d) const { return m_hiddenDevices.contains(d.Key()); }   // by settings key
    void SetDeviceHidden(const std::wstring& key, bool hidden);
    // Device names (AppProfiles.cpp): the user's names from [Device.<key>] Name, and settings moved to a new key.
    void NameDevices(std::vector<VideoDeviceInfo>& video);
    std::wstring NameForKey(const std::wstring& key) const;   // also for devices that aren't connected
    void RenameDevice(const std::wstring& key, std::wstring name);   // empty = Windows' name again
    void CopyProfileTo(const std::wstring& key);   // the shown device's profile onto another device
    std::map<std::wstring, std::wstring> m_nameEdits;   // Settings ▸ Devices: text being typed, per key
    std::wstring m_reopenPending;   // "Try again" on an error: that device is opened again at the next frame
    std::wstring m_hidePending;   // picked in the device menu: hidden at the start of the next frame
    VideoDeviceInfo m_windowPending;   // picked in the device menu: opened in its own window at the next frame
    std::set<std::string> m_collapsed;   // [Ui] Collapsed: ids of collapsed sections (all open by default)
    void DrawPropertiesPanel(float& y, float cx, float cw);
    void HandleCanvasMouse(const UiInput& in);
    void AddSource(SourceKind kind, const VideoDeviceInfo* device);
    void PickImage(uint32_t itemId);   // file dialog for an image source (off the render thread)
    void ApplyPickedFiles();
    Scene* ProgramScene() { return m_scenes.Get(m_scenes.program); }
    Scene* EditScene() { return m_scenes.Get(m_studio ? m_scenes.preview : m_scenes.program); }   // what the panel and canvas edit
    void StartTransition(int toScene);
    RectF ProgramRect() const;   // studio mode: right half
    RectF EditRect() const;      // canvas being edited on screen
    SceneItem* SelectedItem();
    // Advanced editing: Delete, arrows (Shift: 10 px), Ctrl+D; canvas size / frame rate (sources scale along).
    void DeleteSelectedItem();
    void DuplicateSelectedItem();
    void NudgeSelectedItem(float dx, float dy);
    void DuplicateEditScene();
    void SetCanvas(UINT w, UINT h, UINT fps);
    void ScenesChanged()
    {
        m_scenesDirty = true;
        m_settingsDirty = true;
        m_undoPending = true;   // becomes one undo step when the gesture ends (UndoTick)
        m_lastSceneEdit = QpcNow();
    }
    // Undo / redo of scene edits (Advanced mode): snapshots of the scene collection.
    void UndoTick(const UiInput& in);
    void CommitUndo();
    void Undo();
    void Redo();
    void RestoreScenes(SceneCollection s);
    void UndoSelftest(double t);
    void StudioSelftest(double t);   // --studio-test: every transition and duration, number keys, preview edits
    bool EnsureCanvasEncoder();
    std::shared_ptr<AudioSource> ProgramAudioSource();

    // microphone commentary and the recording mix
    void UpdateMic();
    void UpdateMix();                                    // every frame: program mixer inputs
    std::shared_ptr<AudioSource> RecordAudioSource();   // what recordings/replays/sharing carry
    std::vector<std::shared_ptr<AudioSource>> RecordTracks();
    bool WantHevc() const { return m_hevc || m_cmd.hevc; }   // sharing has its own H.264 encoder
    // Recording encoder speed preset: 0 Auto, 1 Fast, 2 Balanced, 3 Best (AppStream.cpp).
    int EncoderSpeed() const { return m_cmd.encoderSpeed >= 0 ? m_cmd.encoderSpeed : m_encoderSpeed; }
    static int QvsForPreset(int p) { return p == 1 ? 0 : (p == 3 ? 100 : -1); }   // CODECAPI_AVEncCommonQualityVsSpeed, -1 = default
    static int PresetForQvs(int q) { return q == 0 ? 1 : (q >= 100 ? 3 : 2); }
    std::wstring AutoKey(const EncoderConfig& c) const;
    int DesiredPreset(const EncoderConfig& c);   // resolves Auto (learned per GPU + codec + format)
    void LearnAuto(const std::wstring& key, int preset);
    void MonitorEncoder();                       // Maintain: keeps-up check, Auto, warnings
    // Recording encoder: created on a worker thread (a GPU encoder can take a second to start, more when
    // a second GPU has to wake up) and swapped in when ready. True once m_encoder runs for `link` with
    // these settings; false while it is starting (call again) or when it failed (toast).
    bool StartEncoder(EncoderConfig cfg, const std::wstring& link);
    void PollEncoderJob();   // every frame: a finished start resumes whatever waited for it
    void DropEncoderJob();   // throws a running start away (in the background)
    // Stopping a hardware encoder can take a second (it drains): always off the render thread. Whoever
    // used it (recorder, replay buffer, viewers) must have detached already.
    void RetireEncoder(std::unique_ptr<VideoEncoder> e);
    // Multiview: every other device records its own file next to the main recording (one encoder each).
    struct ExtraRecording {
        std::wstring link, name, path;
        std::shared_ptr<AudioSource> audio;
        std::future<std::unique_ptr<VideoEncoder>> starting;
        std::unique_ptr<VideoEncoder> encoder;
        std::unique_ptr<Recorder> recorder;
    };
    std::vector<ExtraRecording> m_extraRecs;
    void StartExtraRecordings(const std::wstring& mainPath, int splitSeconds);
    void PollExtraRecordings();
    void StopExtraRecordings(const std::wstring& onlyLink = L"");
    std::optional<EncoderConfig> ConfigForLink(const std::wstring& link) const;   // the encoder fed from this source
    bool ProbeOtherGpu(bool hevc);   // true once it is known whether a second GPU can encode (looks in the background)
    EncodeTarget DisplayTarget(bool hevc) const;   // SameGpu, or Cpu when the display GPU can't encode
    // Encoding hardware (Settings > Recording): 0 Auto, 1 Hybrid, 2 Display GPU, 3 Software (AppStream.cpp).
    int EncodeHw() const
    {
        const int hw = m_cmd.softwareEncoder ? 3 : (m_cmd.encodeHw >= 0 ? m_cmd.encodeHw : m_encodeHw);
        // Battery saver / Low-resource: the discrete GPU stays asleep (unless the display GPU can't encode at all).
        if (hw <= 1 && m_cmd.encodeHw < 0 && !SecondGpuAllowed() && !m_displayNoEncoder[m_hevc ? 1 : 0]) return 2;
        return hw;
    }
    void ResolveTarget(EncoderConfig& cfg, bool forSharing);   // where this encoder should run
    bool OtherEncoderGpu(bool hevc, EncoderAdapter& out);     // a second GPU with a hardware encoder (cached)
    std::wstring HybridKey(const EncoderConfig& c) const;      // Auto: "the other GPU can't keep up" per format
    struct EncoderRequest {
        bool hevc = false;
        EncodeTarget target = EncodeTarget::SameGpu;
        int qvs = -1;
        bool operator==(const EncoderRequest&) const = default;
    };
    EncoderRequest WantedEncoder(EncoderConfig c);   // what the settings ask for, for this format
    bool EncoderUpToDate();                          // m_encoder was started for what the settings ask for now
    void CheckRecordingLimits();

    // window / threads
    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    CommandLine m_cmd;
    std::thread m_appThread;
    std::atomic<bool> m_quit{false};
    HANDLE m_wake = nullptr;         // input / resize / device change
    HANDLE m_frameEvent = nullptr;   // any capture produced a frame
    std::atomic<bool> m_resizePending{false};
    std::atomic<UINT> m_clientW{0}, m_clientH{0};
    std::atomic<UINT> m_dpi{96};
    std::atomic<bool> m_fullscreen{false};
    std::atomic<bool> m_hideCursor{false};
    WINDOWPLACEMENT m_windowedPlacement{sizeof(WINDOWPLACEMENT)};

    // input accumulated by the window thread
    struct InputAccum {
        float mx = -1, my = -1;
        bool mouseIn = false, down = false, pressed = false, released = false, dbl = false, moved = false;
        float wheel = 0;
        std::vector<UINT> keys;
        std::wstring chars;
    };
    std::mutex m_inputMutex;
    InputAccum m_input;
    bool m_trackingMouse = false;

    // subsystems
    Settings m_settings;
    DeviceManager m_deviceWatcher;
    DeviceList m_devices;
    Renderer m_renderer;
    Ui m_ui;
    AudioEngine m_audio;
    std::vector<std::unique_ptr<Session>> m_sessions;
    size_t m_active = 0;
    std::vector<std::future<void>> m_graveyard;
    std::unique_ptr<VideoEncoder> m_encoder;
    std::wstring m_encoderSymlink;   // session whose frames feed the encoder
    std::unique_ptr<Recorder> m_recorder;
    int m_headerPartsShown = 0;   // new parts already explained in a toast (Recorder::HeaderParts)
    std::unique_ptr<ReplayBuffer> m_replay;
    std::unique_ptr<Flashback> m_flash;
    VideoSurface m_flashSurface;
    double m_clipIn = -1, m_clipOut = -1;   // Flashback clip markers (seconds), -1 = not set
    uint64_t m_replayGen = 0;
    bool m_recUsesMixer = false;   // the running recording taps the mixer, so keep it running
    std::vector<MixInput> m_mixInputs;
    std::shared_ptr<AudioSource> m_micSrc;
    std::unique_ptr<AudioCapture> m_micCap;
    std::wstring m_micRunningId;
    int64_t m_lastDiskCheck = 0;
    bool m_diskWarned = false;    // "about 5 minutes of disk space left" shown for this recording
    double DiskMinutesLeft(uint64_t freeBytes) const;   // at the recording's bitrate, before the free-space stop
    UINT StreamCapKbps(UINT kbps) const { return m_profile.low ? std::min(kbps, 6000u) : kbps; }   // Low-resource: sharing at most 6 Mbps
    int m_studioTick = 0, m_keepOnlyTick = 0;

    // advanced mode
    bool m_advanced = false;
    VideoDeviceInfo m_simpleDevice;   // device shown in Simple mode before switching to Advanced
    Settings m_sceneIni;
    SceneCollection m_scenes;
    Compositor m_compositor;
    RenderTarget m_program, m_preview, m_transA, m_transB;
    bool m_studio = false;
    ProgramMixer m_mixer;
    int m_transitionMode = 1;          // 0 cut, 1 fade, 2 slide, 3 wipe
    float m_transitionMs = 300.0f;
    bool m_transitioning = false;
    int m_transitionFrom = 0;
    int64_t m_transitionStart = 0;
    uint32_t m_selectedItem = 0;
    bool m_scenesDirty = false;
    int64_t m_lastCanvasSubmit = 0;
    float m_panelScroll = 0;
    enum class Drag { None, Move, Resize } m_drag = Drag::None;
    int m_dragCorner = 0;
    float m_dragStartX = 0, m_dragStartY = 0;
    RectF m_dragStartRect;
    std::wstring m_editText;   // text being edited in the properties panel
    uint32_t m_editTextItem = 0;
    std::mutex m_pickMutex;
    struct Picked { uint32_t item; std::wstring path; };
    std::vector<Picked> m_picked;
    std::wstring m_trimPicked;   // guarded by m_pickMutex
    std::unique_ptr<TrimState> m_trim;
    // pop-out mini player
    std::atomic<HWND> m_miniHwnd{nullptr};
    std::atomic<bool> m_miniResize{false};
    std::atomic<bool> m_miniCloseRequest{false};   // closed from the mini window itself
    bool m_miniOn = false;
    OutputWindow m_miniOut;
    std::atomic<float> m_miniAspect{16.0f / 9.0f};   // the mini player keeps the picture's shape
    float m_miniOpacity = 1.0f;                      // [Mini] Opacity (0.3 .. 1)
    std::wstring m_miniPlacement;                    // [Mini] Window: "left,top,right,bottom" of the last session
    // A device in its own window (AppWindows.cpp). The list belongs to the app thread; the window thread creates /
    // destroys the windows (posted messages) and only touches the atomics, `fullscreen` and `windowed`.
    struct DeviceWindow {
        App* app = nullptr;
        std::wstring symlink, name, key;   // key: its settings section (VideoDeviceInfo::Key)
        std::wstring placement;   // saved "left,top,right,bottom,maximized"
        std::atomic<HWND> hwnd{nullptr};
        std::atomic<bool> resize{false}, closeRequest{false};
        OutputWindow out;
        bool fullscreen = false;
        WINDOWPLACEMENT windowed{};
        uint64_t lastFrames = ~0ull;
        int64_t lastDrawn = 0;
        bool shown = false;   // logged the first picture
    };
    std::vector<std::unique_ptr<DeviceWindow>> m_deviceWindows, m_closedWindows;
    bool HasDeviceWindow(const std::wstring& symlink) const;
    void OpenDeviceWindow(const VideoDeviceInfo& d);
    void CloseDeviceWindow(size_t i, bool remember);   // remember: opened again at the next start (app exit)
    void RenderDeviceWindows();
    // lag check
    struct LagCheck {
        int flashes = 0;               // flashes shown so far
        bool white = false;            // flash showing
        int64_t phaseStart = 0;
        bool measured = false;         // this flash was seen
        std::vector<double> results;   // ms, flash on screen -> frame arrived
        int missed = 0;
    };
    std::unique_ptr<LagCheck> m_lag;
    std::atomic<HWND> m_flashHwnd{nullptr};
    std::atomic<bool> m_flashWhite{false};
    std::atomic<int64_t> m_flashShownQpc{0};   // when the latest white flash reached the screen
    std::wstring m_lagResult;
    std::unique_ptr<StreamServer> m_server;
    std::unique_ptr<WebServer> m_web;
    // Sharing PIN ([Network] ViewerPinOn / ViewerPin, encrypted for this Windows user): the stream and the browser
    // viewer are only sent to viewers who give it. PINs of other PCs this one watched: [NetworkPins] (also encrypted).
    net::PinGate m_pinGate;
    bool m_viewerPinOn = true;
    std::string m_viewerPin;
    std::wstring m_pinEntry;   // status card: the PIN being typed for a network stream
    void SetViewerPin(bool on, const std::string& pin);   // saved; viewers with an old PIN are asked again
    void ApplyViewerPin();                                // the gate gets this run's PIN (--pin wins)
    std::wstring NetPinKey(const VideoDeviceInfo& info) const;   // per sharing PC (its name, else its address)
    std::string NetPinFor(const VideoDeviceInfo& info);
    void RememberNetPin(const VideoDeviceInfo& info, const std::string& pin);
    uint64_t m_encoderGen = 0, m_sharedGen = 0;   // m_sharedGen: which share-encoder instance the servers use
    std::unique_ptr<VideoEncoder> m_shareEncoder;
    std::wstring m_shareSymlink;   // what it encodes ("#canvas" in Advanced mode)
    uint64_t m_shareEncGen = 0;
    int m_streamQuality = 0;       // 0 smart, 1..4 = 4 / 8 / 15 / 25 Mbps
    UINT m_smartKbps = 0;          // smart mode's current choice
    int64_t m_lastRateCheck = 0, m_rateHoldUntil = 0, m_rateCooldown = 0;
    std::future<std::unique_ptr<VideoEncoder>> m_shareNext;   // replacement at a new bitrate, started off-thread
    std::wstring m_shareNextLink;
    std::future<std::unique_ptr<VideoEncoder>> m_shareStart;   // first encoder for a sharing source, started off-thread
    std::wstring m_shareStartLink;
    std::wstring m_rateReason;     // why smart mode last changed (shown in settings)
    Discovery m_discovery;
    DeviceList m_localDevices;                 // without network sources
    std::vector<VideoDeviceInfo> m_manualNet;  // streams opened by address

    // rescans
    std::atomic<int64_t> m_rescanAt{0};
    std::atomic<bool> m_defaultOutChanged{false};
    std::future<DeviceList> m_enumFuture;
    int64_t m_lastMaintain = 0;

    // preferences
    ScaleMode m_scale = ScaleMode::Fit;
    bool m_smooth = true;
    int m_enhance = 0;                 // [Video] Enhance: 0 off, 1 sharpen, 2 upscale + sharpen (screen only)
    bool m_aiUpscale = false;
    int m_aspect = 0;                  // [Video] Aspect (picture shape): 0 auto, 1 4:3, 2 16:9, 3 source pixels
    // Shape the picture is shown with (width / height): SD frames (720 / 704 × 480 / 576 …) are 4:3 TV pictures in
    // Auto; everything else has square pixels.
    double DisplayAspect(UINT w, UINT h) const { return DisplayAspect(w, h, m_aspect); }
    static double DisplayAspect(UINT w, UINT h, int aspect);
    // Shape of a w×h picture from this surface (cropped / turned): the source's pixel shape carries over.
    static double ShownAspect(const VideoSurface& sf, UINT w, UINT h, int aspect);
    PictureTransform m_xf;            // crop / mirror / turn of the shown device (part of its profile)
    HdrIn m_hdr;                      // HDR handling of the shown device (part of its profile)
    // [Video] HdrEnabled: the HDR features (experimental, off by default). Off: no HDR screen output, no HDR10
    // recordings, no forced HDR10 / HLG source modes, default brightness; sources that flag HDR themselves are still
    // tone-mapped to SDR. The per-device values are kept. --hdr-on, --hdr-output-test and --hdr-mode turn it on for their
    // run (--test-hdr alone doesn't: it checks the tone mapping with the switch off).
    bool m_hdrEnabled = false;
    bool HdrOn() const { return m_hdrEnabled || m_cmd.hdrOn || m_cmd.hdrOutputTest || m_cmd.hdrMode >= 0; }
    HdrIn EffectiveHdr(const HdrIn& h) const { return HdrOn() ? h : HdrIn{}; }
    bool m_hdrScreen = true;          // [Video] HdrScreen: real HDR on monitors with Windows HDR on
    Renderer::HdrDisplay m_hdrDisplayInfo;   // the window's monitor (UpdateHdrOutput)
    void UpdateHdrOutput(bool displayChanged);
    bool m_hdrScreenFailed = false;   // the graphics driver refused HDR output
    bool m_recordHdr = true;          // [Recording] Hdr: HDR sources are recorded as HDR10 (HEVC 10-bit)
    bool m_encoderHdrAsked = false;   // the recording encoder was started for HDR (it may have fallen back to SDR)
    bool WantHdrRecording(const Session& s) const
    {
        return HdrOn() && m_recordHdr && !m_advanced && s.surface.hasImage && s.surface.transfer != Transfer::Sdr;
    }
    bool EncodesHdr(const std::wstring& link) const;
    void DrawHdrSettings(float& y, float cx, float cw);   // Settings ▸ HDR (experimental)
    static const wchar_t* TransferName(Transfer t) { return t == Transfer::Pq ? L"HDR10 (PQ)" : (t == Transfer::Hlg ? L"HLG" : L"SDR"); }
    std::wstring m_xfChangedLink;     // a device's crop / turn changed: its recording encoder follows the new size
    int64_t m_xfChangedQpc = 0;
    bool TransformLocked(const Session& s) const;   // being recorded: the picture size can't change
    void DrawTransformSettings(float& y, float cx, float cw);
    // Crop by dragging on the picture (AppCrop.cpp).
    bool m_cropEdit = false;
    int m_cropDrag = -1;                // screen edge being dragged (0 left, 1 top, 2 right, 3 bottom)
    PictureTransform m_cropBefore;      // restored by Cancel / Esc
    bool CanCropOnPicture(std::wstring* why);
    void BeginCropEdit();
    void EndCropEdit(bool keep);
    RectF CropEditRect(const Session& s) const;
    void DrawCropEditor();
    // Per-device profiles (AppProfiles.cpp). The members above (m_scale, m_aspect, filters, Enhance, m_delayMs) are
    // the profile of the device shown in Simple mode (what Settings edits), or the general one in Advanced mode.
    DeviceProfile m_generalProfile;   // [Video] / [Audio]: the Advanced canvas, and where new devices start
    std::wstring m_profileKey;        // device whose profile the members hold (empty = general)
    DeviceProfile CurrentProfile() const;
    void UseProfile(const DeviceProfile& p);
    DeviceProfile LoadProfile(const std::wstring& section) const;
    void SaveProfile(const std::wstring& section, const DeviceProfile& p);
    void SyncProfile();
    void SaveProfiles();
    void ProfileSelftest(double t);
    void CheckSound(Session& s, int64_t now);   // every 2 s (Maintain)
    double SoundSilentMs(const Session& s, int64_t now) const;   // since the device last delivered sound packets
    void TwinSelftest(double t);
          // [Video] AiUpscale: the GPU vendor's video super resolution (screen only)
    float m_enhanceStrength = 0.5f;    // [Video] EnhanceStrength
    ColorMatrix m_matrix = ColorMatrix::Auto;
    ColorRange m_range = ColorRange::Auto;
    bool m_preferUncompressed = true;
    bool m_vsync = false;
    bool m_standbyOn = true;        // keep the last picture when the signal is lost
    bool m_standbyOnBlack = true;   // ... also when the picture stays black (this card's "no HDMI" output)
    bool m_autoCrop = false;        // zoom past black bars (display only)
    int m_screenFilter = 0;         // 0 off, 1 sharp pixels, 2 scanlines, 3 CRT, 4 LCD grid
    int m_filterLines = 0;          // original resolution for scanlines etc.: 0 = source, else 240 / 480 / ...
    float m_filterStrength = 0.8f;
    int m_gpuChoice = -1;   // -1 automatic, -2 software, >= 0 adapter index
    bool m_recreateRenderer = false;
    std::vector<std::wstring> m_adapters;
    std::vector<std::wstring> m_fontFamilies;   // installed font families (Advanced text sources), enumerated on first use
    const std::vector<std::wstring>& FontFamilies();
    std::wstring m_outputSetting;   // empty = default device
    float m_volume = 1.0f;
    bool m_muted = false;
    bool m_passthrough = true;
    bool m_exclusiveAudio = false;
    int m_latency = int(AudioLatency::Low);
    int m_delayMs = 0;
    // The sound output is a Bluetooth device (checked when the output changes): its delay isn't in Windows' numbers.
    bool OutputIsBluetooth();
    std::wstring m_btCheckedId;
    bool m_btOutput = false;
    bool m_multiview = false;
    bool m_autoSwitch = true;
    bool m_startFullscreen = false;
    bool m_showStats = false;
    int m_quality = 2;               // bitrate preset index
    std::wstring m_recordFolderSetting;   // empty = Videos\ZeroDisplay
    // Settings ▸ Files (AppFiles.cpp)
    std::wstring m_shotFolderSetting;     // [Screenshots] Folder, empty = Pictures\ZeroDisplay
    std::wstring m_nameTemplate;          // [Recording] NameTemplate: {type} {device} {date} {time}
    static const wchar_t* const kDefaultNameTemplate;
    int m_shotFormat = 0;                 // [Screenshots] Format: 0 PNG, 1 JPG
    bool m_shotClipboard = false;         // [Screenshots] Clipboard
    bool m_shotFlash = true;              // [Screenshots] Flash
    std::atomic<int> m_themeMode{0};      // [General] Theme: 0 follow Windows, 1 light, 2 dark
    std::atomic<bool> m_systemDark{true}; // Windows' app mode, refreshed on WM_SETTINGCHANGE
    std::atomic<bool> m_themeDark{true};  // the colours in use
    std::unordered_map<std::string, float> m_cardHeights;   // Settings: each open section's card, as tall as last frame
    int64_t m_shotFlashQpc = 0;           // the screen flashes white for a moment (screenshot taken)
    int m_minFreeGb = 1;                  // [Recording] MinFreeGb: recording stops below this much free space
    std::atomic<int> m_keepGb{0};         // [Recording] KeepGb: 0 off, else oldest app-made files to the Recycle Bin above it
    int m_keepGbAsked = -1;               // Settings: the size picked, waiting for "yes" (it deletes files)
    std::pair<int, std::wstring> m_folderPicked;   // 0 recordings / 1 screenshots, guarded by m_pickMutex
    std::mutex m_madeMutex;
    uint64_t MinFreeBytes() const { return uint64_t(m_minFreeGb) << 30; }
    static std::wstring ExpandNameTemplate(const std::wstring& tmpl, const std::wstring& type, const std::wstring& device, const SYSTEMTIME& st);
    std::wstring NewFilePath(const std::wstring& folder, const wchar_t* type, const wchar_t* ext);
    void PickFolder(int which);
    void ApplyPickedFolder();
    std::wstring MadeFilesIndex() const;
    void RememberMadeFile(const std::wstring& path);
    void EnforceKeepLimit();   // worker thread
    void CopyImageToClipboard(UINT w, UINT h, const std::vector<uint8_t>& bgra);
    bool m_shareOnStart = false;
    bool m_replayOn = false;
    float m_replaySeconds = 30.0f;
    bool m_hevc = false;
    int m_encoderSpeed = 0;           // 0 auto, 1 fast, 2 balanced (encoder default), 3 best
    int m_encodeHw = 0;               // 0 auto, 1 hybrid, 2 display GPU, 3 software
    EncoderRequest m_encoderRequest;  // what m_encoder was started for (fallbacks don't count as a change)
    bool m_hybridBroken = false;      // the other GPU's encoder failed to start this session
    bool m_displayNoEncoder[2] = {};  // [hevc]: the display GPU turned out to have no usable hardware encoder
    bool m_hybridWarned = false;
    std::future<void> m_otherProbe;   // looking for a second GPU in the background
    // Wireless streaming (AppWireless.cpp): the Miracast receiver and the devices casting to it.
    std::unique_ptr<class MiracastSink> m_miracast;
    std::future<std::unique_ptr<class MiracastSink>> m_receiverStart;
    bool m_wirelessView = false;   // the Wireless mode tab
    // [General] Wireless: master switch. Off: no cast receiver, own Wi-Fi, sharing, discovery or network sources,
    // nothing listens on the network. The choices below are kept and come back when it is turned on.
    bool m_wireless = true;
    bool m_wirelessForced = false;   // a command-line flag decided it for this run (not saved)
    void SetWireless(bool on);
    bool m_receiverOn = false;     // [Wireless] Receiver: off until the user opens the tab or turns it on
    bool m_castPin = false;        // [Wireless] Pin
    bool m_receiverPinUsed = false, m_receiverStartPin = false;
    // Wi-Fi Direct does one thing at a time on most adapters: the receiver steps aside while the app's own
    // network starts, then tries again next to it ("paused" if the adapter can't do both).
    bool m_receiverUnderHotspot = false, m_receiverStartHotspot = false, m_receiverRestart = false;
    std::future<void> m_receiverStopping;
    struct Cast {
        std::wstring key, name;
        ComPtr<IUnknown> source;   // Windows.Media.Core.MediaSource
    };
    std::vector<Cast> m_casts;
    std::wstring m_castPinShown;
    bool m_castFirewallBlocked = false;   // a cast connected but no picture came in (sticky until one works)
    VideoDeviceInfo m_beforeCast;         // what Simple showed before a cast took over
    float m_wirelessScroll = 0;
    float m_flipLo = 1, m_flipHi = 0;   // --flip-log
    bool m_flipBright = false;
    // The app's own Wi-Fi network and its welcome page (W2/W3); nothing runs until the user turns it on.
    std::unique_ptr<class Hotspot> m_hotspot;
    std::future<bool> m_hotspotStart;
    std::unique_ptr<class CaptivePortal> m_portal;
    bool m_hotspotOn = false;                    // [Wireless] Hotspot
    std::wstring m_hotspotSsid, m_hotspotPass;   // [Wireless] HotspotName / HotspotPassword (the app's own network)
    std::wstring m_hotspotAddr;                  // the PC on the hotspot network ("" until known)
    int m_hotspotClients = 0;
    int64_t m_hotspotPolled = 0;
    std::future<void> m_hotspotStopping;         // the previous network going down (a new one waits for it)
    bool m_showHsPass = false;   // own Wi-Fi: password and join code shown (hidden by default; not saved)
    std::wstring m_hsEditSsid, m_hsEditPass;     // the name / password boxes
    std::wstring m_portalTried;                  // hotspot address the welcome page couldn't start on
    mutable std::wstring m_lanIp;
    mutable int64_t m_lanIpAt = 0;
    std::future<bool> m_fwCheck;
    int m_fwPublic = -1;                         // Windows Firewall lets ZeroDisplay in on public networks: -1 unknown
    void CheckFirewall();
    struct QrCache {
        std::string text;
        QrCode code;
        const QrCode& For(const std::string& t);
    } m_qrWifi, m_qrViewer, m_qrWelcome;
    void UpdateHotspot();
    void StopHotspot(bool wait);
    std::wstring ViewerUrl(bool hotspot) const;
    RectF WirelessArea() const;
    std::wstring ReceiverName() const;
    Session* ActiveCast();
    bool KeepOpen(const Session& s) const;   // a connected cast: its stream must be read or the device drops it
    void SetWirelessView(bool on);
    void UpdateWireless();
    void CastEnded(const std::wstring& key, const std::wstring& name, bool toast);
    ComPtr<IUnknown> CastMediaSource(const std::wstring& link) const;
    void AllowCastingThroughFirewall();
    bool CastBlockedByFirewall() const;
    void DrawWireless(const UiInput& in);
    ResourceProbe m_probe;   // self-test: CPU per thread and memory
    // UI layer: redrawn only when something changed (input, animation, toast) or every m_uiRefreshMs for live numbers.
    int64_t m_lastUiDraw = 0;
    float m_lastUiMx = -2, m_lastUiMy = -2;
    bool m_lastUiMouseIn = false;
    std::atomic<bool> m_uiDirty{true};
    double m_uiRefreshMs = 250;
    uint64_t m_uiDraws = 0, m_frames = 0;
    double m_uiDrawMsTotal = 0, m_uiDrawMsMax = 0;
    bool UiNeedsDraw(const UiInput& in, int64_t now);
    // Power & memory profiles (AppPower.cpp).
    struct PowerProfile {
        bool battery = false, memory = false, low = false;
    };
    PowerProfile m_profile;
    bool m_profileApplied = false;
    int m_batterySaver = 1;     // [General] BatterySaver: 0 off, 1 on battery, 2 always
    int m_memorySaver = 1;      // [General] MemorySaver: 0 off, 1 when Windows runs low, 2 always
    bool m_lowResource = false; // [General] LowResource
    bool m_preview30 = false;   // [Video] Preview30
    bool m_onBattery = false, m_lowMemory = false, m_timer1ms = false, m_trimmedWhileHidden = false, m_perfHintShown = false;
    int m_slowSeconds = 0;
    int64_t m_profilePolled = 0, m_lastPresentQpc = 0;
    bool m_capSkip = false;
    bool m_swapSlot = false;
    bool m_fastSource = false;          // a source faster than the screen: presented without waiting for the refresh
    UINT m_displayHz = 60;              // refresh rate of the window's monitor (re-read every second: games / settings change it)
    int64_t m_displayHzChecked = 0;
    std::atomic<bool> m_srRecheck{false};   // back from another app / power source changed: check AI upscaling again
    std::atomic<int64_t> m_displayChangedQpc{0};   // WM_DISPLAYCHANGE: resolution / refresh / orientation of a screen changed
    int m_behindStreak = 0;                        // consecutive 2 s windows the recording encoder fell behind
    // Auto moved the recording from the second GPU to the display GPU: checked before it's remembered. If the display GPU
    // falls behind too (even on Fast), the recording goes back to the second GPU once; a minute of keeping up saves it.
    bool m_hybridMoved = false;
    bool m_hybridMovedBack = false;   // this recording went back already: no more moves
    EncoderConfig m_hybridFrom;       // the second GPU's encoder, to go back to
    int64_t m_hybridMovedQpc = 0;
    void UpdateDisplayHz();
    // What the window's monitor is doing (refresh rate, HDR, SDR brightness), read once a second and on display changes
    // by its own thread: these Windows calls can take tens of ms on some PCs and must never hold up a frame.
    struct DisplayInfo {
        UINT width = 0, height = 0, hz = 0;
        Renderer::HdrDisplay hdr;
        uint64_t seq = 0;
    };
    std::thread m_displayThread;
    HANDLE m_displayWake = nullptr;
    std::atomic<bool> m_displayQuit{false};
    std::mutex m_displayMutex;
    DisplayInfo m_displayInfo;       // guarded by m_displayMutex
    uint64_t m_displaySeen = 0;      // render thread: the last m_displayInfo.seq applied
    void DisplayWatch();
    bool SourceFasterThanScreen() const;            // waited for the swap chain and haven't presented since
    uint64_t m_unchangedFrames = 0;     // loop passes with nothing new to show (not drawn)
    int m_presentCount = 0;
    int64_t m_presentFpsStart = 0;
    double m_presentFps = 0;     // this frame isn't shown (30 fps limit)
    HANDLE m_lowMemEvent = nullptr;
    void UpdateProfile();
    // Keep awake: a console player never touches the PC, so Windows would turn the screen off / sleep mid-game.
    bool m_keepAwake = true;          // [General] KeepAwake
    HANDLE m_powerRequest = nullptr;  // PowerCreateRequest, shown in "powercfg /requests"
    bool m_awakeDisplay = false, m_awakeSystem = false;
    void UpdateKeepAwake();
    void ApplyProfile();
    void SetTimerResolution(bool fine);
    std::wstring ProfileSummary() const;
    std::vector<uint32_t> ScenesInUse() const;
    void CheckWeakPc();
    void CheckCrashReport();
    bool ReplayAllowed() const { return !m_profile.battery && !m_profile.memory && !m_profile.low; }
    bool SecondGpuAllowed() const { return !m_profile.battery && !m_profile.low; }
    bool EnhanceAllowed() const { return !m_profile.battery && !m_profile.memory && !m_profile.low; }
    bool FiltersAllowed() const { return !m_profile.low; }
    bool PreviewCapped() const { return m_preview30 || m_cmd.preview30 || m_profile.low; }
    int ScreenFilter() const { return FiltersAllowed() ? m_screenFilter : 0; }
    int ScreenFilter(const DeviceProfile& p) const { return FiltersAllowed() ? p.filter : 0; }
    int EnhanceMode(const DeviceProfile& p) const { return m_smooth && ScreenFilter(p) == 0 && !m_renderer.Warp() && EnhanceAllowed() ? p.enhance : 0; }
    // Minimized or completely covered: nothing is drawn or presented; capture, recording, replay, sharing and the
    // mini player carry on.
    bool m_hidden = false;
    int64_t m_hiddenCheck = 0;
    uint64_t m_hiddenFrames = 0;
    void InvalidateUi() { m_uiDirty = true; }
    GpuProfiler m_gpuProf;   // F3 / self-test: GPU time per frame section
    bool m_profiling = false;
    std::vector<SceneCollection> m_undo, m_redo;
    SceneCollection m_undoBase;   // the scenes as of the last undo step
    bool m_undoInit = false, m_undoPending = false;
    int64_t m_lastSceneEdit = 0;
    struct EncoderJob {
        std::unique_ptr<VideoEncoder> encoder;
        size_t index = 0;   // which candidate started
    };
    std::future<EncoderJob> m_encoderNext;      // StartEncoder: running start
    std::wstring m_encoderNextLink;
    EncoderConfig m_encoderNextBase;
    std::vector<EncoderConfig> m_encoderNextTry;   // candidates in order (other GPU, display GPU, H.264 ...)
    EncoderRequest m_encoderNextRequest;
    bool m_recordPending = false;               // F9 pressed, the encoder is starting
    std::map<std::wstring, int> m_autoLearned;   // Auto: preset that keeps up, per GPU + codec + format
    bool m_autoCalibrating = false;
    int64_t m_autoSince = 0;
    uint64_t m_lastSubmitted = 0, m_lastEmitted = 0;   // recording encoder keep-up monitor
    bool m_encoderBehind = false;
    bool m_behindWarned = false;
    int m_splitMinutes = 0;        // 0 = one file
    int m_limitMinutes = 0;        // 0 = no limit
    int m_stopNoSignalMinutes = 0; // [Recording] StopNoSignal: stop after this long without a signal (0 = never)
    int64_t m_noSignalSince = 0;   // the recorded source lost its signal (0 = it has one)
    bool m_noSignalWarned = false;
    bool m_separateTracks = false;
    std::wstring m_micId;          // commentary microphone endpoint; empty = off
    float m_micGain = 1.0f;
    bool m_micGate = true;
    bool m_micComp = true;
    bool m_globalHotkeys = true;
    bool m_hotkeysRegistered = false;
    std::atomic<int> m_hotkeysFailed{0};   // global hotkeys another app already owns (shown in Settings)
    // Settings ▸ Hotkeys (AppHotkeys.cpp): a key in the window and a global one per action ([Hotkeys] <Action>, <Action>Global).
    struct HotkeyAction {
        const wchar_t* key;    // settings key
        const wchar_t* name;   // shown
        UINT local, global;    // defaults (vk | KEY_CTRL / KEY_ALT / KEY_SHIFT, 0 = none)
    };
    static constexpr int kHotkeyCount = 10;
    static const HotkeyAction kHotkeyActions[kHotkeyCount];
    std::atomic<UINT> m_hotkeyLocal[kHotkeyCount] = {}, m_hotkeyGlobal[kHotkeyCount] = {};
    std::atomic<uint32_t> m_hotkeyTaken{0};   // bit i: action i's global key is owned by another app
    int m_hotkeyCapture = -1;                 // waiting for a key: action (+ kHotkeyCount for its global key)
    std::atomic<bool> m_hotkeyCapturing{false};
    static std::wstring KeyName(UINT combo);
    std::wstring KeyHint(int action) const;   // " (F9)" for tooltips, "" when it has no key
    void LoadHotkeys();
    void SaveHotkeys();
    void RunHotkey(int action);
    void TogglePauseRecording();
    // AppHelp.cpp
    bool m_showHelp = false;      // F1: every key
    bool m_welcomeDone = false;   // [General] WelcomeDone: the first-start note was seen
    int64_t m_welcomeQpc = 0;
    void DrawHelp(const UiInput& in);
    void DrawWelcome(const UiInput& in);
    bool m_micMuted = false;   // [Audio] MicMuted: the microphone is left out of recordings / sharing
    void ToggleMicMute();
    float m_micLevel = 0;   // shown in Settings ▸ Audio (peak, decays)
    bool HandleHotkeyCapture(const std::vector<UINT>& keys);
    void DrawHotkeyRows(float& y, float cx, float cw);
    // AppSettingsIo.cpp: export / import / reset everything (import and reset apply while closing, then a restart)
    enum class OnExit { None, Reset, Import };
    std::atomic<OnExit> m_onExit{OnExit::None};
    std::atomic<bool> m_restartAfterExit{false};
    std::wstring m_importPath;    // confirmed import
    std::wstring m_importAsked;   // picked, waiting for "yes"
    bool m_resetAsked = false;
    std::pair<int, std::wstring> m_settingsFilePicked;   // 1 export to / 2 import from, guarded by m_pickMutex
    void PickSettingsFile(bool exportIt);
    void ApplyPickedSettingsFile();
    bool ExportSettings(const std::wstring& path);
    void ApplySettingsChangeOnExit();
    void RequestRestart(OnExit what);
    // AppCues.cpp
    enum class Cue { Start, Stop, Saved };
    bool m_soundCue = false;                   // [Recording] SoundCue
    std::atomic<bool> m_askBeforeClose{true};  // [General] AskBeforeClose
    std::atomic<uint32_t> m_closeRisk{0};      // 1 recording, 2 viewers, 4 unsaved replay footage
    std::atomic<int> m_closeViewers{0};
    int64_t m_lastReplaySave = 0;
    void PlayCue(Cue cue);
    void UpdateCloseRisk();
    bool ConfirmClose();   // window thread
    // AppTaskbar.cpp: a red dot on the taskbar button and "REC" in the title while recording (window thread).
    ComPtr<ITaskbarList3> m_taskbar;
    int m_taskbarState = 0;   // app thread: last state posted (0 none, 1 recording, 2 paused)
    std::atomic<int> m_taskbarShown{0};
    void ShowRecordingOnTaskbar(int state);
    std::wstring PictureShapeHint();       // Settings ▸ Video ▸ Picture shape: what the active source looks like now
    // Creates the folder and checks that a file can be written there; otherwise says so (toast) and returns false.
    bool EnsureFolder(const std::wstring& folder, const wchar_t* what);
    std::wstring DiskFreeHint() const;     // free space where recordings go (checked at most every 5 s)
    mutable double m_diskFreeGb = -1;
    mutable int64_t m_diskFreeAt = 0;
    bool m_settingsDirty = false;
    int64_t m_lastSave = 0;

    // ui state
    int64_t m_startQpc = 0;
    int64_t m_lastFrameQpc = 0;
    int64_t m_lastCaptureQpc = 0;   // a capture frame was uploaded (Frame)
    int64_t m_lastMoveQpc = 0;
    float m_barAlpha = 1.0f;
    // Top-bar auto-hide runs on real time (the UI is drawn only when something changes): the fade starts exactly
    // 2.5 s after the last mouse move, the loop wakes for that moment, and fades take the same time at any fps.
    int64_t m_barAnimQpc = 0;      // last animation step
    bool m_barCanHide = false;     // nothing holds the bar (menu, Settings, Advanced, no picture, mouse over it)
    int64_t BarHideAt() const { return m_lastMoveQpc + QpcFrequency() * 5 / 2; }
    bool m_settingsOpen = false;
    bool m_clean = false;   // clean output mode (not saved)
    float m_settingsScroll = 0;
    float m_meter[2] = {0, 0};
    // Toasts: up to three at the bottom; one with a file offers "Show in folder" / "Open" and stays longer. `key`
    // replaces an earlier toast with the same key (e.g. the volume), the same text just stays longer.
    struct ToastItem {
        std::wstring text, file, key;
        int64_t until = 0;
    };
    std::mutex m_toastMutex;
    std::deque<ToastItem> m_toasts;
    std::atomic<int64_t> m_toastUntil{0};   // the latest end of any toast (redraw / wake-ups)
    void ToastFile(const std::wstring& text, const std::wstring& file);
    void Toast(const std::wstring& text, const std::wstring& key);
    bool m_screenshotRequested = false;
    double m_presentMs = 0;
    double m_uiMs = 0;
    std::wstring m_statsText;
    int64_t m_statsAt = 0;

    // selftest
    size_t m_cycleStep = 0;
    bool m_uiShotNow = false;
    bool m_uiShotTaken = false;
    std::wstring m_stepShot;   // testing (--ui-steps "t:shot:N"): the window with its interface, saved during the next frame
    bool m_statsBeforeShot = false;
    bool m_selftestDone = false;
};

} // namespace cc
