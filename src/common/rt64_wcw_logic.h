//
// RT64
//

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <array>
#include <algorithm>
#include <deque>
#include <memory>

// [wcw] Decision logic for frame interpolation, present pacing, the CRT beam simulator and ray-traced shadows, kept
// free of renderer state so it can be unit tested (tests/wcw). The renderer calls these functions; a change to any
// of these rules belongs here, with a test.
namespace RT64::WcwLogic {
    // Ray-traced shadows

    // Other scenes stay shadow casters for this many game frames after their last build. Counted in game frames,
    // not workloads: frame interpolation renders every workload once per displayed frame (4 times per game frame
    // at 120 fps), which aged the wrestlers' scene out before the floor's pass used it.
    constexpr uint64_t ShadowCasterStaleFrames = 2;

    // Whether a scene last built on game frame builtFrame still casts shadows on game frame frameCounter.
    bool shadowCasterFresh(uint64_t frameCounter, uint64_t builtFrame);

    // Ray tracer texture memory (aki-roadmap#72). Each scene slot keeps one RGBA16F texture: its resolved history
    // (shadow RGB, AO; a miss holds the miss factor with AO < 0), which the composite also reads and encodes. The
    // resolve writes a shared scratch and copies the region it wrote back into the slot. Shared beside it: the
    // trace's current sample (RGBA16F factor and AO) and hit distance (R32F).
    constexpr uint64_t RtSlotTexelBytes = 8;
    constexpr uint64_t RtSharedTexelBytes = 8 + 8 + 4;
    // Before #72: per slot two RGBA16F histories and an RGBA16F output; shared an unused RGBA16F output, the RGBA32F
    // current sample and the R16F AO. For the WCW_GPU_LOG comparison and the tests.
    constexpr uint64_t RtSlotTexelBytesBefore72 = 8 + 8 + 8;
    constexpr uint64_t RtSharedTexelBytesBefore72 = 8 + 16 + 2;

    // GPU bytes of the ray tracer's trace-resolution textures with allocatedSlots slots holding a history.
    uint64_t rtTextureBytes(uint32_t traceWidth, uint32_t traceHeight, uint32_t allocatedSlots, bool before72 = false);

    // A slot whose scene wasn't built for this many game frames gives its textures and bottom-level structure back
    // (a menu, an entrance or another venue's scene). Long past ShadowCasterStaleFrames, so a slot that can still
    // cast shadows is never released.
    constexpr uint64_t RtSlotIdleFrames = 300;

    // Whether a slot's textures are released before the next workload: idle (see RtSlotIdleFrames), or sized for
    // another trace resolution and no longer a fresh shadow caster (its next trace would recreate them anyway; until
    // then they'd hold the old size). A slot in use keeps them: passes onto targets of two sizes in one frame would
    // otherwise recreate them every frame and never keep a history.
    bool releaseSlotTextures(uint64_t frameCounter, uint64_t builtFrame, bool sizeMatches);

    // The trace texels the resolve writes for a traced rect (x0, y0, x1, y1; exclusive ends): whole 8x8 thread
    // groups from the rect's origin, clipped to the trace texture. This is the region copied back into the slot.
    void resolveRegion(const uint32_t rect[4], uint32_t traceWidth, uint32_t traceHeight, uint32_t region[4]);

    // What a workload's trace pass covers on screen.
    enum class TraceArea {
        // The whole color target: the pass that darkens the misses (crowd and background, never in the traced scene).
        FullScreen,
        // Everything the pass's geometry covers: with outside darkness, its surfaces beyond the receiver radius
        // darken too. Its misses keep factor 1, so tracing them is wasted work.
        Geometry,
        // Only the geometry within the receiver radius: nothing beyond it gets shadows, AO or darkness.
        Receivers
    };

    // The trace area for a pass. Tracing every pass full screen whenever the venue had outside darkness made each
    // No Mercy arena frame trace its 5-7 passes over the whole target (Quality at 5760x2160: ~48 ms of GPU per
    // game frame), which slowed the game itself, at 60/120 fps most of all.
    TraceArea traceArea(bool darkenMisses, float outsideDarkness);

    // Frame interpolation renders every workload once per displayed frame: 2 times per game frame at 60 fps, 4 at
    // 120. Tracing each of them multiplied the ray tracing cost by that much (No Mercy arenas at Quality fell to ~6
    // presents/s and slowed the game itself), so only the real frame (the game frame's last, current weight 1)
    // traces; a generated frame composites its pass's latest trace, at most half a game frame old at 60 fps.
    // A pass traces anyway when it has no trace yet, or after MaxGeneratedReuse generated frames in a row (the real
    // frame was skipped under load), so shadows never freeze.
    constexpr uint32_t MaxGeneratedReuse = 3;

    // The reuse limit for a display rate (aki-roadmap#165): about one and a half game frames of generated frames, at
    // least MaxGeneratedReuse. A fixed 3 was 1.5 game frames at 60 fps but only 0.75 at 120 (3 generated + 1 real
    // per game frame): a game frame a quarter late made the generated frames trace at full cost, which slowed the game
    // more, which made more generated frames trace (RT Ultra at 120 fps fell to ~7 presents/s). framesPerGameFrame:
    // WcwLogic::framesPerGameFrame's (2 at 60 fps, 4 at 120); 3 at 60, 6 at 120, 7 at 144.
    uint32_t maxGeneratedReuse(uint32_t framesPerGameFrame);

    // Whether a frame of curFrameWeight (1 = real, below 1 = generated by interpolation) is generated.
    bool generatedFrame(float curFrameWeight);

    // Whether a pass traces on this render. generated: the displayed frame is generated (generatedFrame);
    // haveTrace: the pass's slot holds a trace of this same pass (see passSignature); reusesSinceTrace: generated
    // frames that reused it since.
    bool traceRender(bool generated, bool haveTrace, uint32_t reusesSinceTrace, uint32_t maxReuse = MaxGeneratedReuse);

    // aki-roadmap#171: a framebuffer pass that only fills rectangles, after a pass of the same workload drew another colour
    // buffer, is the game clearing its next back buffer. Revenge (triple-buffered) clears the next buffer that way at the
    // end of each frame; when the VI still shows that buffer for one more present, presenting it showed a black frame.
    bool trailingClear(bool fillRectOnly, bool earlierPassDrewOtherBuffer);

    // Keep the image already on screen instead of presenting the VI's buffer: only when that buffer's last write was a
    // trailing clear, something was presented before, the previous present wasn't kept too (a false positive costs at
    // most one frame, never a frozen screen) and the present carries no framebuffer operations (a CPU write to the
    // buffer, which the trailing-clear flag doesn't see). A fade to black drawn on the shown buffer itself isn't a
    // trailing clear, so it still shows.
    bool keepPreviousImage(bool shownBufferTrailingClear, bool presentedBefore, bool keptLast, bool cpuWrites);

    // A pass's identity: its draws' triangle counts, in any order. A generated frame reuses the trace of the
    // previous game frame's real frame, and No Mercy sorts the wrestlers' draws again every game frame: with the
    // order in the signature the wrestler pass (the most expensive trace) never matched and traced twice per game
    // frame at 60/120 fps (aki-roadmap#86). Two passes can share a slot (similar scenes); reusing by slot alone
    // composited one pass's trace for both, darkening generated frames ~5%.
    uint64_t passSignature(const uint32_t *triangleCounts, size_t count);

    // Whether a slot's trace can stand in for this pass on a generated frame: the slot holds a trace (traced) of
    // this pass (same signature) that made the same outside darkness choice (WcwLogic::MissDarkening, decided for
    // every pass on every target as if it traced). A reused arena pass that didn't count as the target's darkening
    // pass let a second pass darken the misses again: generated frames ~5% darker, 3% of game frames off.
    bool reusableTrace(bool traced, uint64_t tracedSignature, uint64_t signature, bool tracedDarkened, bool darkens);

    // Whether a pass builds its acceleration structures (BLAS + TLAS) on this render (aki-roadmap#36). A pass that
    // composites its slot's latest trace (traceRender false) reads neither, yet every generated frame built them:
    // ~0.1 ms each, over half the workloads at 120 fps. It still builds when it traces every frame
    // (traceEveryFrame: WCW_RT_TRACE_GENERATED), or when its trace textures are made again on this render
    // (traceTexturesChange: target size or trace scale changed), which drops the trace it would composite.
    bool buildForPass(bool traceEveryFrame, bool generated, bool haveTrace, uint32_t reusesSinceTrace, bool traceTexturesChange,
        uint32_t maxReuse = MaxGeneratedReuse);

    // What a pass's latest trace was made with: the ray tracer's trace textures and the slot's history.
    struct TraceTextures {
        // Ray tracing also ran on the previous render (re-enabled after being off, every history is stale).
        bool active = false;
        bool haveTextures = false;
        // The color target size and trace scale the textures were made for.
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        float traceScale = 0.0f;
        bool haveHistory = false;
        uint32_t historyWidth = 0;
        uint32_t historyHeight = 0;
        // The mode (WCW_RT_MODE_*) that wrote the history: shadows and AO, or a debug view's color.
        uint32_t historyMode = UINT32_MAX;
    };

    // Whether recordTrace makes the trace textures or the slot's history again, or drops the history, for a pass
    // into a target of targetWidth x targetHeight at targetScale in mode: the latest trace can't be composited then
    // (buildForPass's traceTexturesChange). A changed mode (the Lighting editor's debug views) and a changed trace
    // scale (an RT quality change between recordBuild and recordTrace) count, as do the target size and the
    // trace size the history was made at.
    bool traceTexturesChange(const TraceTextures &textures, uint32_t targetWidth, uint32_t targetHeight, float targetScale, uint32_t mode);

    // Ray tracing quality presets

    // The menu's Ray Tracing Quality levels.
    enum class RtQuality : uint32_t {
        Performance = 0,
        Balanced = 1,
        Quality = 2,
        Ultra = 3
    };

    // Ray-traced ambient occlusion

    // The AO looks. There is no AO menu: each quality level picks one (RtPreset::ao): none on Performance, Subtle on
    // Balanced, Strong on Quality and Ultra.
    enum class AoLevel : uint32_t {
        Off = 0,
        Subtle = 1,
        Strong = 2
    };

    struct AoSettings {
        // Cosine-weighted occlusion rays per pixel per frame (0 = no AO).
        uint32_t samples = 0;
        // How much a fully occluded pixel darkens, on the world and on characters (wrestlers).
        float strength = 0.0f;
        float characterStrength = 0.0f;
        // Ray length: occluders further away don't count, nearer ones count more (1 - (t / radius)^2).
        float radius = 0.0f;
        float characterRadius = 0.0f;
        // Contrast: the occluded fraction is multiplied by this (clamped to 1) before darkening.
        float gain = 1.0f;
        float characterGain = 1.0f;
    };

    // WCW_RT_AO_CYCLE: AO on for periodMs, off for periodMs, repeating, phased to wall-clock time (Unix ms) so a
    // screen recording can be labelled from its own start time. Even periods are on.
    bool aoCycleOn(uint64_t unixMs, uint64_t periodMs);

    // The AO parameters for a look with this many rays per pixel per frame. The look (strengths, radii, gains)
    // depends on the level alone; Off traces no rays whatever the count.
    AoSettings aoLookSettings(AoLevel level, uint32_t samples);

    // What a Ray Tracing Quality level traces. Performance and Balanced trace at reduced resolution (soft,
    // temporally resolved shadows don't need every pixel), capped at maxLines so 4K targets don't multiply the
    // rays; Quality traces every pixel up to 1440 lines (so at 4K it traces two thirds of the lines), and Ultra
    // traces every pixel at any size. Higher levels add rays, and the AO look comes with the level.
    struct RtPreset {
        // Trace resolution relative to the color target, before the line cap.
        float scale = 1.0f;
        // Cap on the trace height in lines (0 = none).
        float maxLines = 0.0f;
        // Jittered shadow rays per light per pixel per frame (the key light gets more by its weight).
        uint32_t samplesPerLight = 1;
        AoLevel ao = AoLevel::Off;
        uint32_t aoSamples = 0;
    };

    // The preset for a quality level; levels past Ultra clamp to Ultra.
    RtPreset rtPreset(uint32_t quality);

    // The trace resolution relative to a color target targetHeight lines tall. A supersampled target (rendered at
    // downsampleMultiplier times the output size, then box-filtered down) traces at no more than the output size:
    // the soft, temporally resolved shadows gain nothing from the extra samples, and they'd cost 4x the rays.
    float rtTraceScale(const RtPreset &preset, uint32_t targetHeight, uint32_t downsampleMultiplier = 1);

    // Resolution

    // The render scale at Auto (WindowIntegerScale) resolution: the smallest whole multiple of referenceHeight that
    // covers the window, times downsampleMultiplier (Supersampling: render that much larger and box-filter it back
    // down, so the presented image still fits the window). A window not created yet counts as 1x.
    float windowResolutionMultiplier(uint32_t windowHeight, uint32_t referenceHeight, uint32_t downsampleMultiplier);

    // The AO parameters for a quality level.
    AoSettings aoSettings(uint32_t quality);

    // The resolve pass's cleanup of the traced shadow and AO terms, which it filters and accumulates separately.
    // Shadows have real edges (a wrestler's outline on the mat), so they get a tight spatial filter and react
    // quickly; AO is soft by nature and the noisier term (few rays in every direction), so it gets a wide filter
    // and a longer history. Radii are in trace pixels (1 = 3x3); alpha is the weight of this frame's value
    // against the reprojected history.
    struct DenoiseSettings {
        uint32_t shadowRadius = 1;
        uint32_t aoRadius = 3;
        float shadowAlpha = 0.1f;
        float aoAlpha = 0.06f;
    };

    // The resolve filter reads up to this many pixels around each pixel; the trace rect's margin must cover it.
    constexpr uint32_t DenoiseMaxRadius = 4;

    DenoiseSettings denoiseSettings();

    // Wrestler sheen: highlights and backlight

    // TV wrestling is lit for sheen: skin, oil and leather catch the arena lights, and the lights rim the edges of
    // the wrestlers that face them, outlining them against the dark crowd. The ray tracer adds both to wrestlers (character draws) from the same
    // lights and shadow rays as the shadows, so a blocked light gives no highlight. Values brighten the shadow
    // factor (which goes up to 2): specular is the peak highlight of a fully lit pixel, rim the peak outline.
    struct SheenSettings {
        float specular = 0.0f;
        // Blinn-Phong exponent: higher is a tighter, glossier highlight.
        float shininess = 1.0f;
        float rim = 0.0f;
        // How quickly the outline fades from the silhouette inward (higher is thinner).
        float rimPower = 1.0f;
    };

    // The same at every quality level (no extra rays); RT off has no sheen.
    SheenSettings sheenSettings();

    // Colored light

    // A preset light's RGB scaled to unit luminance (Rec. 709 weights), so a color tints the light without changing
    // how bright it is: white stays white, a warm [1, 0.9, 0.75] becomes slightly red-heavy at the same brightness.
    // Channels are capped at MaxLightChannel so a saturated color can't blow out one channel; black, negative or
    // missing colors fall back to white.
    constexpr float MaxLightChannel = 2.0f;
    std::array<float, 3> normalizedLightColor(float r, float g, float b);

    // Blue noise for the ray tracer's random samples (soft shadow jitter, AO directions): a size x size tile
    // (size a power of two) built with void-and-cluster (Ulichney 1993) on a torus, so it tiles seamlessly.
    // Returns the values in [0, 1), row-major: each rank appears once, as (rank + 0.5) / (size * size). Unlike
    // white or interleaved gradient noise, neighbouring pixels get well-spread values, so the filtered result
    // is fine even grain instead of blotches. Deterministic: the same size gives the same tile.
    constexpr uint32_t BlueNoiseSize = 64;
    std::vector<float> blueNoiseTile(uint32_t size);

    // CRT beam simulator

    // The simulated CRT's refresh rate without frame interpolation.
    constexpr double BeamCrtRate = 60.0;

    enum class BeamUnavailable {
        None,
        NeedsVSync,
        RefreshRateTooLow
    };

    struct BeamDecision {
        // The simulated CRT's refresh rate.
        double crtRate = BeamCrtRate;
        // Display refreshes per simulated CRT refresh; 0 when the beam simulator can't run.
        double ratio = 0.0;
        BeamUnavailable unavailable = BeamUnavailable::None;
    };

    // With frame interpolation above 60 fps (e.g. 120), the simulated CRT refreshes at that rate when the display has
    // at least two refreshes per frame, so every interpolated frame gets a scan of its own. Otherwise it stays at
    // 60 Hz and each scan shows the latest frame. contentRate is the interpolated framerate (0 without interpolation).
    BeamDecision decideBeam(uint32_t displayRefreshRate, bool vsync, bool interpolating, uint32_t contentRate);

    // Present pacing

    // The time present i of a game frame's framesToPresent presents is due: spread evenly over the game frame.
    std::chrono::steady_clock::time_point evenPresentTarget(std::chrono::steady_clock::time_point frameStart, int i, int framesToPresent, uint32_t viOriginalRate);

    // Whether a present first sleeps one frame period (at targetRate) past the previous present. Not with the beam
    // simulator: its presents are paced to their even targets and then wait for a vblank, and sleeping a full
    // period first rounded every 120 fps frame up to 3 refreshes at 280 Hz (presents fell to ~92/s).
    bool sleepFramePeriodBeforePresent(bool havePreviousPresent, uint32_t targetRate, uint32_t viOriginalRate, bool beamActive);

    // Present pacing below the display rate (aki-roadmap#87)

    // Without frame interpolation, a game below the display rate (30 fps on 60 Hz) is presented when its frames
    // arrive. World Tour and VPW64 swap at random one VI early or late: their SDK scheduler starts a frame's gfx task
    // either at once or at the next retrace, depending on whether an audio task happens to finish after the task is
    // posted (on hardware the gfx task was always posted late enough to wait for the next retrace). Their presents
    // came 50/17 ms apart instead of 33/33 (~100 hitches/min). Whether to hold presents to the game's rate: the game
    // runs at most 2/3 of the display rate (a frame spans 1.5 refreshes or more), with no interpolation (targetRate
    // at or below the game rate) and without the beam simulator (it paces its own presents). 0 rates are unknown.
    bool holdPresentsToGameRate(uint32_t displayRate, uint32_t targetRate, uint32_t viOriginalRate, bool beamActive);

    // Holds a frame that arrives early until one game frame period after the previous present's target, so the
    // presents settle at the later phase (on hardware's: the frame shows 1 VI later than at the early phase) and come
    // one game frame apart. A late frame moves the grid to its arrival. A hold of a whole game frame period or more
    // starts a new grid instead (the game sped up and the rate is stale), so latency never builds up.
    // The grid advances GameRateSlackUs less than a game frame period: a step of exactly one period never gives the
    // phase back (frames arriving a microsecond faster than the step held each one longer), while this lets it drift
    // back to the arrivals in a few seconds when no late frame comes. Late frames (every ~10 frames in World Tour)
    // re-anchor it long before the drift adds up to a refresh.
    constexpr int64_t GameRateSlackUs = 100;

    // The VI rate estimate (a 2-of-3 vote over the last frames' VI counts) reads 0 for the two frames after a late
    // frame and its catch-up frame (VI counts 3, 1), and 20 after 3, 1, 3: exactly the frames to hold. Pacing
    // turned off there and lost its grid, so ~40% of the 50/17 pairs stayed. The paced rate is kept through up to
    // this many presents of a missing, too fast or slower reading; one that lasts longer is a real rate change.
    constexpr uint32_t GameRateDropoutPresents = 4;

    class GameRatePacer {
    public:
        // The rate to pace this present at, or 0 not to pace it (see holdPresentsToGameRate and
        // GameRateDropoutPresents). A faster rate that can be paced is taken at once: a shorter period never adds a gap.
        uint32_t pacingRate(uint32_t displayRate, uint32_t targetRate, uint32_t viOriginalRate, bool beamActive);

        // When to present a frame that is ready at now, at a rate from pacingRate.
        std::chrono::steady_clock::time_point target(std::chrono::steady_clock::time_point now, uint32_t rate);

        // Forget the grid and the rate (pacing turned off, or a present that wasn't paced).
        void reset();

    private:
        bool haveGrid = false;
        std::chrono::steady_clock::time_point grid;
        uint32_t pacedRate = 0;
        uint32_t stalePresents = 0;
    };

    // Frame interpolation

    // The slowest game rate frame interpolation spreads a game frame over (the AKI games run at 30 or 20 fps).
    // The measured rate dips below it on slow frames (ray tracing), and each displayed frame beyond the second needs
    // a generated-frame target in each of the three rotating target sets, kept for the rest of the run: No Mercy at
    // 60 fps with RT grew them to 3 or 4 targets of 58.5 MB each depending on timing, so GPU memory differed by up to
    // 0.5 GB between identical runs (aki-roadmap#97). A slower game frame shows at most this many frames' worth;
    // the earliest generated frames of a catch-up are skipped instead (InterpolationTicks::beginGameFrame).
    constexpr uint32_t MinInterpolatedGameRate = 20;

    // Displayed frames per game frame: display rate / game rate, rounded, at least 2 (2 for a 30 fps game at 60,
    // 4 at 120), the game rate taken as at least MinInterpolatedGameRate (at most 3 at 60, 6 at 120).
    // capOverride (WCW_HFR_CAP) replaces it when non-zero.
    uint32_t framesPerGameFrame(uint32_t targetRate, uint32_t viOriginalRate, uint32_t capOverride);

    struct FrameWeights {
        float prev = 0.0f;
        float cur = 1.0f;
    };

    // Tick accounting that turns a display rate and a game rate into displayed frames and interpolation weights.
    // Each game frame adds targetRate logical ticks; each displayed frame adds viOriginalRate display ticks.
    struct InterpolationTicks {
        int64_t logical = 0;
        int64_t display = 0;

        void reset();

        // Starts a game frame and returns how many frames to display for it (capped at framesPerGameFrame).
        uint32_t beginGameFrame(uint32_t targetRate, uint32_t viOriginalRate, uint32_t capOverride);

        // Advances one displayed frame and returns its weights. The game frame's last displayed frame has a
        // current weight of 1: the real frame.
        FrameWeights advance(uint32_t targetRate, uint32_t viOriginalRate);

        // Skips displayed frames (not rendered).
        void skip(uint32_t viOriginalRate, uint32_t frames);
    };

    // HDR output

    // The menu's HDR Picture choices. Each sets paper white (the brightness of SDR white: the game picture and menus).
    // The headroom above it (display peak / paper white) is what the CRT filter spends on a stronger phosphor mask.
    enum class HdrPicture : uint32_t {
        // Brighter with the CRT filter off; CrtDetail (but no dimmer than HdrComfortNits) with it on.
        Auto = 0,
        // Windows' SDR content brightness: as bright as the desktop.
        Brighter = 1,
        // Halfway (geometric mean) between Brighter and CrtDetail.
        Balanced = 2,
        // Dim enough that the CRT mask reaches full strength.
        CrtDetail = 3,
        // The user's own paper white, in nits.
        Custom = 4
    };

    // What the display reports. 0 = unknown.
    struct HdrDisplay {
        float peakNits = 0.0f;
        float fullFrameNits = 0.0f;
        // Windows' "SDR content brightness".
        float sdrWhiteNits = 0.0f;
    };

    // The CRT filter's state: on, its preset's own mask strength (guest-advanced "maskstr"), and whether the beam
    // simulator shares the headroom.
    struct HdrCrt {
        bool on = false;
        float baseMaskStrength = 0.3f;
        bool beam = false;
    };

    constexpr float HdrMinPaperWhiteNits = 80.0f;
    constexpr float HdrMaxPaperWhiteNits = 1000.0f;
    // Assumed when the display doesn't report its peak / Windows doesn't report its SDR white.
    constexpr float HdrUnknownPeakNits = 1000.0f;
    constexpr float HdrUnknownSdrWhiteNits = 200.0f;
    // Auto with the CRT filter doesn't go dimmer than this for a stronger mask; CRT detail doesn't go below the floor.
    constexpr float HdrComfortNits = 160.0f;
    constexpr float HdrCrtDetailFloorNits = 100.0f;

    // Paper white in nits for a choice, between HdrMinPaperWhiteNits and HdrMaxPaperWhiteNits. Custom only clamps
    // customNits to that range.
    float hdrPaperWhite(HdrPicture picture, float customNits, const HdrDisplay &display, const HdrCrt &crt);

    // The CRT filter's adaptive phosphor mask. A full-strength mask passes about a third of the light, so in SDR the
    // presets use a weak mask; with HDR the headroom pays for a stronger one and gain multiplies the light back in,
    // so the picture is never dimmer than in SDR. With the beam simulator, mask and beam share the headroom evenly.
    struct HdrMask {
        float strength = 0.0f;
        // Brightness multiplier that makes up for the stronger mask.
        float gain = 1.0f;
        // The beam simulator's share of the headroom (1 = none).
        float beamScale = 1.0f;
    };

    HdrMask hdrMask(float paperWhiteNits, float peakNits, float baseMaskStrength, bool beam);

    // What the settings readout tells the user.
    enum class HdrAdvice : uint32_t {
        // Fine as is.
        Good = 0,
        // Brighter than the display can hold across the whole screen (its full-frame brightness): the monitor dims
        // big bright scenes (No Mercy's white ring mat) and the picture pumps.
        TooBright = 1,
        // With the CRT filter: the mask can't get much stronger than in SDR. Lower the brightness.
        LowHeadroom = 2,
        // Dimmer than a comfortable picture.
        Dim = 3
    };

    HdrAdvice hdrAdvice(float paperWhiteNits, const HdrDisplay &display, const HdrCrt &crt);

    // Generated-frame priming: copying the game's buffer into a run's generated targets happens once per run, before
    // its first generating layer draws the blended frame. Priming after the blend applied translucent full-screen
    // layers (fades) twice; priming every layer replaced the earlier layers' draws with the blend (Revenge flicker).
    // primeEnabled (the game's profile) primes runs whose first layer generates. A run whose earlier layers drew
    // without generating (generatedLayersBefore 0 at runLength > 1) is primed whatever the profile: those layers
    // drew only into the game's buffer, so without the copy the generated targets lack them and keep stale pixels.
    // No Mercy's attract mode starts some runs with a fill-rect-only layer (not an interpolation target, see
    // skipClearOnlyTargets): its generated frames showed an old frame's arena through the title text, a flash every
    // game frame for ~1 s per loop (aki-roadmap#6).
    bool primeGeneratedTargets(bool primeEnabled, bool generateInterpolatedFrames, bool usingMSAA, uint32_t runLength, uint32_t generatedLayersBefore);

    // Multi-layer runs that end early

    // A run's generated frame waits for the run's expected number of layers; a run that ends shorter is published
    // when the next run starts. When it is also shorter than the run before it, the game drew only part of its
    // usual layers this frame (Revenge's attract mode: 1 of 3, 3 of 5), and the generated frame lacks the rest:
    // a one-present flash (a missing overlay or darkening layer, ~15-20% luminance). Such a run's generated frame
    // is dropped and the real frame shows for the whole game frame, as at 30 fps. A run as long as the previous
    // one is the scene's steady structure (fewer layers than the recent maximum) and is published. So is a run as
    // long as the one before the previous: the previous run was a one-frame spike (VPW2 matches draw a 4th layer
    // every ~16th frame of 3), and dropping the steady run after it cost a present 1-3 times a second (#85).
    // runBeforePrevious is 0 when unknown.
    bool publishShortRun(uint32_t runLength, uint32_t previousRunLength, uint32_t runBeforePrevious);

    // Multi-layer runs longer than expected

    // The workload queued after a run's layer, as the render thread sees it when that layer decides whether to publish
    // the run's generated frame: not queued yet, another layer of the same run (same colour address, or a workload
    // that draws nothing), or the first layer of a new run.
    enum class NextLayer {
        Unknown,
        SameRun,
        NewRun
    };

    NextLayer nextLayer(bool queued, uint32_t nextColorAddress, bool nextDrawsNothing, uint32_t runColorAddress);

    // Whether the run's generated frame is published at this layer. It waits for the run's expected length (the
    // longest of the recent runs), but a run can be longer than that: Revenge's attract mode draws a 5-layer run
    // among 3-layer ones, and publishing it at its 3rd layer showed a generated frame without the last 2 layers (a
    // dark flash, generated 6.4 against real 8.2; aki-roadmap#6 cause 2). The game queues a run's layers ahead of the
    // renderer, so the next workload usually says whether the run goes on: while it does, the publish waits for a
    // later layer. Unknown keeps the expected-length rule.
    bool publishRunAtLayer(uint32_t runLength, uint32_t expectedRunLength, NextLayer next);

    // Whether a run's generated frame is dropped (the real frame shows for the whole game frame, as at 30 fps): a
    // layer skipped its generated frame, or a layer found a cut (CameraCutCounter, MatrixSplitCounter). A cut found
    // in a later layer comes after the earlier layers drew their share of the generated frame between the two shots:
    // drawing only the remaining layers real left Revenge's cut into the ring (found in the 5th of 5 layers) with a
    // generated frame of the ring without the wrestlers (aki-roadmap#6).
    bool dropRunGeneratedFrame(bool layerSkipped, bool cutInRun);

    // Camera cuts

    // Relative change between two 4x4 matrices (16 floats each): |cur - prev| / |cur| (Frobenius norms). -1 when a
    // value isn't finite or cur is zero.
    float matrixChange(const float prev[16], const float cur[16]);

    // Counts, over a layer's matched object pairs, how far each pair's camera matrix moved since the previous frame.
    // A camera cut moves every object's camera at once; a pan or zoom moves it a few percent per game frame. On a cut
    // the generated frame interpolated the camera between two unrelated shots and showed a third, made-up view (a
    // stretched wrestler, the wrong part of the arena; 10-50% luminance off for one present, aki-roadmap#6 cause 3).
    struct CameraCutCounter {
        // A pair whose camera changed by more than this has jumped. In Revenge's attract mode (WCW_PAIR_LOG), 99.5% of
        // layers moved their camera by at most 0.10 per game frame and none without a cut by more than 0.29;
        // the cuts measured 0.33 to 2.1.
        static constexpr float JumpChange = 0.25f;
        // A cut needs this many pairs (so one odd object can't decide it) and most of them jumped.
        static constexpr uint32_t MinPairs = 4;

        uint32_t pairs = 0;
        uint32_t jumped = 0;
        float maxChange = 0.0f;

        void reset();
        void add(float change);
        bool cut() const;
    };

    // Widescreen (Aspect Ratio = Expand)

    // Whether a framebuffer pass draws the whole screen, so Expand aspect-corrects it (the pass's 2D keeps its proportions
    // in a widened frame) instead of stretching it. True when the pass's scissor (in framebuffer pixels) has the
    // framebuffer's own pixel ratio (within 10%), or when it spans the framebuffer's full width and at least its height.
    // The second case: RT64 sizes a framebuffer from what was drawn so far, so No Mercy's story-mode screens (480x240)
    // measured 480x196 or 480x198 in some passes and 480x240 in others. The ratio test alone flipped between them, and
    // the dialog text was drawn at one width for a few seconds and then jumped to another.
    bool framebufferPassCoversScreen(int32_t scissorWidth, int32_t scissorHeight, int32_t fbWidth, int32_t fbHeight);

    // A G_MWO_POINT_XYSCREEN vertex modify (value: x in the high 16 bits, y in the low 16 bits, signed quarter pixels of
    // the N64 screen) as it must land in an Expand frame. Transformed vertices of an aspect-adjusted projection get their
    // screen x compressed toward their viewport's center (the projection's x scale times projRatioScale), and the wide
    // viewport then spreads them back to their original proportions. A modify sets the screen position directly, so it
    // skipped the compression and came out stretched by the widening: No Mercy's story-mode flow lines and the hub's
    // panel text moved away from the boxes they belong to. x' = viewportCenterX + (x - viewportCenterX) * projRatioScale;
    // y unchanged. projRatioScale 1 (Original, or a projection Expand leaves alone) returns the value unchanged.
    uint32_t modifiedScreenXYForAspect(uint32_t value, float viewportCenterX, float projRatioScale);

    // Matrix identity across frames

    // AKI engines double-buffer their uploaded matrices: an object's matrix alternates between two slots 'stride'
    // bytes apart from frame to frame. Both slots get one identity: an address whose partner (address +/- stride) was
    // already seen takes the partner's identity. A stride of 0 disables the pairing.
    class MatrixSlotIdentity {
    public:
        explicit MatrixSlotIdentity(uint32_t stride);
        uint32_t canonical(uint32_t address);

    private:
        uint32_t stride;
        std::unordered_map<uint32_t, uint32_t> identities;
        std::mutex identitiesMutex;
    };

    // HUD Placement without G_EX tags

    // The games never tag their HUD with gEXSetRectAlign / G_EX_ORIGIN_*, so Graphics > HUD Placement did nothing.
    // Instead the renderer collects the 2D draws a pass makes after its widened 3D scene (texrects, fill rects,
    // triangles drawn with a matrix without perspective), and anchors them here: draws that touch or nearly touch are
    // one element; an element left of the centre anchors to the left edge, right of it to the right edge, and one
    // across the centre or near it stays centred. Grouping first keeps an element in one piece (a timer's digits,
    // a meter's segments) instead of tearing it at the centre line.
    enum class HudAnchor : uint8_t {
        Centre,
        Left,
        Right
    };

    // A 2D draw's bounds in the game's framebuffer pixels.
    struct HudBox {
        float left, top, right, bottom;
    };

    // Draws closer than this fraction of the screen width (on both axes) belong to one element.
    constexpr float HudElementGap = 0.025f;
    // An element whose centre is closer to the screen centre than this fraction of the screen width stays centred.
    constexpr float HudCentreBand = 0.08f;
    // Passes with more 2D draws than this are left alone (a full 2D screen, not a HUD; also bounds the grouping cost).
    constexpr size_t HudMaxBoxes = 512;

    // One anchor per box. The screen spans screenLeft..screenRight in the same pixels as the boxes.
    std::vector<HudAnchor> hudAnchors(const std::vector<HudBox> &boxes, float screenLeft, float screenRight);

    // Whether HUD placement changes anything: the frame is wider than the game's 4:3 (centredWidthFraction = 4:3 width /
    // output width, below 1) and the HUD Placement mode moves elements (placement = rt64's extAspectPercentage, 0 for
    // Original).
    bool hudPlacementActive(float centredWidthFraction, float placement);

    // Horizontal shift, in normalized device coordinates of the wide output, that moves a centred 4:3 element to its
    // anchor: all the way to the window edge at placement 1, part of the way for a clamped mode (16:9 in a 21:9
    // window). Matches rt64's G_EX_ORIGIN_LEFT / RIGHT rect origins.
    float hudShiftNdc(HudAnchor anchor, float centredWidthFraction, float placement);

    // Texture packs (aki-roadmap#99)

    // The low-mip cache key of a streamed replacement: its pack (the file system index) and its path in that pack.
    // Keyed by the path alone, stacked packs with the same file names (hash-named DDS packs) showed each other's low
    // mip while the real texture streamed in.
    std::string lowMipCacheKey(uint32_t fileSystemIndex, const std::string &relativePath);

    // Texture dumps (aki-roadmap#98)

    // What a dump writes for a texture the game loads.
    enum class TexDumpWrite {
        // Already dumped since the dump started.
        Nothing,
        // The raw files the texture tools read (.tmem, .tile.json, .rice.*), but not the PNG: the pack folder already
        // holds a hash-named file for it, which may be the player's edit.
        SidecarsOnly,
        // The PNG and the raw files.
        All
    };

    // dumpedThisSession: the hash was dumped since the dump started. packFileExists: the dump's pack folder held a
    // "<hash>.png" or ".dds" when the dump started. A second dump session used to rewrite every PNG and lose the
    // player's edits, because only the per-session set was checked.
    TexDumpWrite texDumpWrite(bool dumpedThisSession, bool packFileExists);

    // Display lists (aki-roadmap#116)

    // Most commands the interpreter runs for one display list task. A list that branches back into itself (a hack's
    // codes writing into display-list memory did: WWF inVasion's Stretched Ringmats + Backstage Assault) never ends.
    // The interpreter then ran it forever on the render thread, adding projections and draw data on every pass:
    // ~0.9 GB/s, until the PC ran out of memory. Real hardware would hang the RSP; here the list is ended, so memory
    // stays bounded and the game sees its task complete. Real tasks are far shorter: the longest measured (WCW_DL_STATS=1,
    // 2026-10-20) were 4,361 commands in a TNA vs ROH match and 7,768 in Lucha Underground's attract (859,820 codes).
    constexpr uint64_t DisplayListCommandLimit = 1000000;

    // World transforms one workload may hold: draw calls index them with uint16_t (RSP curTransformIndex, a draw call's
    // min/maxWorldMatrix), so past 65535 the indices wrap and later stages (frame matching) read the wrong ones and
    // crash. A looping list reaches that within one pass; no real frame comes close.
    constexpr uint64_t WorldTransformLimit = 65535;

    // One display list task's budget: take() before each command, with the workload's world transform count; false
    // once the commands pass the limit or the transforms reach WorldTransformLimit (then the list ends).
    struct DisplayListBudget {
        uint64_t limit = DisplayListCommandLimit;
        uint64_t transformLimit = WorldTransformLimit;
        uint64_t commands = 0;
        bool transformsFull = false;

        bool take(uint64_t worldTransforms) {
            commands++;
            transformsFull = transformsFull || (worldTransforms >= transformLimit);
            return (commands <= limit) && !transformsFull;
        }

        bool exhausted() const {
            return (commands > limit) || transformsFull;
        }
    };

    // The Windows extended-length form of an absolute path (aki-roadmap#115): "\\?\C:\..." or "\\?\UNC\server\share\...".
    // The ports aren't long-path aware, so a pack file whose full path reached MAX_PATH (260) didn't open (the texture
    // silently stayed stock), and a folder nested past it threw from the directory walk and ended the game (0xC0000409).
    // Win32 doesn't normalize a \\?\ path, so this also turns '/' into '\', drops "." and empty components and resolves
    // "..". Empty, relative or drive-relative ("C:foo") input, and a path already in \\?\ or \\.\ form, come back unchanged.
    std::wstring extendedLengthPath(const std::wstring &absolutePath);

    // A file inside a pack root (aki-roadmap#115 review): root joined with a pack-relative path. Pack paths come with
    // '/' (rt64.json entries, Rice names, the forward-slash forms the database builds) and may hold "." or "..", and
    // Win32 resolves neither inside a \\?\ path, so with an extended-length root the joined path goes through
    // extendedLengthPath again: '/' becomes '\' and "."/".." are resolved. Any other root is joined as given.
    std::wstring extendedLengthJoin(const std::wstring &root, const std::wstring &relative);

    // Graphics device loss

    // What a renderer thread does with its next piece of work (a display list, a workload, a present, a texture
    // upload batch).
    enum class DeviceLostWork {
        // Render it as usual.
        Render,
        // Skip all GPU work, but keep the bookkeeping going (workload and present ids, queue barriers, upload
        // queues), so the threads waiting on it don't block the quit.
        SkipGpuWork
    };

    // Once the device is lost every GPU call can fail and resources created since come back empty: the render thread
    // crashed building the ray tracing scene, the texture thread naming a texture, after the loss was detected. So
    // from the first piece of work after the loss, no thread touches the GPU again.
    DeviceLostWork deviceLostWork(bool deviceLost);

    // How long the "Graphics device lost" message waits for the player before the game quits anyway. Nobody may be
    // at the keyboard (a test run, a player who walked away); the game must still shut down and flush its saves.
    constexpr double DeviceLostMessageSeconds = 15.0;

    // The quit after device loss, polled by the frontend: show the message once, then quit when the player closes
    // it or after DeviceLostMessageSeconds, whichever comes first, and only once.
    class DeviceLossQuit {
    public:
        enum class Action {
            None,
            ShowMessage,
            Quit
        };

        // deviceLost: the device-lost flag; nowSeconds: any steady clock; messageClosed: the player closed the
        // message.
        Action update(bool deviceLost, double nowSeconds, bool messageClosed);

    private:
        bool shown = false;
        bool quit = false;
        double shownSeconds = 0.0;
    };

    // Frame interpolation: drawn matrices that jump while the objects stay put

    // Whether a matched object pair split: its drawn matrix jumped while the object itself stayed put (its
    // camera-independent matrix didn't move). The game moved part of the view from one matrix to another (No Mercy's
    // attract mode does it on the last frame of each shot, for ~90% of the pairs): the real frames look the same, but
    // interpolating between the two splits showed a third view (smoke and lights moved or gone, a wrestler missing;
    // 10-60% luminance off for one present, aki-roadmap#6). The camera matrices don't jump, so CameraCutCounter
    // misses it. drawnChange and stillChange are matrixChange values of the drawn and the camera-independent
    // matrices; negative (not finite) never splits.
    // A drawn matrix that changed by more than this has jumped (CameraCutCounter's threshold). The split pairs
    // measured 0.75 to 1.5 (WCW_XF_LOG); ordinary motion stays under 0.3.
    constexpr float SplitDrawnJump = 0.25f;
    // The camera-independent matrix changed by at most this: the object stayed put. The split pairs measured up to
    // 0.042 (wrestlers' limbs in motion).
    constexpr float SplitStillChange = 0.1f;

    bool splitPair(float drawnChange, float stillChange);

    // Counts a layer's matched pairs and the split ones among them. When most pairs of a big layer split (the shot's
    // scene: the attract mode's split layers had 34-185 pairs), the run's generated frame is dropped and the real
    // frame shows for the whole game frame. The whole run: the layers before it (a 2-pair layer that split, the
    // background) already drew their generated frames, and a 2-pair split there darkened the frame on its own.
    // Leaving only the split pairs uninterpolated still showed the third view (the view itself is interpolated).
    // Small layers aren't counted: in a match, layers of up to 13 pairs split on ~10% of game frames without a wrong
    // frame (Raw quick match), and dropping those runs would have cost that many generated frames.
    // A big layer also counts when more than a third of its pairs' drawn matrices jumped (by SplitJumpChange or more),
    // whatever their camera-independent matrices did. Revenge moves the wrestlers into place the frame after a
    // camera cut (39 of 94 pairs jumped, their camera-independent matrices moved too), and some Revenge and VPW2 cuts
    // move the objects while the camera test sees < 0.25 (47 of 48, 46 of 113 pairs): the generated frames squashed a
    // wrestler to a sliver, showed a third view or a giant arm (aki-roadmap#6). In the attract modes of WT, Revenge,
    // VPW64, WM2K and VPW2 (WCW_PAIR_LOG), big layers jumped like that on 0.2-1.1% of game frames, nearly all at
    // camera cuts.
    constexpr float SplitJumpChange = 0.3f;
    // Any layer, however small, counts when one of its pairs' drawn matrices changed more than tenfold: no motion does
    // that. Revenge's cut into the ring left every other layer unmatched and showed only in a 6-pair layer (drawn
    // matrices x309); its generated frame had no wrestlers. In five attract modes such pairs showed up on 1-70 game
    // frames per 3 minutes, nearly all at camera cuts; ordinary motion stays under 1.5.
    constexpr float SplitExtremeChange = 10.0f;

    struct MatrixSplitCounter {
        static constexpr uint32_t MinPairs = 20;

        uint32_t pairs = 0;
        uint32_t split = 0;
        uint32_t jumped = 0;
        uint32_t extreme = 0;

        void reset();
        // drawnChange: the pair's drawn-matrix matrixChange (negative when unknown: not counted as jumped).
        void add(bool pairSplit, float drawnChange = -1.0f);
        bool splitLayer() const;
    };

    // Unmapped transforms on generated frames (aki-roadmap#110)

    // Whether a draw's clip-space vertices (x, y, z, w each; vertexCount of them) cross the camera's near plane: some
    // in front of it (z >= -w, the N64 convention RSPProcessCS keeps) and some just behind it (z < -w). Only vertices
    // in front of the camera (w > 0) count, so big meshes that reach behind the camera (floors, crowds) don't; vertices
    // that aren't finite are skipped.
    bool crossesNearPlane(const float *clip, size_t vertexCount);

    // The view-projection a real frame draws with: the game's (viewProj, 16 floats, row-major) with the projection's
    // aspect adjustment (ProjectionProcessor's adjustProjectionMatrix: column 0 times projRatioScale, Expand).
    void drawnViewProj(const float viewProj[16], float projRatioScale, float out[16]);

    // Whether a generated frame draws an unmapped transform (no previous-frame pair: drawn with its current matrix)
    // with the real frame's view-projection instead of the interpolated one, so that it lands and clips exactly as in
    // the real frame. Only for transforms whose vertices cross the near plane under the real or the interpolated
    // view, in a match. World Tour's WCW quick match draws two thin untextured boxes on unmapped transforms right on
    // the camera's near plane (z/w -0.90 to -1.02). The real frame clips them away (one sliver under the timer); the
    // generated frame's camera, half a game frame away, brought them in front of it: two dark slabs across the ring
    // and the mat on one generated frame (luminance 59.1 between 60.6 and 63.2). Hiding unmapped transforms on
    // generated frames removed the dip. Other unmapped objects keep the interpolated view, so they stay in place
    // relative to the interpolated scene around them.
    bool pinUnmappedToRealView(bool inMatch, bool generated, bool realCrosses, bool generatedCrosses);

    // Render target regrow settle (aki-roadmap#4)

    // Render targets newly allocated by one workload (pixels, summed over color and depth) at or above which the
    // workload waits before recording into them, and how long.
    constexpr uint64_t RegrowSettleMinPixels = 4ULL * 1024ULL * 1024ULL;
    constexpr uint32_t RegrowSettleDefaultMs = 100;

    // How long (ms) a workload waits after regrowing its render targets, before it records into them. Supersampling
    // 2x at 1600x900 / 150% DPI regrows No Mercy's targets from 3840x1920 to 7670x2880 a few seconds in; the first
    // workload into the new targets hung the GPU every time (NVIDIA Xid 13 GPC SAVE_RESTORE_ADDR_OOB, then
    // DEVICE_HUNG), even with every rt64 queue serialised and the allocations made while the GPU was idle. Waiting
    // 50 ms or more after the regrow avoided it (10 ms didn't always). The settings-apply hang (aki-roadmap#4, first
    // draws after the targets are recreated) is probably the same case; not verified yet. overrideMs: WCW_REGROW_SETTLE_MS (negative = unset, 0 = off). Small regrows
    // (N64-sized targets, low resolutions) never wait.
    // deferringRelease: released textures are destroyed later (DeferredReleaseList), which is what the wait really
    // bought: the hang came from destroying the textures a regrow releases, not from first use of the new ones
    // (keeping them alive with no wait was clean, destroying them at once with no wait hung). Then nothing waits
    // unless overrideMs asks for it.
    uint32_t regrowSettleMs(uint64_t regrownPixels, int32_t overrideMs, bool deferringRelease);

    // Deferred release of render target textures (aki-roadmap#4)

    // When a render target's textures were released: how many presents had started and how many workloads had
    // finished (their GPU work waited for) by then, and the time.
    struct ReleaseStamp {
        uint64_t presentsStarted = 0;
        uint64_t workloadsDone = 0;
        uint64_t ms = 0;
    };

    // Released textures live on for at least this many finished workloads and this long.
    constexpr uint64_t DeferredReleaseWorkloads = 2;
    constexpr uint64_t DeferredReleaseMinMs = 100;

    // Whether textures released at stamp can be destroyed: every present that had started by then has finished (the
    // present thread records the targets' textures without the workload thread's locks), the workload thread has
    // finished DeferredReleaseWorkloads more workloads, and DeferredReleaseMinMs have passed (the old settle needed
    // 50 ms or more). A clock that went backwards isn't ready.
    bool releaseReady(const ReleaseStamp &stamp, uint64_t presentsFinished, uint64_t workloadsDone, uint64_t nowMs);

    // Released render target textures waiting to be destroyed, oldest first. T is the bundle of GPU objects a target
    // released (destroyed with the entry).
    template<typename T>
    class DeferredReleaseList {
    public:
        void push(const ReleaseStamp &stamp, uint64_t bytes, std::unique_ptr<T> item) {
            if (item == nullptr) {
                return;
            }

            entries.push_back({ stamp, bytes, std::move(item) });
            pending += bytes;
            peakPending = std::max(peakPending, pending);
        }

        // Destroys the entries that are ready (releaseReady), oldest first, stopping at the first that isn't: stamps
        // only grow, so later entries aren't ready either. Returns how many were destroyed.
        size_t collect(uint64_t presentsFinished, uint64_t workloadsDone, uint64_t nowMs) {
            size_t count = 0;
            while (!entries.empty() && releaseReady(entries.front().stamp, presentsFinished, workloadsDone, nowMs)) {
                pending -= entries.front().bytes;
                entries.pop_front();
                count++;
            }

            destroyed += count;
            return count;
        }

        // Destroys everything (shutdown, once no queue uses the textures).
        size_t clear() {
            const size_t count = entries.size();
            entries.clear();
            pending = 0;
            destroyed += count;
            return count;
        }

        uint64_t pendingBytes() const { return pending; }
        uint64_t peakPendingBytes() const { return peakPending; }
        uint64_t destroyedCount() const { return destroyed; }
        size_t size() const { return entries.size(); }

    private:
        struct Entry {
            ReleaseStamp stamp;
            uint64_t bytes;
            std::unique_ptr<T> item;
        };

        std::deque<Entry> entries;
        uint64_t pending = 0;
        uint64_t peakPending = 0;
        uint64_t destroyed = 0;
    };

    // HDR readback for the luminance log and BMP dumps (aki-roadmap#128)

    // An IEEE half float's value (subnormals, infinities and NaNs included).
    float halfToFloat(uint16_t half);

    // Converts the scRGB (RGBA16F) swap chain read back by WCW_PRESENT_LUM / WCW_BMP_* to 8-bit sRGB BGRA, undoing
    // the paper white scale, so the luminance log and BMPs stay comparable with SDR, and adds up the nits. Per pixel
    // this was three powf calls and three half conversions on the present thread: 30+ ms a present at 1280x720,
    // which took HDR runs with the luminance log from 60 presents/s to 10-20 from present 240 on (where the log
    // starts). Tables indexed by the half's bits do the same per channel; the sRGB table is rebuilt when the scale
    // changes.
    class HdrReadbackConverter {
    public:
        // src: width RGBA16F pixels; dst: width BGRA8 pixels. invScale: 1 / the paper white scale.
        void convertRow(const uint16_t *src, uint8_t *dst, uint32_t width, float invScale, double &nitsSum, float &nitsMax);
        // How many times the sRGB table was built (tests).
        uint32_t srgbTableBuilds() const { return srgbBuilds; }

    private:
        std::vector<float> halfTable;
        std::vector<uint8_t> srgbTable;
        float srgbTableScale = 0.0f;
        uint32_t srgbBuilds = 0;
    };

    // The 8-bit sRGB value of a linear value (clamped to 0-1), as the readback always converted it.
    uint8_t linearToSrgb8(float linearValue);

    // In-game Datadog metrics (aki-roadmap#125)

    // WCW_DD_METRICS=1 sends aggregated performance numbers to the local Datadog Agent (DogStatsD, udp
    // 127.0.0.1:8125) every DdWindowSeconds while the game runs. Off by default. The aggregation and the DogStatsD
    // lines are here; common/rt64_wcw_dd is the sink. Names follow AkiTools lib\ddmetrics.ps1 (aki.run.*) with the
    // aki.game. prefix, so one dashboard can show test runs and play sessions side by side.
    constexpr double DdWindowSeconds = 10.0;
    // Each UDP packet stays under this, several lines per packet (as ddmetrics.ps1).
    constexpr size_t DdMaxPacketBytes = 1400;
    // A present interval above this many times the expected frame time is a hitch.
    constexpr double DdHitchFactor = 2.0;

    // Whether an environment value turns a switch on: set, not empty and not "0" (WCW_DD_METRICS, WCW_PERF_LOG).
    bool envSwitchOn(const char *value);

    // One window's numbers. Percentiles and the 1%-low rate need at least one present interval (hasIntervals); the
    // GPU numbers need GPU timing rows (gpuRenderRows / gpuPresentRows).
    // Deliberate differences from aki-run's numbers of the same name (aki-shot pacing, from wcw_present_lum.csv):
    // - hitches: above 2x the larger of the target frame time and the median interval; aki-shot counts above 1.5x
    //   the target, so a 30 fps scene on a 60 target counts every frame there and none here;
    // - p50/p95/p99: nearest-rank, aki-shot interpolates (numpy);
    // - real_fps: presents minus generated presents, so a repeated real frame counts; aki-run prefers distinct
    //   frames from the luminance log (pacing.game_fps), which the renderer doesn't have.
    struct DdFrameStats {
        double seconds = 0.0;
        uint32_t presents = 0;
        uint32_t generated = 0;
        uint32_t repeated = 0;
        double presentsPerS = 0.0;
        double realFps = 0.0;
        double interpolatedPerS = 0.0;
        double repeatedPerMin = 0.0;
        bool hasIntervals = false;
        double p50Ms = 0.0;
        double p95Ms = 0.0;
        double p99Ms = 0.0;
        double maxMs = 0.0;
        // 1000 / the mean of the slowest 1% of the intervals (at least one).
        double low1PctFps = 0.0;
        uint32_t hitches = 0;
        double hitchesPerMin = 0.0;
        // Per GPU timing row (one recorded command list; medians as aki-shot perf / aki.run.perf.*).
        uint32_t gpuRenderRows = 0;
        double gpuRenderMs = 0.0;
        double gpuRenderP95Ms = 0.0;
        double rtBuildMs = 0.0;
        double rtTraceMs = 0.0;
        // Per game frame: the rows of one presentId summed (every workload, every interpolated frame rendered from
        // it), median and p95; and its RT build + trace. Only game frames whose rows all landed in the window.
        uint32_t gameFrames = 0;
        double gpuGameFrameMs = 0.0;
        double gpuGameFrameP95Ms = 0.0;
        double rtGameFrameMs = 0.0;
        uint32_t gpuPresentRows = 0;
        double gpuPresentMs = 0.0;
    };

    // Collects presents and GPU rows and summarizes them per window. Not thread-safe (the sink locks it).
    class DdFrameWindow {
    public:
        // Starts a window at nowMs (steady clock ms) and forgets its samples. The last present time is kept, so the
        // first interval of a window spans the boundary.
        void start(double nowMs);
        // A present at tMs. generated: an in-between frame; repeated: the real frame shown again.
        void addPresent(double tMs, bool generated, bool repeated);
        // GPU time of one render timing row (whole row; its RT build and trace parts), for the game frame presentId
        // (rows of one game frame arrive together), or of one present.
        void addGpuRender(uint64_t presentId, double gpuMs, double rtBuildMs, double rtTraceMs);
        void addGpuPresent(double gpuMs);
        // Summarizes the window up to nowMs and starts the next one there. fpsTarget: the expected present rate
        // (ddFpsTarget). A hitch is an interval above DdHitchFactor times the larger of the target's frame time and
        // the window's median interval, so a game that steadily draws below the target (30 fps on a 60 target) has
        // no hitches, only its real stalls.
        DdFrameStats take(double nowMs, uint32_t fpsTarget);
    private:
        double windowStartMs = -1.0;
        double lastPresentMs = -1.0;
        uint32_t presents = 0;
        uint32_t generated = 0;
        uint32_t repeated = 0;
        std::vector<double> intervals;
        std::vector<double> gpuRender;
        std::vector<double> rtBuild;
        std::vector<double> rtTrace;
        std::vector<double> gpuPresent;
        std::vector<double> frameGpu;
        std::vector<double> frameRt;
        // The game frame whose rows are arriving (kept across windows).
        bool frameOpen = false;
        uint64_t framePresentId = 0;
        double frameGpuSum = 0.0;
        double frameRtSum = 0.0;
    };

    // Nearest-rank percentile (p in 0..1) of unsorted values; 0 for none.
    double ddPercentile(std::vector<double> values, double p);

    // The present rate to expect: the target rate when the framerate setting sets one (Display, Manual), else the
    // game's VI rate (Original). 0 when neither is known.
    uint32_t ddFpsTarget(uint32_t targetRate, uint32_t viOriginalRate);

    // Process numbers (Windows; has = false elsewhere), and VRAM from DXGI QueryVideoMemoryInfo (hasVram).
    struct DdProcessSample {
        bool has = false;
        double privateMb = 0.0;
        double workingSetMb = 0.0;
        uint32_t handles = 0;
        bool hasVram = false;
        double vramMb = 0.0;
        double vramBudgetMb = 0.0;
    };

    // The gauges for one window, by full name (aki.game.*), only those with data.
    std::vector<std::pair<std::string, double>> ddWindowMetrics(const DdFrameStats &stats, const DdProcessSample &process);

    // Whether the window that started at windowStartMs is due to be sent at nowMs (DdWindowSeconds).
    bool ddWindowDue(double nowMs, double windowStartMs);

    // One adapter's DXGI QueryVideoMemoryInfo (local segment): this process's usage and its budget, bytes.
    struct DdVramAdapter {
        uint64_t usage = 0;
        uint64_t budget = 0;
    };

    // The process's VRAM: usage summed over the adapters (it is 0 on the ones it doesn't use), and the budget of
    // the adapter it uses most (the first one on a tie, so an idle second GPU never replaces the first's budget).
    DdVramAdapter ddPickVram(const std::vector<DdVramAdapter> &adapters);

    // The settings and scene the numbers belong to, turned into tags. Gauges carry these tags, so they must stay low
    // cardinality: no per-session ids.
    struct DdContext {
        std::string game;
        uint32_t fpsTarget = 0;
        // UserConfiguration::RefreshRate: original / display / manual.
        std::string framerate;
        bool rt = false;
        uint32_t rtQuality = 0;
        std::string resolution;
        std::string aspect;
        std::string msaa;
        int downsample = 1;
        bool hdr = false;
        // The CRT preset (a name or a .slangp path; the tag is its file stem), empty when the filter is off.
        std::string crt;
        bool beam = false;
        // The ray tracer's detected venue (rt only), empty when unknown.
        std::string venue;
        // WCW_DD_TAGS, already sanitized (ddParseTags).
        std::vector<std::string> extraTags;
        bool operator==(const DdContext &o) const;
        bool operator!=(const DdContext &o) const { return !(*this == o); }
    };

    const char *rtQualityName(uint32_t quality);
    // "crt/presets/crt-guest-advanced-ntsc.slangp" -> "crt-guest-advanced-ntsc": no folders (user paths) in tags.
    std::string ddCrtPresetName(const std::string &preset);
    // The context's tags. Extra tags whose key is one of these tags' own keys are dropped (WCW_DD_TAGS=game:x would
    // otherwise add a second game tag).
    std::vector<std::string> ddContextTags(const DdContext &context);
    // "rt:off -> rt:on, fps_target:60 -> fps_target:120": the tags that changed, by key; empty when none did.
    std::string ddTagChanges(const std::vector<std::string> &before, const std::vector<std::string> &after);

    // The aki-run game key from the executable's name: "NoMercyRecompiled.exe" -> "nomercy", "WCWRecompiled" -> "wt"
    // (AkiTools games.psd1). Unknown names are lowercased without "recompiled" and the extension.
    std::string ddGameFromExe(const std::string &exeName);
    // A play session's tags and events. The sink feeds it the renderer's settings every present and the ray tracer's
    // venue on a change, and sends what it decides:
    // - the first settings start the session (SessionStart);
    // - changes in the first DdSettleSeconds after that are the startup settling (the swap chain's rate arriving,
    //   the saved settings applied) and become the baseline silently; later ones are SettingsChanged, with the tag
    //   changes as text;
    // - a venue is a tag only while ray tracing runs (only the ray tracer detects it); a venue change with RT on and
    //   a session started is VenueChanged.
    constexpr double DdSettleSeconds = 3.0;

    class DdSession {
    public:
        enum class Event {
            None,
            SessionStart,
            SettingsChanged,
            VenueChanged
        };

        struct Decision {
            Event event = Event::None;
            // SettingsChanged / VenueChanged: ddTagChanges of the tags.
            std::string changes;
        };

        // The game and WCW_DD_TAGS, fixed for the session.
        void setIdentity(const std::string &game, const std::vector<std::string> &extraTags);
        // The renderer's settings at nowMs (ms); its game, extraTags and venue fields are ignored.
        Decision noteContext(const DdContext &context, double nowMs);
        Decision noteVenue(const std::string &venue);
        bool started() const { return haveContext; }
        const DdContext &context() const { return current; }
        // The tags gauges and events carry: the context's, or only the game before the session started.
        std::vector<std::string> tags() const;
    private:
        DdContext current;
        std::vector<std::string> currentTags;
        bool haveContext = false;
        double startMs = 0.0;
        std::string game;
        std::vector<std::string> extraTags;
        std::string venue;
    };

    // "conversion:btbw,owner:Fire Ant" -> sanitized tags; entries without a ':' or with an empty value are dropped.
    std::vector<std::string> ddParseTags(const char *list);

    // ddmetrics.ps1 ConvertTo-AkiMetricName: lowercase ASCII letters, digits, '_' and '.', starting with a letter.
    std::string ddMetricName(const std::string &name);
    // ddmetrics.ps1 ConvertTo-AkiTag: "key:value", lowercase, other characters as '_', at most 200 characters; empty
    // when the value is empty.
    std::string ddTag(const std::string &key, const std::string &value);
    // '0.######' (ddmetrics.ps1 Format-AkiValue): up to 6 decimals, no trailing zeros, no exponent.
    std::string ddFormatValue(double value);
    // "name:value|g|#tag,tag"; empty for a value that isn't finite.
    std::string ddGaugeLine(const std::string &name, double value, const std::vector<std::string> &tags);
    // "_e{title,text}:title|text|k:key|s:aki-game|t:alert|#tags" (lengths in bytes; text newlines as \n).
    std::string ddEventLine(const std::string &title, const std::string &text, const std::vector<std::string> &tags,
        const std::string &alertType, const std::string &aggregationKey);
    // Joins lines with '\n' into packets of at most maxBytes; a longer line goes alone.
    std::vector<std::string> ddPackets(const std::vector<std::string> &lines, size_t maxBytes = DdMaxPacketBytes);
};
