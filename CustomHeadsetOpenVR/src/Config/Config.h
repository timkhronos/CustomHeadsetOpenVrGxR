#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <tuple>

// vendor build selection
// build.js passes /DVENDOR_GALAXYXR through ExternalCompilerOptions for the GalaxyXRNative vendor build.
// when no vendor define is set this is the vendor-neutral build (driver name CustomHeadsetOpenVR).
// this mirrors the vendor mechanism in upstream CustomHeadsetOpenVR so the two drivers can coexist.
#if !defined(VENDOR_GALAXYXR)
#define VENDOR_NEUTRAL
#endif

struct ConfigColor{
	double r = 1.0;
	double g = 1.0;
	double b = 1.0;
};

struct HiddenAreaMeshConfig {
	bool enable = false;
	bool testMode = false;
	int detailLevel = 8;
	double radiusTopOuter = 0.25;
	double radiusTopInner = 0.25;
	double radiusBottomInner = 0.25;
	double radiusBottomOuter = 0.25;

	constexpr bool operator==(const HiddenAreaMeshConfig& other) const {
		return std::tie(this->enable, this->testMode, this->detailLevel, this->radiusTopOuter, this->radiusTopInner, this->radiusBottomInner, this->radiusBottomOuter) ==
		       std::tie(other.enable, other.testMode, other.detailLevel, other.radiusTopOuter, other.radiusTopInner, other.radiusBottomInner, other.radiusBottomOuter);
	}
	constexpr bool operator!=(const HiddenAreaMeshConfig& other) const {
		return !(this->operator==(other));
	}
};

struct StationaryDimmingConfig{
	// if the display should be dimmed when the headset is stationary
	bool enable = true;
	// the angle that the headset has to rotate for it to be considered as moved
	double movementThreshold = 0.4;
	// the time in seconds that the headset has to be stationary for it to be dimmed
	double movementTime = 15.0;
	// the amount to dim the display to when stationary
	double dimBrightnessPercent = 2;
	// the amount per second to dim the display when stationary
	double dimSeconds = 10;
	// the amount per second to brighten the display when moving
	double brightenSeconds = 5;
};


// one control point of the spline distortion curve
struct StreamFrameDistortionPoint{
	// radius, 0 at the optical center, roughly 0.5 at the edge midpoints
	double r = 0;
	// radial scale multiplier at that radius, 1.0 = no change
	double scale = 1;
};

// diagnostic band that limits the distortion correction to a radius range so
// one region of the curve can be tuned against untouched surroundings
struct StreamFrameAnnulusConfig{
	bool enable = false;
	double rMin = 0.0;
	double rMax = 0.75;
	// width of the smooth ramp at both edges of the band
	double feather = 0.05;
};

// interactive in-headset distortion tuner: the human eye as the null
// detector. while enabled the driver takes over the distortion curves with a
// per-band working copy edited live from the controllers (joystick y adjusts
// the highlighted band's scale, a/b step bands outward/inward, x cycles
// linked/left/right eye editing, y resets the band, holding either grip
// saves an importable profile). the tuner forces the angular grid and warped
// overlays on so the nulling task is ready the moment the toggle flips.
struct StreamFrameDistortionTuneConfig{
	bool enable = false;
	// scale units per second at full stick deflection (response is squared,
	// so half deflection moves at a quarter rate for fine work)
	double rate = 0.08;
	// band radii, in the same aspect-corrected radius space as the spline r
	std::vector<double> bands = {0.15, 0.22, 0.30, 0.38, 0.46, 0.55, 0.65};
	// stepped adjustment: when > 0, the stick applies exactly this scale
	// step every 100ms while deflected past halfway, instead of the analog
	// rate. deterministic fine nulling ("one click at a time").
	double stepSize = 0.0;
	// opacity of the band highlight ring (0 hides it entirely)
	double ringOpacity = 0.55;
	// force the angular grid + warped overlays on while tuning. off = the
	// tuner leaves the overlays to the user's own toggles (e.g. tuning
	// against real game content, or the world-locked grid variant).
	bool forceGrid = true;
	// band segments for the tuner session: 1 = radial editing as before,
	// 4 or 8 adds a segment walk (joystick click) so each band can be
	// nudged per angular sector. the ALL position (walk start) still
	// edits the whole band; segments carry deltas on top of it.
	// tune segments last: center first, radial bands second — a wrong
	// center masquerades as exactly the asymmetry segments would absorb.
	int segments = 1;
	// per-band segment counts, one entry per band (inner to outer). outer
	// bands cover far more circumference, so they can carry far more
	// segments than inner ones (e.g. 4,4,8,8,12,16,16). empty = uniform
	// `segments` everywhere; shorter than the band list = last entry
	// repeats; entries clamp to 1..32. the tuner flattens whatever layout
	// into uniform max-count segment curves on save, so profiles and the
	// baked lut are unchanged in shape.
	std::vector<int> segmentLayout = {};
};

// center-offset tuning mode: distinct from the band tuner and used
// independently. while enabled the distortion is replaced by a small
// "breathing" radial pulse (sinusoidal k1) whose stationary point makes the
// currently configured optical center directly visible; the sticks then
// drag it onto the lens's true center (the fringe-free sharpest point of
// the fine grid). results are the centerOffset values, saved independently.
struct StreamFrameCenterTuneConfig{
	bool enable = false;
	// amplitude of the breathing pulse (k1 peak). 0.05 = +-1.25% scale at r=0.5
	double breatheAmp = 0.05;
};

// dense per-eye displacement map: the primary (camera-measured) distortion
// correction representation. a regular cols x rows lattice of control
// points over the eye's bounds-normalized uv square (row major, v major:
// index = (row * cols + col) * 2, +0 = du, +1 = dv), each holding the
// SOURCE SAMPLE OFFSET in uv units at that output position: the output
// pixel at uv samples the content at uv + disp(uv), i.e. content appears
// moved by -disp. bicubic (Catmull-Rom) upsampled at bake, sampled with
// one bilinear tap per pixel, applied after (composed with) the radial
// path so radial curves stay valid as a smooth prior or legacy profile.
// scaled by distortion.gain like the curves. an empty or malformed map
// (wrong length) is identity. produced by tools/gxr_sweep.py (Gray-code
// camera fit) or tools/gxr_overlay.py (manual editing against the camera).
struct StreamFrameDisplacementMap{
	bool enable = true;
	int cols = 0;
	int rows = 0;
	std::vector<double> left = {};
	std::vector<double> right = {};
	// provenance, informational only ("graycode", "manual", ...)
	std::string source = "";
};

// camera calibration support: everything the tools/ python scripts drive
// through settings.json (hot reloaded) while a camera sits in front of one
// lens. see Docs/CameraCalibration.md.
struct StreamFrameCalibConfig{
	// force the output of both eyes to opaque black regardless of every
	// other setting: panel protection while the camera rig stays assembled
	// between sessions. checked last in the shader, nothing overrides it.
	bool blackout = false;
	// which eye the calibration outputs (pattern, capture-mode grid) target:
	// -1 both, 0 left, 1 right. the other eye is black while a pattern is
	// showing so it never leaks into the camera. eye-by-eye workflow.
	int eye = -1;
	// grey level (sRGB code fraction, 0..1) of the calibration pattern's
	// white and of the sboys grid lines. independent of the general
	// brightness so the camera exposure can be pinned once.
	double patternBrightness = 1.0;
	// manual editing preset: sboys hue grid drawn in CONTENT space (so the
	// map warps it exactly like game content), scene behind it desaturated
	// and dimmed so the grid reads clearly against whatever world the user
	// is standing in (no opaque grey: keeps the world as an extra visual
	// reference). does not touch the correction gain.
	bool captureMode = false;
	// gray-code sweep pattern index, -1 off. rendered in OUTPUT space
	// (encodes exactly which output uv the panel shows) at patternBrightness,
	// opaque, bypassing the warp/color chain. sequence: 0 black, 1 white,
	// then for axis u then v, for bit 0..bits-1 (MSB first): pattern,
	// inverse. so 2 + 4 * bits patterns; the driver echoes the shown index
	// and a frame count into diagnostic.json for the capture handshake.
	int pattern = -1;
	// bits per axis of the sweep code, 1..12
	int patternBits = 10;
};

// one distortion curve: k1/k2 polynomial coefficients and/or spline points,
// which of the two is evaluated follows the global distortion mode
struct StreamFrameCurve{
	double k1 = 0;
	double k2 = 0;
	std::vector<StreamFrameDistortionPoint> points = {};
};

struct StreamFrameDistortionConfig{
	// "k1k2" evaluates 1 + k1 r^2 + k2 r^4, "spline" interpolates the points
	std::string mode = "k1k2";
	// spline control points of the base curve, sorted by r internally. flat
	// outside the range. the base k1/k2 live at the streamFrame top level.
	std::vector<StreamFrameDistortionPoint> points = {};
	// global multiplier on the correction: baked scale becomes
	// 1 + gain * (scale - 1). gain 1 = the curve as authored, 0 = off,
	// -1 = the exact inverse. one knob for the perceptual 1d search:
	// sweep gain while watching the warped angular grid during a slow
	// head rotation and keep whatever swims least (settles curve sign
	// AND amplitude in one pass, scaling out any measurement bias).
	double gain = 1.0;
	// separate curves per eye and/or per axis. per axis blends a horizontal and
	// a vertical curve around the ring, capturing elliptic/astigmatic error.
	bool perEye = false;
	bool perAxis = false;
	// named curves used when the toggles are active. expected keys:
	// perEye: "left", "right". perAxis: "horizontal", "vertical".
	// both: "leftHorizontal", "leftVertical", "rightHorizontal", "rightVertical".
	// a missing key falls back to the base curve.
	std::map<std::string, StreamFrameCurve> curves = {};
	// angular band segments: 1 = purely radial curves (default). 2..32
	// splits every band into that many angular segments with their own
	// scale, interpolated periodically around the ring — positional
	// correction for top/bottom/nasal/temporal asymmetry that radial
	// bands cannot express. segment curves live in `curves` under keys
	// "left#0".."left#N-1" / "right#0".. and fall back to the plain
	// per-eye curve when missing. segments > 1 takes precedence over
	// perAxis (it is a superset of the elliptic blend).
	int segments = 1;
	StreamFrameAnnulusConfig annulus = {};
	StreamFrameDistortionTuneConfig tune = {};
	StreamFrameCenterTuneConfig centerTune = {};
	// dense displacement map, composed after the radial curves
	StreamFrameDisplacementMap map = {};
};

struct StreamFrameCASConfig{
	// contrast adaptive sharpening applied before encoding
	bool enable = false;
	// 0 to 1
	double strength = 0.5;
	// per-eye override: when enabled, strengthLeft/strengthRight replace the
	// shared strength. lets one eye be sharpened harder (e.g. masking mild
	// off-axis lens blur from facial asymmetry) without over-sharpening the
	// good eye.
	bool perEye = false;
	double strengthLeft = 0.5;
	double strengthRight = 0.5;
};

// fade the streamed frames to black when the headset has not moved for a
// while, e.g. left on a desk with SteamVR running. uniform full fade, so no
// uneven oled wear. brightness returns quickly once movement is detected.
struct StreamFrameDimmingConfig{
	bool enable = false;
	// the angle in degrees that the headset has to rotate to count as moved
	double movementThreshold = 0.4;
	// seconds of stillness before dimming starts
	double movementTime = 15.0;
	// seconds to fade fully to black
	double dimSeconds = 10.0;
	// seconds to fade back to full brightness on movement
	double brightenSeconds = 1.0;
};

// Galaxy XR native-identity options (acted on only in the GalaxyXRNative
// vendor build; the fields always exist so config parsing is uniform)
struct GalaxyXrConfig{
	// rewrite the streamed HMD's visible model/manufacturer to Samsung
	// Galaxy XR, set device icons, and replace the controllers' dangling
	// render model references with converted Galaxy XR controller models.
	// backup/restore semantics; does not touch tracking-system, serial,
	// controller type, or input profile. requires a SteamVR restart.
	bool nativeIdentity = false;
	// live render-model tuning: when non-empty, the controller shim points
	// RenderModelName at {driver}/rendermodels/<variant>_left|_right instead
	// of the default galaxy_xr_controller_*. changing this value in
	// settings.json swaps the model live (SteamVR reloads on name change).
	// used by tools/convert_rendermodels.py --live; cleared when the tuned
	// transform is baked into the shipped assets.
	std::string renderModelVariant = "";
	// override the controllers' InputProfilePath to the shipped official
	// samsung_input_profile.json (controller type stays oculus_touch, so
	// existing Touch bindings keep working; adds official legacy bindings,
	// per-app bindings and grip/aim/tip pose components). replaces the
	// dangling {vrlink}/input/samsung_input_profile.json reference that
	// currently makes SteamVR fall back to generic Touch handling.
	// requires a SteamVR restart.
	bool nativeInputProfile = false;
	// write driver_vrlink.overrideRenderWidth/Height = 3552x3840 (the Galaxy
	// XR native per-eye panel geometry) into steamvr.vrsettings. the APK's
	// spoofed identity makes vrlink cap the render target at the spoofed
	// model's geometry (e.g. 2160x2160); the global override replaces the
	// capped value after model matching (validated by the community
	// Apply-Settings tool, exp17 diagnostics). default ON: this is a
	// correctness fix, not cosmetic. when turned OFF the keys are removed
	// (only if they hold our value), returning vrlink to its own defaults.
	// takes effect at SteamVR start.
	bool nativeResolution = true;
	// stream quality preset, mirroring the community Apply-Settings tiers.
	// "default" leaves vrlink's built-in encode/bandwidth defaults (and
	// removes any tier keys we previously wrote). the other tiers write
	// encodeWidth / streamFormatWidth(1536, validated foveated transport
	// maximum) / recommendedBandwidthMbit+targetBandwidth and disable the
	// automatic width/bandwidth pickers:
	//   stable  2048/1536/250   quality 2560/1536/300
	//   high    3072/1536/300   highest 3072/1536/350 (chroma ceiling)
	//   ultra   4032/1536/350 (above-transport source; needs Wi-Fi 7 6GHz,
	//           watch driver_vrlink.txt for NVENC Invalid Level / buffer
	//           starvation and fall back to high)
	// effective at the next SteamVR start / headset connect.
	std::string streamQuality = "default";
	// uniform scale for the controller render models. the official assets
	// measure ~124x63mm while the physical controller tapes ~145x70mm.
	// 2026-08-24 field default 1.16: SteamVR Home mesh overlays the shell
	// in passthrough. scales the MESH system only: geometry, mesh-bearing
	// component origins, motion pivots/centers and translation vectors.
	// pose anchors (tip, grip family, base, hand_anchor) are real-metre
	// physical points and are never scaled; the skeleton and game hand
	// meshes never see this value. the driver generates a variant folder
	// and swaps to it live via the render-model name-change reload.
	double renderModelScale = 1.16;
	// apply the fixed raw->grip convention shift to the controller poses
	// (rotate X +22deg, translate +5cm local Z). what it actually is, per
	// the 2026-08-24 audit against Game Link's profile: vrlink's raw is
	// Oculus Touch RING convention (Samsung ships the Touch component set
	// verbatim; grip sits 9.7cm down the handle). the shift moves raw to
	// a Valve Index-style origin, and with the knuckles remap (the game is
	// told it talks to an Index) Index-correct titles line up on raw with
	// no per-game work: SteamVR Home mesh, Blade & Sorcery, A Fisherman's
	// Tale. the identity experiment "be a Touch instead" (gripConvention
	// off + simulateTouch, Samsung's own frame) was worse in every title
	// tested (2026-08-24 test B). named pose components are written by the
	// generator from the official values rebased through the inverse of
	// this shift (see officialComponents). the GUI pose offsets are
	// personal trim on top. escape hatch only; leave on.
	bool gripConvention = true;
	// hand_anchor pose component: a driver-tunable pose used ONLY by our
	// per-app default bindings (UE4 titles whose hand mesh is authored
	// for a different controller's raw frame, e.g. The Wizards - Dark
	// Times). raw/handgrip/openxr_grip stay identity for everything else.
	// authored for the LEFT hand in the render model frame (cm, and deg in
	// SteamVR's component rotate_xyz convention); X, yaw and roll
	// are mirrored for the right hand. live: changing a value regenerates
	// the render model variant and SteamVR reloads it on the name change.
	double handAnchorXCm = 0.0;
	double handAnchorYCm = 0.0;
	double handAnchorZCm = 0.0;
	double handAnchorPitchDeg = 0.0;
	double handAnchorYawDeg = 0.0;
	double handAnchorRollDeg = 0.0;
	// controller mesh counter-translation (cm, left-hand authored, X
	// mirrored). the shell mesh was authored on vrlink's tracking origin,
	// so any raw-pose trim that fixes in-game hands (e.g. -2cm Z, 2026-08-24)
	// drags the SteamVR Home mesh off the physical controller. this shifts
	// every mesh-bearing render model component the other way (unscaled,
	// real cm) without touching any pose. live: regenerates the variant.
	// official pose components (2026-08-24 audit against Game Link's
	// vst_controller_*.json): Samsung ships the Oculus Touch component set
	// verbatim (openxr_grip z=0.098/20.6deg, handgrip=grip z=0.097/5.0deg,
	// tip -37.4deg, openxr_aim -39.4deg, base z=0.149). those are physical
	// points measured against vrlink's raw. our raw is vrlink raw with
	// gripConvention applied, so the generator writes the official values
	// REBASED through the inverse convention: every named pose path
	// (dashboard laser via tip, OpenXR grip/aim, handgrip bindings) lands
	// on the same physical point it does under Game Link. the shared /
	// per-hand trims are treated as vrlink error correction and are NOT
	// folded in. false = use the base json values as authored.
	bool officialComponents = true;
	// controller identity experiment (2026-08-24): when true the driver
	// adds an oculus_touch layout (priority 95, above knuckles) to the
	// shipped remapping json at startup so Touch-authored game bindings
	// auto-remap with Touch simulation; when false the layout is removed
	// and knuckles remains the fallback. the remapping file is read by
	// SteamVR at startup: changing this needs a SteamVR restart.
	bool simulateTouch = false;
	// aim-family measured correction. 2026-08-24 test A: with the official
	// tip rebased, the dashboard pointer emanated ~1cm forward and ~1cm
	// above the physical tip (Samsung's tip is the Touch ring-front value
	// on a ringless shell). Y -1 / Z +1 field-ratified the same day.
	// applied to tip and openxr_aim together (same physical feature)
	// after the rebase, in our raw frame, cm, X mirrored for the right.
	double aimTrimXCm = 0.0;
	double aimTrimYCm = -1.0;
	double aimTrimZCm = 1.0;
	// fold the shared (mirrored) and per-hand pose trims into the official
	// component rebase, so trimming where the HAND sits does not drag the
	// physical points (tip, base, grip) along with raw. translation is
	// folded exactly; rotation is folded as yaw only (small, and exact
	// folding would need SteamVR's rotate_xyz euler order).
	bool componentRebaseIncludeTrim = true;
	double meshOffsetXCm = 0.0;
	double meshOffsetYCm = 0.0;
	double meshOffsetZCm = 0.0;
	// skeletal-hand offset (cm), applied in the driver-input tap to the
	// wrist bone of vrlink's skeleton: moves the skeletal hand relative to
	// its anchor WITHOUT touching the device pose, render model, or the
	// grip pivot games rotate around. hot-applied per skeleton update -
	// tune live from settings.json. x is mirrored for the right hand when
	// skeletonOffsetMirror is true.
	double skeletonOffsetXCm = 0.0;
	double skeletonOffsetYCm = 0.0;
	double skeletonOffsetZCm = 0.0;
	bool skeletonOffsetMirror = true;
};

struct StreamFrameConfig{
	// process direct mode layer textures before the streaming driver consumes them
	bool enable = false;
	// saturation with 50 being normal, same semantics as customShader.saturation
	double saturation = 50;
	// vibrance from -100 to 100 with 0 being off: saturation change weighted
	// toward the least saturated pixels (positive enriches muted colors while
	// leaving already vivid ones nearly untouched, so it clips much later than
	// raw saturation; negative pushes muted colors toward gray while vivid
	// accents survive). applied after saturation, stacks with it.
	double vibrance = 0;
	// contrast with 50 being normal, same semantics as customShader.contrast
	double contrast = 50;
	// the point from 0-100% of white that the contrast is centered around
	double contrastMidpoint = 50;
	// if the contrast should be done in linear space instead of gamma
	bool contrastLinear = false;
	// gamma of the output, 2.2 is neutral
	double gamma = 2.2;
	// general brightness multiplier on linear rgb, 1 = neutral, applied
	// at all times after the color chain (also while the dashboard is
	// open). the light-sensitive-eyes / dark-room knob.
	double brightness = 1.0;
	// per channel tint multiplier
	ConfigColor colorMultiplier = {};
	// 3x3 linear rgb color matrix, row major. active when exactly 9 values.
	std::vector<double> srgbMatrix = {};
	// FXAA-class single pass AA integrated into the layer shader, applied
	// BEFORE CAS so sharpening acts on resolved edges (off by default:
	// costs up to ~8 extra taps per pixel on edges and softens text
	// slightly; intended for titles with heavy specular/geometry shimmer)
	// 0 off, 1 fast (in-pass, CAS sharpens raw neighbors around the AA
	// resolved center), 2 quality (separate FXAA pre-pass into an fx
	// intermediate; CAS then sees fully resolved neighborhoods, at the
	// cost of one extra full-region pass and one extra scratch texture)
	int fxaaMode = 0;
	StreamFrameCASConfig cas = {};
	// add low amplitude noise before encoding to reduce banding in dark scenes
	bool dither = false;
	// ==== black floor diagnostics + fixes (GUI Debug section) ====
	// near-black on the GxR stream crushes/steps. candidate mechanisms:
	// (a) a full-vs-limited range mismatch somewhere in the encode ->
	// decode -> display chain (everything below code ~16 crushed, or
	// blacks grey + whites clipped for the inverse), (b) encoder
	// quantization starving dark low-contrast regions — the foveated-
	// encode boundary square that becomes visible in dark scenes is this
	// mechanism's signature: two QP regions with different effective
	// floors meeting at an edge, (c) the display's own OLED black floor.
	// the ramp bar identifies which; rangeMode and the shadow lift are
	// the fixes. see Docs/BlackFloorProtocol.md for the test protocol.
	struct BlackFloorConfig {
		// draw the near-black diagnostic ramps: 17 patches, sRGB codes
		// 0..32 step 2, one strip across screen center (foveal encode
		// region) and one near the bottom (peripheral region), white
		// ticks marking codes 0/8/16/24/32. the bar is injected BEFORE
		// the fixes + dither so the patches ride the exact pipeline
		// game shadows do.
		bool rampBar = false;
		// 0 off; 1 compress into limited range before encode
		// (g' = (16 + 219 g) / 255) — the fix when the display decodes
		// full-range video as limited; 2 expand as if limited
		// (inverse) — the fix for the opposite mismatch
		int rangeMode = 0;
		// shadow-only lift: linear squeeze below kneeCode raising true
		// black to floorCode, identity above. lifts dark content above
		// the OLED/encoder floor without greying the whole image.
		bool shadowLift = false;
		double floorCode = 2.0;
		double kneeCode = 8.0;
		// adjustable black point (field session 2): remap [bp, 255] ->
		// [0, 255] in sRGB code space. the calibration recipe: ramp bar
		// on, raise bp until the two darkest patches just merge, then
		// back off one notch — maximum contrast the chain can carry
		// without crushing real shadow detail. this supersedes
		// rangeMode "expand" for taste-darkening: expand is a fixed
		// 16-code chop (measured crushing ~15 of 17 ramp patches);
		// the black point is the same operation with a chosen pivot.
		double blackPointCode = 0.0;
	};
	BlackFloorConfig blackFloor = {};
	StreamFrameDimmingConfig stationaryDimming = {};
	// radial distortion pre perturbation, applied to the streamed eye images to
	// compensate an imperfect distortion profile on the standalone headset.
	double k1 = 0;
	double k2 = 0;
	StreamFrameDistortionConfig distortion = {};
	// camera calibration support (blackout, patterns, capture preset)
	StreamFrameCalibConfig calib = {};
	// optical center offset from the texture center, in uv units, per eye
	double centerOffsetXLeft = 0;
	double centerOffsetXRight = 0;
	double centerOffsetY = 0;
	// per-eye whole-image alignment shift (prism correction), in fractions
	// of the eye's image (bounds-normalized uv). corrects the RELATIVE
	// alignment between the two eyes' images when an eye sits off its lens
	// axis (the lens then acts as a weak prism and fusion strains — the
	// vertical direction especially, fusional range there is tiny).
	// positive h moves that eye's image right, positive v moves it up.
	// values are small: 0.002 is already a strong vertical correction.
	struct {
		double leftH = 0;
		double leftV = 0;
		double rightH = 0;
		double rightV = 0;
	} alignment = {};
	// skip the color adjustment while the dashboard is open, in case the
	// compositor shader replacement also applies it to the flattened scene in
	// that state. off by default: the recommended setup is to leave the custom
	// shader disabled or neutral for streamed headsets and let this pass be the
	// single source of truth in every state. does not affect cas/dither.
	bool skipColorWhileDashboardOpen = false;
	// process during SubmitLayer (using the previous frame's sync texture)
	// instead of during Present. try this if Present time processing has no
	// visible effect because the driver already consumes the layer at submit.
	bool processAtSubmitLayer = false;
	// gaze consumption (eye tracking tap must be receiving valid data).
	// debugRing draws a small ring at the mapped gaze point per eye — the
	// live calibration tool for the direction->viewport mapping that the
	// dynamic pupil swim pass will reuse. tanHalfFov are the assumed
	// symmetric projection half-angle tangents used for the mapping; tune
	// until the ring lands where you look (live reload, shader hot reload).
	struct {
		bool debugRing = false;
		// fallback mapping only (used when the HMD display component's real
		// projection frusta are unavailable)
		double tanHalfFovX = 1.19;
		double tanHalfFovY = 1.19;
		// lead the gaze by extrapolating recent gaze motion this many ms
		// forward, compensating capture->link->publish latency. 0 disables.
		double predictionMs = 30;
		// overlay a calibration grid: the straight-line reference for pupil
		// swim calibration. mode "uv" = lines every 0.1 uv; mode "angular"
		// = lines every gridAngularDeg degrees of visual angle computed
		// from the real projection frusta (sboy-style distortion photos:
		// each rendered line has a known angular position, so a photo
		// through the lens directly measures distortion error)
		bool debugGrid = false;
		// "uv": lines every 0.1 uv. "angular": lines every gridAngularDeg
		// of visual angle. "sboys": the camera-calibration pattern from
		// sboys3/camera-calibration — per-axis visual-angle lines every
		// gridAngularDeg, HUE-CODED by their absolute angular index so a
		// calibrated camera (and the fit script) can identify every line
		// without counting from center, plus a bright axis cross. render
		// with overlayWarped ON so the pattern passes through the
		// distortion correction like game content does.
		std::string gridMode = "uv";
		double gridAngularDeg = 2.5;
		// sboys mode only: replace game content with a dim grey
		// background so the camera sees nothing but the pattern
		bool gridOpaque = false;
		// world-locked fixation dot for VOR-based swim probing: latched to
		// the current view direction when enabled (toggle off/on to
		// re-center). the user fixates the dot and slowly rotates their
		// head in place; VOR keeps the eye on target, so any systematic
		// gaze-vs-dot residual measures the optics/tracking chain.
		bool calibDot = false;
		// one-switch probe capture for scoring runs: acts as calibDot +
		// swimProbe + overlayWarped together, so an A/B scoring session is
		// a single toggle in the GUI with no ordering to get wrong
		bool probeCapture = false;
		// draw the angular grid at fixed WORLD azimuth/elevation instead of
		// head-locked lens angles: the grid then stays put while the head
		// rotates, which is exactly the stimulus the swim nulling task
		// wants (angular mode only; needs the head pose, on automatically)
		bool gridWorldLocked = false;
		// while the dot is on, log throttled SwimProbe lines: angular
		// residual (raw + smoothed gaze), head angular velocity, and
		// per-eye lens UVs of dot and gaze — the raw data for empirical
		// static-profile and pupil-swim fitting
		bool swimProbe = false;
		// draw the calibration grid and fixation dot in content space so
		// the distortion profile warps them like scene content. use for
		// profile validation: grid straightness + probe scoring runs.
		bool overlayWarped = false;
	} eyeGaze = {};
	// dynamic pupil swim correction (requires gaze). phase A: the
	// distortion center follows the gaze point by these fractions per
	// axis; 0 = static behavior, correction vanishes at center gaze by
	// construction. tune with the debug grid: fixate an intersection,
	// move gaze around it, raise until nearby lines stop
	// bending/shifting with gaze. shift clamped to +-0.15 uv.
	struct {
		double centerStrengthX = 0;
		double centerStrengthY = 0;
	} pupilSwim = {};
	// keyed mutex acquire timeout for the frame sync texture, in ms. when it
	// expires the frame passes through unprocessed (a visible "flash" of
	// ungraded color), which happens under heavy load (shader compilation,
	// level streaming). after a skip the timeout escalates (3x, min 15ms) to
	// break flash streaks, and resets on the next acquired frame.
	int syncTimeoutMs = 10;
	// passive recon logger: opt-in, off by default. installs observation-only
	// vtable hooks on vrlink's D3D11 context to map its layer-consumption
	// point (zero-copy v3 feasibility), NVENC module, and copy/bind shape.
	// intended for ONE disposable session; never substitutes or alters
	// anything. see ReconLogger.h.
	bool reconLogger = false;
	// render-side hitch instrumentation, the HITCHDIAG analog of KALDIAG:
	// every 2s a summary of the frame-callback cadence (dt mean/max, counts
	// over 16.7/33ms, AcquireSync wait, our own work time, skip/create/evict
	// counters), plus a one-shot HITCH line whenever the gap since the
	// previous frame callback exceeds 25ms, tagged with what the previous
	// frame did (scratch create, lut bake, shader compile, sync skip) so
	// outliers self-attribute. cost is a few clock reads per frame.
	bool hitchDiag = true;
	// scratch LRU evictions are moved to a deferred list and released a few
	// frames later, one per frame, AFTER the keyed mutex is released - so a
	// resolution/layer change never pays release cost inside the same
	// mutex-held frame that already pays the (unavoidable) creation stall.
	// off = legacy synchronous evict-in-frame, kept for A/B.
	bool deferredEviction = true;
	// render the processed frame directly into the layer texture (slice
	// aware RTV) instead of drawing into a scratch target and copying the
	// bounds region back. cuts per-eye traffic from ~6x to ~4x of the
	// texture size (the field stutter in heavy titles at 5000x5400+ per eye
	// was bandwidth, not shader math) and halves scratch VRAM. per-texture
	// automatic fallback to the copy-back path if the layer refuses an RTV.
	bool directRender = true;
	// zero-copy path: instead of warping the layer in place, warp into our
	// own shared shadow textures and hand vrlink the SHADOW handles at
	// SubmitLayer. the whole frame path becomes one draw (sample app,
	// write shadow): ~2x traffic vs 4x for directRender and 6x legacy.
	// costs a triple-buffered shadow ring per layer size (same VRAM as one
	// extra swap set). array-layer (single-pass instanced) apps still copy
	// into scratch first (the shader samples Texture2D, not an array), so
	// they run at ~4x into the shadow. experimental: vrlink accepting
	// handles outside its own swap sets is the one assumption we cannot
	// verify from this side, hence default OFF until field-confirmed; if a
	// session shows black/frozen frames, turn this off.
	bool zeroCopy = false;
	// experimental throw/velocity fix mode: 0 = off, 1 = classic (the v3
	// estimator: position-derived linear velocity substituted via a smooth
	// speed-ramped blend, nothing else), 2 = full (adds angular velocity
	// substitution, wrist-flick blend term, peak/direction holds and the
	// release-gesture anchor). classic preserved because field testing
	// rated it the best-feeling iteration; full is the later heuristic
	// stack. json values: "off" / "classic" / "full".
	// 3 = derive: DISCARD the runtime's velocity entirely for streamed
	// (vrlink) controllers and always report the pose-derived estimate —
	// no engage gate, no blend, no peak hold: one consistent self
	// coherent signal, the same method SteamVR itself would use on the
	// poses. all modes apply ONLY to streamed controllers (serials
	// VRLINK*/SamsungVST*); lighthouse devices (LHR-*) have native
	// velocity and are never touched.
	// zero-copy v3: consumption-point source substitution. the frame is
	// warped into a rotating SHARED shadow set and vrlink's per-frame
	// staging copy (the recon-verified single consumption point) is
	// redirected to read the fresh shadow. the layer keeps the app's
	// unprocessed frame, so every failure (stale shadow, open failure,
	// toggle off) degrades to a passthrough flash — never a freeze (v1
	// wall: handle bookkeeping untouched) and never an encoder reset (v2
	// wall: NVENC surfaces untouched). our per-eye traffic 4x -> 2x.
	// EXPERIMENTAL: one dedicated toggle-on test in a disposable session.
	bool zeroCopyV3 = false;
	// NVENC tap: OBSERVE-ONLY recon of vrlink's encoder (init params, rate
	// control surface, registered resources). answers whether NVENC
	// consumes the layer directly (the v3c site) and exposes the parameter
	// surface for the black-floor work. modifies nothing. enable BEFORE
	// launching SteamVR so the encoder creation is not missed.
	bool nvencTap = false;
	// 5 = kalmanCAM ("kalmanCAM"): mode 4 with the fast magnitude channel
	// replaced by a constant-acceleration (Singer) estimator — the
	// low-risk arm of the CA experiment (calm direction untouched).
	// 6 = kalmanCA ("kalmanCA"): one CA estimator per axis carries pose,
	// velocity AND acceleration (angular gains an angular-accel state);
	// the ramp-lag magnitude deficit is removed by the model instead of
	// rescaled away. both CA modes skip the legacy blend/peak-hold
	// stack entirely (clean state reporting).
	int velocityFixMode = 6; // kalmanCA: release default 2026-08-15
	// (supersedes the 2026-08-11 CV consolidation — the CA campaign
	// closed with relDirOff 4.3deg / relAngOff 3.8deg / rel/pk 1.00 at
	// the release instant, felt and instrumented in agreement)
	// stored-config schema for the version-gated migration below (see
	// ConfigLoader): absent in files written before 2026-08-15 -> 1.
	// bump when a migration is added; the GUI persists it (stored as a
	// value distinct from the GUI serializer default so the pruner
	// keeps it, making deliberate post-migration choices sticky).
	int streamFrameSchema = 1;
	// derive-mode speed-adaptive smoothing: the estimator is a low lag
	// endpoint derivative, so its noise shows fully in derive mode (the
	// old modes' 1 m/s engage gate was hiding it). the filter time
	// constant slides from tauSlow (held still: kill trembling, latency
	// invisible) to tauFast (throw speeds: near raw so peak and phase
	// survive) as effective speed (|v| + 0.15|w|) crosses speedLow..High.
	double deriveSmoothTauSlowMs = 90.0;
	double deriveSmoothTauFastMs = 6.0;
	double deriveSmoothSpeedLow = 0.25;
	double deriveSmoothSpeedHigh = 1.6;
	// separate ANGULAR smoothing (off = original behavior: one alpha from
	// combined speed drives both channels, keeping v and w phase locked).
	// field data 2026-08-10: reported |w| swings +-40% around raw with 32%
	// per-sample jitter tails — the shared alpha tuned for linear speeds
	// under-serves the angular channel. when enabled, the angular channel
	// gets its own speed-adaptive alpha from these knobs (angular speeds
	// in rad/s; defaults chosen to match the old 0.15 rad/s-per-m/s
	// conversion, so enabling with defaults is nearly behavior neutral).
	bool deriveSmoothAngSeparate = false;
	double deriveSmoothAngTauSlowMs = 90.0;
	double deriveSmoothAngTauFastMs = 6.0;
	double deriveSmoothAngSpeedLow = 1.7;
	double deriveSmoothAngSpeedHigh = 10.5;
	// derive-mode split-channel output. the axis-wise EMA smooths
	// MAGNITUDE well, but smoothing each axis independently does not
	// stabilize DIRECTION when components sit near zero crossings: field
	// data (2026-08-09 burst log) shows the filtered vector's direction
	// swinging 73-83 deg/sample (median) at ~0.5 m/s and 23-50 deg/sample
	// inside the 40ms release zone — the "objects fly off in random
	// directions" residual. split mode keeps the EMA for magnitude only
	// and takes direction from a speed^weightPow weighted vector sum of
	// the RAW estimator outputs over a short trailing window: fast,
	// high-SNR samples pin the direction, slow noisy ones contribute
	// ~nothing. independent per-channel toggles keep A/B single-variable.
	bool deriveSplitDirLinear = false;
	bool deriveSplitDirAngular = false;
	double deriveDirWindowMs = 50.0;
	double deriveDirWeightPow = 2.0;
	// direction REFERENCE for split mode. field data (2026-08-10) showed
	// the "window" average of raw estimates barely helps: consecutive SG
	// estimates share 7/8 of their input positions, so their noise is
	// almost fully correlated and averaging them does not cancel it.
	// 1 = secant: direction of the raw position DISPLACEMENT across the
	//     derive ring (newest - oldest). displacement over ~25-70ms at
	//     throw speed is 5-20cm against ~1-4mm position noise, so its
	//     direction is clean to a few degrees; lag is ~half the ring
	//     span of arc curvature (deterministic and small). DEFAULT.
	// 2 = runtime: direction of vrlink's own reported velocity (device
	//     side sensor fusion: smooth and consistent, magnitude heavily
	//     smoothed — which does not matter, we only take its direction).
	// 0 = window: the original speed^pow weighted average (kept for A/B).
	int deriveDirSource = 1;
	// MAGNITUDE source for split mode. 0 = vector: |vector EMA| (original;
	// under-reads and jitters during direction change because opposing
	// components cancel inside the average). 1 = scalar: EMA of |raw|
	// itself with the same adaptive tau — smooths the speed without the
	// cancellation loss. field direction is solved by the secant (BURSTDIR
	// 2026-08-10: 2.8-3.5 deg median from raw), so with direction
	// decoupled, tauFast can also simply be raised (15-20ms) for less
	// magnitude jitter with no direction penalty.
	int deriveMagSource = 0;
	// release latch: field data 2026-08-10 (202 ReleaseSnap events) shows a
	// tail problem — the input release event trails the motion, and ~25% of
	// throws sample the output AFTER the hand slowed (release/peak ratio
	// p25 = 0.80, long tail to near zero). when enabled, the moment a
	// trigger/grip RELEASE arrives from the input tap, the output replays
	// the peak (v, w) of the last latchWindowMs for latchHoldMs (full
	// strength for the first half, linear decay after) so late-sampling
	// games still read the throw. median throws (already at peak) are
	// unaffected. derive mode only; off by default for a clean A/B.
	bool deriveReleaseLatch = false;
	double deriveLatchWindowMs = 150.0;
	double deriveLatchHoldMs = 120.0;
	double deriveLatchMinSpeed = 0.8;
	// per-channel latch peaks (field 2026-08-10: a single effective-speed
	// peak key picked the WINDUP moment for arm throws — |w| spikes while v
	// points backward — and the latch replayed that poisoned vector at
	// release: ratio p90 2.22, direction 56 deg off. flicks improved with
	// the same key because their true peak IS angular dominant. so: v
	// replays from the linear-peak moment, w from the angular-peak moment,
	// each behind its own gate.)
	double deriveLatchAngMinSpeed = 6.0;
	// input position prefilter feeding the derive fit AND the secant:
	// per-axis median of the last 3 raw positions kills single-sample
	// network spikes (the p90 18%/sample jitter tail) at ~1 sample lag.
	// "off" or "median3".
	int derivePreFilter = 0;
	// input pre-smoothing (the adjustable-strength version of the
	// prefilter idea): EMA over raw positions/orientations BEFORE any
	// derivation, strength in ms (0 = off). scope selects what consumes
	// the smoothed stream: "direction" = only the secant (direction is
	// cleaned, magnitude still derived from the exact positions);
	// "both" = the fit AND the secant (maximum smoothness, some peak lag).
	double derivePreSmoothMs = 0.0;
	int derivePreSmoothScope = 0; // 0=direction 1=both
	// CONSUMER DISCRIMINATOR (diagnostic): many engines ignore the driver's
	// reported velocity entirely and estimate throws from rendered pose
	// history (Unity XR toolkit, VRTK, custom rigs). across sessions our
	// radically different velocity outputs produced near identical felt
	// results — the signature of exactly that. "zero" reports zero
	// velocity: if throwing still works AT ALL, the game does not read
	// vecVelocity and the pose stream is the real battlefield. 2-minute
	// test, then turn it off.
	int deriveDiagVelocity = 0; // 0=off 1=zero
	// pose-assist: if the game derives throws from pose deltas, make the
	// POSE tell the throw's story too — during the latch hold, the
	// reported position is forward-integrated along the latched velocity
	// (same decay), so pose-history estimators read the clean release
	// instead of the snap-back. brief visual hand overshoot at release is
	// the price; opt-in.
	bool deriveLatchPoseAssist = false;
	// KALMAN mode (velocityFixMode "kalman"): replicate the native
	// lighthouse ARCHITECTURE rather than patching symptoms. native
	// controllers report one coherent fused kinematic state — pose,
	// velocity, angular velocity all from a single estimator, so the
	// runtime's forward prediction and every game-side pose-history
	// estimator agree by construction. this mode runs a per-controller
	// constant-velocity Kalman filter over the incoming stream and reports
	// THE FILTER STATE as the pose: position, orientation, v and w are
	// self consistent; no splits, no latches, no replays.
	// kalmanProcessAccel (m/s^2) is THE responsiveness knob: high = trusts
	// motion (snappy, noisier), low = trusts smoothness (calm, laggier).
	// RATIFIED 2026-08-11 (campaign close, §3/§4): A=1 P=2.7 W=400 O=1.25
	// L=0. these ARE the consolidation defaults — the 1.6.0 commit updated
	// mode/dup/coast but missed this trio, so the driver published the
	// pre-campaign 40/2.0/0.5 and every GUI reset restored untuned values
	// (field incident 2026-08-11, cost one capture).
	double kalmanProcessAccel = 1.0;
	double kalmanPosNoiseMm = 2.7;
	double kalmanProcessAngAccel = 400.0;
	double kalmanOriNoiseDeg = 1.25;
	// optional fixed forward prediction of the reported state (native
	// drivers do this to counter transport latency); 0 = off
	double kalmanLeadMs = 0.0;
	// EXPERIMENT B — fixed-skew release rewind (single-session test,
	// default OFF). the input release event travels a slower path than the
	// pose stream: it lands 50-150ms after the true release, so games
	// sample the snap-back. this reports, for a short hold after the
	// release event arrives, the velocity from rewindMs EARLIER in the
	// kalman history — pure time re-alignment by one physical constant
	// (the transport skew), no peak picking, no heuristics. if the right
	// rewind exists, opposite throws vanish at one setting; if no setting
	// works, the hypothesis is falsified and the experiment ends. the
	// pose is never touched.
	double kalmanReleaseRewindMs = 0.0;
	double kalmanRewindHoldMs = 100.0;
	// direction/magnitude split reporting (field 2026-08-10 tuning session:
	// the user's hands found A=1 best DESPITE weak throws — direction
	// stability dominates felt quality, but magnitude lag at A=1 makes
	// items fall out of the hand. the two channels want different
	// smoothing, exactly the derive-era split finding. when set, the
	// REPORTED velocity direction comes from an EMA of the state velocity
	// with this time constant, while magnitude stays live from the state —
	// run A back at 40-60 for full-strength snappy throws with A=1-like
	// direction calm. continuous and phase agnostic: no events, no moment
	// picking. 0 = off. pose untouched.
	double kalmanDirSmoothMs = 0.0;
	double kalmanAngDirSmoothMs = 0.0;
	// direction derotation lead (2026-08-14, the combined-throw fix): a
	// filter with velocity group delay L reports the path tangent from L
	// ago. on a STRAIGHT path that is a pure magnitude deficit; on a
	// CURVED path — exactly what linear + wrist combined produces — it
	// is a DIRECTION error of ~omega*L (at low-J group delays of tens of
	// ms and throw omega of 8-20 rad/s: 15-40deg, present only when both
	// channels are active; matches the field triad of linear-fine /
	// flick-fine / combined-bent). this rotates the reported velocity
	// forward about the filter's own w-hat by |w| * dirLead (Rodrigues)
	// — the direction-space analog of kalmanLeadMs. continuous, always
	// on, no gating: identity when w ~ 0 (linear throws untouched),
	// nothing to bend when |v| ~ 0 (pure flicks untouched). magnitude,
	// spin and pose are never touched, so it composes cleanly with low
	// J's release-instant peak-hold — the two J pressures decouple.
	// tune: start ~ the felt group delay (10-30ms at J=10-17), watch
	// PEAKDIAG dirOff on combined throws. 0 = off.
	// RATIFIED 2026-08-15: Td=5 (supersedes the 2026-08-14 Td=10). the
	// peak instant and the release instant want DIFFERENT derotation:
	// release sits on the post-peak downslope where less lag has
	// accumulated, and the game samples release. the relDirOff
	// instrument (release-instant output vs ring-secant truth, the
	// channel relOffPk is structurally blind to) reads 4.3 / 5.7 /
	// 8.7 / 12.7 deg at Td 5/10/15/20 — monotone, hands agree, and
	// rel/pk stays 1.00 throughout (magnitude provably untouched).
	// 2026-08-17 correction: that relDirOff series was scored against a
	// secant of the FILTERED pose (DeriveMotion ran after the report
	// block overwrote the pose), so it grows ~|w|*Td by construction —
	// it did not ratify anything. what stands is the felt evidence
	// (combined throws skew left without derotation) and the raw-
	// referenced session: Td 0/10/5 scored 10/13/12 of 15 on combined
	// throws, i.e. Td=5 fine, Td=0 worse. Td=5 kept. the CA-full
	// state-prediction variant (v + a*tau*(1-e^(-L/tau))) added 08-15
	// was inert at the shipped J/tau (dead accel state, ~0.2deg bend)
	// and is retired; CA-full uses the Rodrigues form like every mode.
	// scope: this knob shapes vecVelocity; a pose-history game only sees
	// it through the runtime's ~10ms extrapolation (a few degrees at
	// most). second-order for such games, first-order for vecVelocity
	// games.
	// 2026-08-24: default 0. the Td=5 ratification was scored while
	// vecAngularVelocity was reported in the wrong (world) frame, so the
	// derotation was largely compensating vrserver's mis-predicted
	// orientation. with kalmanAngularOutFrame=body, a nonzero Td made
	// throw directions erratic in the field. keep 0 unless re-ratified.
	double kalmanDirLeadMs = 0.0;
	// adaptive direction lead (2026-08-15, tail experiment A): the fixed
	// Td is tuned for the median throw, but the release-tail autopsy
	// shows every genuine residual (12-24deg) is a maximum-violence
	// whip (wRel 23-39 rad/s) where the filter's effective lag is
	// ~8-17ms, not 5 — the optimum is throw-dependent. when enabled,
	// the derotation time becomes Td_eff = base + slope * |w| (clamped
	// to 50ms total), OVERRIDING the manual knob: an ordinary 12 rad/s
	// throw gets ~8.6ms, a 30 rad/s whip ~14ms. pure proportionality —
	// no thresholds, identity at w = 0, magnitude and pose untouched.
	bool kalmanDirLeadAdaptive = false;
	double kalmanDirLeadBaseMs = 5.0;
	double kalmanDirLeadWMs = 0.3; // ms per rad/s
	// adaptive measurement trust (2026-08-15, tail experiment B, pure
	// opt-in): innovation-scheduled R — the textbook adaptive-Kalman
	// route, attacking the lag ITSELF instead of compensating it (so
	// it also reaches pose-history games, which the derotation cannot).
	// a fast EMA of the BASE-R-normalized NIS (base-normalized, or
	// shrinking R would inflate the very statistic that shrinks it)
	// divides R and Ra continuously: divisor = clamp(schedNis, 1,
	// maxDiv). at the measured baseline (nis ~0.02, R overstated ~50x)
	// the divisor sits pinned at 1 = bit-identical to off; during a
	// violent whip the innovations blow through the model and trust
	// ramps within ~25ms. the cost is honest: measurement noise passes
	// through during fast motion, where it is perceptually masked.
	// retired for this transport (2026-08-16): even with the dup
	// composition fixed, dividing R during maneuvers makes the filter
	// hug STALE samples at release (left bias, magnitude jitter). kept
	// functional for experiments; not recommended.
	bool kalmanAdaptiveR = false;
	double kalmanAdaptiveRMaxDiv = 16.0;
	// magnitude channel (field 2026-08-10: A=1 + raised P/O is the user
	// verified sweet spot for DIRECTION, but that configuration's lag
	// under-reports throw SPEED — "strength feels low", items falling out.
	// the inverse of naive splitting: direction stays with the calm state;
	// MAGNITUDE comes from a parallel fast estimator over the same
	// measurements (kalmanMagSource "fast", accel knob below). magScale is
	// a plain always-on trim multiplier on top (1.0 = neutral).)
	int kalmanMagSource = 0; // 0=state 1=fast
	double kalmanMagAccel = 60.0;
	double kalmanMagScale = 1.0;
	double kalmanAngMagScale = 1.0;
	// duplicate-sample skip (field 2026-08-10: raw-step telemetry caught
	// 12,121 frozen steps in one session — vrlink repeats the last pose
	// whenever fresh tracking data has not arrived, and every repeat tells
	// the filter "the hand stopped dead". this drags throw velocity down
	// (the calm state's chronic weakness) and makes fast estimators
	// oscillate stop/jump (the flip engine). a duplicate is a MISSING
	// measurement, not a measurement of stillness: while the state is
	// moving, duplicates now coast the filter (predict only) instead of
	// braking it. genuine stillness keeps normal updates. textbook
	// missing-data handling; toggle for A/B.
	// 0=off 1=coast 2=drop. field 2026-08-11: with device-time active,
	// COAST is feel-rejected (extrapolated positions + snap poison the
	// pose history the game fits throws from; NIS 100-500 spikes), and
	// OFF pays a velocity drag (every repeat says "stopped"; the chronic
	// ~0.80 strength). DROP treats a detected repeat as never having
	// arrived: no measurement, no prediction, no clock advance — the
	// next real sample predicts across the full accumulated device-time
	// gap in one honest step. no fake stillness, no invented positions;
	// reported pose holds (runtime still animates from v). the run cap
	// below applies to coast AND drop. default off = field champion.
	// 3=soft: the repeat IS processed as a measurement, but with R
	// inflated by dupRScale^2 — honest noise model for a sample of
	// unknown age (its true uncertainty at hand speed v is v*sigma_age,
	// not the sensor floor). gain on repeats shrinks ~k^2 while
	// covariance keeps accumulating through the run, so fresh-sample
	// catch-up self-schedules. dupRScale=1 in soft is bit-identical to
	// off; k -> inf converges toward coast/drop. the run cap applies:
	// repeats sustained past it are accepted at full weight.
	// 4=age (2026-08-16, A/B candidate): the honest version of soft.
	// a repeat is a true position of unknown age; its uncertainty is
	// how far the hand moved in that age, so R_rep = R + (|v|*age)^2
	// (and Ra_rep = Ra + (|w|*age)^2) with age = time since the last
	// FRESH sample. no speed gate (soft only distrusts repeats above
	// 0.5 m/s, so a slow toss runs a different filter than a throw
	// and the behavior steps at 0.5), no scale knob, no 5mm floor:
	// at rest it collapses to R (repeats believed, v -> 0), at 5 m/s
	// a 3-frame-old repeat is (5cm)^2 and effectively ignored, and a
	// long repeat run distrusts itself progressively instead of
	// flipping at the cap. offline it is numerically identical to
	// soft/k=3 at throw speed and continuous below it. the run cap
	// still applies as the "tracker stopped producing" backstop.
	// field 2026-08-17: soft, age and coast are indistinguishable on the
	// raw-referenced release instruments (rel/rawPk J4: 0.78/0.80, J6:
	// 0.86/0.87, J2: 0.58/0.63 soft/age); scores leaned soft. soft
	// stays default; age is the cleaner formulation for anyone who
	// wants a threshold-free dedup.
	int kalmanDupMode = 3;
	double kalmanDupRScale = 3.0;
	// teleport guard: reinit instead of innovating when an accepted step
	// exceeds this floor AND implies >25 m/s (physically impossible hand
	// speed = unflagged tracking reacquire). floor ignores freeze
	// catch-ups (~0.1m). 0 disables. config-only this slice; GUI knob
	// rides the next GUI-touching slice.
	double kalmanTeleportM = 0.75;
	// flagged-loss coasting (2026-08-15): during a flagged tracking loss
	// (out-of-FOV hand) the raw pose passes through FROZEN and the
	// device zero-fills velocity — the hand visibly parks for the loss
	// duration (field: brief FOV exits cluster at ~100ms). native
	// drivers dead-reckon on the IMU through optical loss; vrlink gives
	// us nothing, but the filter state is the next best thing: for up
	// to this many ms of loss the reported pose is the state predicted
	// forward (Singer decay bounding the acceleration, so a stale hot
	// accel cannot run away), velocities reported from the same
	// prediction. stateless per callback (predicted from the last
	// committed state, never re-committed), so nothing accumulates;
	// reacquire still takes the existing clean-reinit path. past the
	// window the pose passes through raw, exactly as before. 0 = off.
	double kalmanLossCoastMs = 250.0;
	// measurement timestamping (estimator correctness pass 2026-08-10):
	// vrlink stamps every pose with poseTimeOffset, and this session's
	// field data shows it is real and VARYING — median +13.8ms, stdev
	// 3.8ms, sample-to-sample swings of ~6ms during throws, and a stale
	// tail down to -69ms. the filter previously treated every sample as
	// "now": at 5 m/s a 6ms timing swing masquerades as 30mm of position
	// noise against a 4mm R, which is exactly the unmodeled noise that
	// forced A=1 and its weak throws. with deviceTime on, each
	// measurement is stamped tMeas = receipt + poseTimeOffset and dt is
	// the device-time delta; out-of-order samples (dt <= 0) are DROPPED,
	// never reinit (the old dt<=0 reinit would zero velocity mid-throw
	// once device time is in play). |offset| > 100ms falls back to
	// receipt time. toggle off = previous behavior exactly, for A/B.
	bool kalmanDeviceTime = true;
	// 3dof-fallback protection (2026-08-16): a frozen position with a
	// moving quaternion is the tracker losing POSITION only (fast or
	// occluded hand -> IMU-only fallback), not a still hand. when
	// detected, the position stays hard-distrusted past the dup run cap
	// and the live orientation keeps updating in every dup mode. field
	// symptom this kills: hand parked ~1m away but still rotating with
	// the wrist for ~0.5s, then teleporting back.
	bool kalmanPosFreeze3dof = true;
	// frame of the REPORTED vecAngularVelocity (and vecAngularAcceleration).
	// the openvr header never states it; the kalman state w is world /
	// driver-frame (integration is dq (x) q) and linear velocity IS world
	// (throw directions prove it). FIELD 2026-08-24: vrserver's photon-time
	// prediction treats the angular vector as BODY-frame - reporting world
	// made a horizontal sword swing pitch up at peak |w| (~15deg at 10rad/s
	// x 25ms prediction); body fixed it, horizontal swings correct since.
	// 0 = world (pre-fix behaviour), 1 = body (q^-1 w q), 2 = zero (the
	// discriminator that found it). live reloaded.
	int kalmanAngularOutFrame = 1;
	// knob B: coordinated-turn coast during position-only freezes. while
	// the tracker feeds frozen positions with a live quaternion, the
	// linear state is predicted along its own v/a (a straight line
	// tangent to the swing). with this on, v and a are rotated by the
	// live angular velocity each step (exp(w dt)), so the coasted hand
	// follows the arc the wrist is tracing. scale 1.0 = full coupling,
	// 0 = off (default: not needed once the angular frame was fixed).
	double kalmanFreezeCoastTurn = 0.0;
	// velocity decay time constant during position-only freezes (ms).
	// short freezes coast (throws unaffected), long occlusions glide to
	// a stop instead of sailing on the occlusion-entry velocity and
	// reacquiring with a wrong-direction state. 0 = pure coast (the
	// 2026-08-16 pre-decay behavior). clamped 20-2000 when nonzero.
	// default 0 since 2026-08-17: the three instrumented sessions
	// (raw-referenced release scoring, ~1100 posOnlyFreeze callbacks
	// per 2-minute epoch) showed no throw or freeze symptom the decay
	// addressed, and the ad-hoc decay is applied to the state but not
	// the covariance (the velocity terms keep growing during the
	// freeze), so the first fresh sample after a long freeze kicks the
	// velocity anyway. pure coast is the consistent choice; the knob
	// stays for the field case that motivated it (long occlusion +
	// wrong-direction reacquire).
	double kalmanPosFreezeVelDecayMs = 0.0;
	// dup run cap (bug fix 2026-08-10; rationale sharpened 2026-08-11):
	// no human hand holds a position BIT-IDENTICALLY for tens of ms —
	// real stillness shows micro-tremor above the 0.3mm gate. a repeat
	// sustained past this cap therefore means the TRACKER stopped
	// producing (set-down controller, long dropout), and in both cases
	// believing the repeat (velocity to zero, hold position) beats
	// extrapolating or distrusting blind. also closes the runaway loop:
	// skipping repeats blocks the very measurements that update the
	// speed the dup gate tests. applies to coast, drop, and soft.
	double kalmanDupCoastMaxMs = 90.0;
	// ET gaze aim assist (plan C): people fixate throw targets BEFORE the
	// hand releases, so gaze carries the intended direction through the
	// one channel immune to the input-timing problem that produces the
	// opposite-direction tail (~8% of throws sample the snap-back). when
	// enabled, the reported velocity direction is bent toward the gaze
	// ray by assist fraction of the angle between them, capped at maxDeg,
	// only above minSpeed, only with fresh valid gaze (<100ms). direction
	// only — magnitude and spin untouched; the rendered hand untouched.
	double kalmanGazeAssist = 0.0;   // 0..1
	double kalmanGazeMaxDeg = 30.0;
	double kalmanGazeMinSpeed = 1.2; // m/s
	// fixed-lag smoothing (field 2026-08-10: the full-lock gaze test proved
	// this game IGNORES driver vecVelocity — 18k bends up to 177 deg with
	// zero effect on throws — and derives throws from POSE history. the
	// battlefield is the reported position stream. a filter estimates the
	// present from the past; a smoother estimates L ms ago using samples
	// from BOTH sides — calm like A=1 AND amplitude-accurate like high A,
	// which filtering fundamentally cannot combine. the entire reported
	// state (pose + velocities, coherent) shifts to t-L; the one honest
	// cost is L ms of added hand latency. 0 = off.
	// 2026-08-17: in CA-full this is now a real fixed-lag Rauch-Tung-
	// Striebel smoother (backward recursion over the stored predicted/
	// filtered Singer states down to t-L; see KalState::rtsN). the CV
	// modes keep the old two-estimate fusion. field motivation: the raw
	// referenced instruments put the input release event only ~35ms
	// (IQR 25-55) after the raw velocity peak, and every causal J read
	// the release while still RISING (rel/rawPk 0.6-0.87, dtPk=0); the
	// faster J that reads more (J=6) went erratic in direction. a
	// smoother reports the velocity AT t-L (L ~ skew) using samples from
	// both sides — the peak of a fast filter at the calm of a slow one —
	// so the game reads the release vector itself. poseTimeOffset
	// carries -L so the runtime predicts from the right epoch. offline:
	// J=12 P=1.5 L=35 reads 0.93/0.97/0.92 of the raw peak at +30/45/60
	// ms with the rest-noise of causal J=6 (which reads 0.90/0.95/0.93
	// but with the field's erratic direction). the two-estimate fusion
	// it replaces was not a smoother.
	double kalmanSmoothLagMs = 0.0;
	// how the smoothed (t-L) state is stamped for the runtime.
	// 0 = latent (default): poseTimeOffset unchanged, i.e. the L-old
	//     state is presented as current. the runtime predicts only its
	//     usual ~photon horizon; the rendered hand carries L ms of extra
	//     latency; the submitted POSITION stream (what this game
	//     differentiates for throws) is the smoothed trajectory delayed
	//     by L, so a release event ~L after the true release reads the
	//     peak vector.
	// 1 = honest: poseTimeOffset -= L. field 2026-08-17: with L=35-45
	//     the runtime extrapolated the position by L+photon (65-80ms)
	//     from the reported velocity; the game's pose-history velocity
	//     is then v + T*a, and at release (hand decelerating) that is
	//     weak or backward. RELDIAG showed relOut/rawPk 0.88-0.92 at
	//     5-7deg (the reported vecVelocity was right) while throws fell
	//     out of the hand — the direct proof this game reads pose
	//     history, not vecVelocity. kept for games that do read
	//     vecVelocity and prefer honest epochs.
	int kalmanSmoothLagEpoch = 0;
	// ==== constant-acceleration (Singer) experiment knobs ====
	// the CV model treats the throw ramp as noise and structurally lags
	// its peak (the measured ~80% magnitude deficit); a CA state tracks
	// a ramp with zero steady-state velocity lag while R stays at the
	// sensor floor — smoothing is NOT loosened to buy magnitude. jerk
	// (m/s^3) is the responsiveness knob of a CA channel, replacing
	// process accel. the acceleration state decays toward zero with
	// tau (Singer model), bounding phantom integration across dup
	// coasts and stops; tau -> inf recovers pure CA for A/B honesty.
	// CA-full responsiveness (linear jerk, angular jerk). field-derived
	// defaults (sessions 1-4, 2026-08-13): RELDIAG showed the release
	// instant reads the post-peak downslope, so low jerk — whose decel
	// lag acts as an accidental peak hold — beats high jerk at the only
	// instant the game samples (J=10: rel/pk 1.00, relOff 0deg median;
	// J=51: rel/pk down to 0.72, relOff 8deg median).
	// RATIFIED 2026-08-14 (Td session): J=17 P=5.7 O=5.75 Td=10. J=17
	// ran the whole sweep clean (rel/pk 1.00, relOff 0.0deg); prior
	// session: good to at least 17, very bad at 50.
	// RATIFIED 2026-08-16 (composition-fix session): with the adaptiveR/
	// dedup composition fixed and honest noise viable, J=4 ran the
	// standard battery "genuinely great, best of everything so far".
	//
	// SHIPPED 2026-08-17 after four instrumented sessions with a RAW
	// reference (see KalState::rawRingN; the pre-08-16 rel/pk, dirOff and
	// lagLin numbers above were filtered-vs-filtered and are kept only
	// as history). what is now measured, not vibed:
	//  - only the RATIOS J/P and Ja/O move behavior (Kalman gains depend
	//    on Q/R alone); P and O are the tracker's noise, not tuning.
	//  - at J=4/tau=20 the acceleration state is effectively dead (peaks
	//    ~4 m/s^2 against a ~65 m/s^2 throw); the filter is a smooth CV
	//    with ~14ms position lag and ~60ms velocity lag. that is not a
	//    flaw: the game(s) tested read POSE HISTORY (twice confirmed:
	//    the 08-10 gaze test, and the RTS session where vecVelocity was
	//    strong+aimed at release yet throws failed). the causal
	//    position stream carries the lagged velocity's momentum for a
	//    beat after the peak, so its finite-difference velocity at the
	//    input release event (skew 22-43ms after the raw peak, drifting
	//    with fatigue) reads 0.95-1.16 of the raw peak at 6-7deg — and it
	//    holds that across a 2x spread in skew. RELDIAG fd/rawPk and
	//    fdRawDir score exactly this channel.
	//  - fine J sweep 3.5/4/4.5/5 flat within noise; J=2 and J=6 both
	//    worse (smear vs erratic direction); Ja 400/1500/4000 flat,
	//    Ja=100 worse. nothing left with a mechanism worth a session.
	//  - the RTS smoother (kalmanSmoothLagMs) is the right tool for a
	//    game that reads vecVelocity (relOut/rawPk 0.87-0.97 at 5-7deg)
	//    and the wrong one for pose-history games (0.80-0.91, +latency).
	double kalmanCaJerk = 4.0;
	// Ja NOTE 2026-08-24: every Ja/O ratification above was scored while
	// vecAngularVelocity was reported in the wrong (world) frame, a setup
	// that rewards a laggier w because vrserver rotated about the wrong
	// axis by |w|*dt. with the frame fixed the felt optimum has probably
	// moved toward snappier. 1500 kept until re-swept (1500/3000/6000 on
	// the B&S horizontal swing: attached at the peak, no overshoot on stop).
	double kalmanCaAngJerk = 1500.0;
	// CA-full measurement noise, separate from the CV knobs so tuning
	// one mode never disturbs the other's field-proven values.
	// field-derived (session 5): P=4.2 was best-or-tied at the release
	// instant for BOTH J=10 and J=17 (rel/pk 1.00, relOff 0.0deg,
	// zero >30deg releases), P=3 measured pathological, P=5.7 fine but
	// no better. still deliberately overstates the sensor — this knob
	// is the mode's smoothness dial, not an honest noise estimate.
	// 2026-08-16: honest 1.5mm ratified — viable now that dup distrust
	// is floored in absolute terms (it no longer weakens when this
	// shrinks) and adaptiveR no longer stacks against it.
	// 2026-08-17: treat as the sensor's noise, fixed. only J/P matters
	// for behavior (offline: J=4/P=1.5 and J=11.2/P=4.2 give the same
	// gains to within the dup floor); if a different tracker needs a
	// different P, scale J with it to keep the ratio.
	double kalmanCaPosNoiseMm = 1.5;
	// 5.75 field-preferred over 1.25 (2026-08-14): instruments show a
	// fatter direction tail at 1.25 (dirOff max 177 vs 92); the felt
	// benefit likely lives in the smoother q/pose stream that
	// pose-history games fit — PEAKDIAG does not score that channel.
	// 2026-08-16: 1.5 ratified alongside the honest linear noise.
	// 2026-08-17: same rule as P — fixed sensor noise; Ja/O is the knob.
	// 2026-08-24: see the Ja note - ratified under the wrong w frame.
	double kalmanCaOriNoiseDeg = 1.5;
	// shared acceleration decay time constant (CA-full, both channels).
	// 2026-08-16: 20ms ratified (with exactCov the low-tau covariance is
	// consistent; 150 was only ever better under the legacy covariance
	// and inflated-R regime).
	double kalmanCaAccelTauMs = 20.0;
	// CA-M fast magnitude channel knobs
	double kalmanCaMagJerk = 800.0;
	double kalmanCaMagAccelTauMs = 150.0;
	// CA-full only: also report the acceleration states in
	// vecAcceleration / vecAngularAcceleration. the runtime's forward
	// prediction integrates them, so honest accel can cut rendered-hand
	// latency — but it doubles prediction overshoot risk, hence its own
	// toggle, off for the first clean A/B.
	bool kalmanCaReportAccel = false;
	// A/B experiment: propagate the CA covariance with the SAME Singer
	// transition the state actually uses (F12 = tau(1-e^(-dt/tau)) instead
	// of dt, F02 = dt*F12/2 instead of dt^2/2). the legacy covariance
	// (this knob off) uses the naive CA transition, which at tau near the
	// 20ms floor over-weights measurements by up to ~25% relative to the
	// model — consistent gains recalibrate NIS and shift the effective
	// meaning of the tuned J/P/O knobs slightly. off = bit-identical to
	// the field-tuned 8.5 behavior.
	// 2026-08-16: ON ratified as default (field-neutral at high tau,
	// correct at the ratified tau=20).
	bool kalmanCaExactCov = true;
	// ==== grip-point velocity compensator ====
	// the estimator honestly reports the TRACKED ORIGIN's velocity; during
	// a wrist snap that includes the origin's tangential velocity w x r
	// about the hand's rotation center — physics, not filter error (field
	// 2026-08-13/14: linear throws 1-3deg dirOff, real throws 4-15deg,
	// flick-only gestures 1.1-1.7x linear speed with scattered direction;
	// per-throw inversion of the lever hypothesis clusters at r ~ 5cm).
	// when enabled the reported linear velocity is transported to the
	// grip point: v_out = v + blend * (w x R(q) rGrip), rGrip per hand in
	// the controller's LOCAL frame (cm). the shadow instrumentation in
	// PEAKDIAG (gOut/gDirOff/wr fields) runs whenever rGrip is nonzero,
	// even with enable off — verify on a session before flipping it on.
	// DEFAULT OFF: engines that transport velocity to their own attach
	// point would double-apply. rGrip comes from the aligner's grip
	// capture (pure wrist swirl with the palm held still — the pivot
	// solve's stationary point IS the rotation center) or manual entry.
	bool kalmanGripEnable = false;
	double kalmanGripBlend = 1.0;
	double kalmanGripLeftCm[3] = {0, 0, 0};
	double kalmanGripRightCm[3] = {0, 0, 0};
	// experimental throw/velocity fix. vrlink's reported controller velocity
	// is heavily smoothed (field data: peaks read ~50-65% of position-derived
	// velocity during throws, ratio varies with motion phase = filter lag,
	// not a scale factor). when enabled, linear velocity is recomputed from
	// a ~50ms window of positions and substituted when meaningfully larger
	// than the reported value, so releases carry true peak speed while calm
	// motion keeps the driver's smoother data.
	bool velocityFix = false;
	// diagnostic: throttle-log controller/tracker poses from the PoseUpdated
	// hook (position, velocity, tracking result), with a burst mode that
	// captures high-velocity moments (throws). live-reloaded, so it can be
	// toggled mid-session. groundwork for the throw/velocity fix.
	bool poseLogging = false;
	// sub-gate for the HIGH-RATE burst channel of pose logging (up to
	// 100Hz/device during fast motion through DriverLog on the pose hot
	// path). field 2026-08-16: burst storms during hard right-hand
	// throws (10.9k lines/session on one device) correlate with
	// game/stream hitches — synchronous log I/O at exactly the worst
	// moment. steady 2s lines and event diagnostics stay under
	// poseLogging alone; bursts now additionally require this, default
	// OFF so release configs never storm.
	bool poseLogBurst = false;
	// GUI-only fence: retired experimental knobs render in the GUI's
	// Graveyard section only when this is set by hand in settings.json.
	// no GUI knob on purpose. values of archived knobs stay ACTIVE
	// regardless — hiding is not disabling.
	bool graveyardEnable = false;
};

// pose adjustments for streamed controllers, applied in the PoseUpdated hook.
// rotation is a local frame euler offset in degrees (x = pitch: positive
// tilts the top of the controller back toward the user), position is a local
// frame offset in cm. lets the grip/aim angle be matched to what games
// expect from other controller types. live reloaded.
// in-headset controller offset aligner. manual mode: sticks adjust the
// selected axis of the selected group (position/rotation) live. automatic
// mode: plant the controller tip on any solid surface, hold the trigger and
// swirl a cone around the planted tip; a least-squares pivot solve recovers
// the position offset (the drawn tip marker freezing is the confirmation).
struct ControllerAlignerConfig{
	bool enable = false;
};

struct ControllersConfig{
	// mixed-space velocity frame fix for playspace-override setups (e.g.
	// lighthouse controllers aligned into the vrlink space): the openvr
	// header leaves DriverPose_t::vecVelocity's frame unspecified while
	// positions are driver-space + WorldFromDriver. with a large alignment
	// yaw the two conventions diverge and thrown objects fly at the right
	// speed in the WRONG direction. 0 = off, 1 = "world" (rotate reported
	// velocity by qWorldFromDriverRotation), 2 = "driver" (inverse).
	// applied only to devices whose WorldFromDriver rotation deviates from
	// identity by more than ~2 degrees, so vanilla devices are untouched.
	// the field test decides which mode matches vrserver's real convention.
	int spaceVelocityFixMode = 0;
	// when true, the pose offsets below describe the LEFT controller and are
	// mirrored for the right hand (position X negated; rotation Y/Z negated).
	// physical controller pairs are mirror images, so the displacement
	// between the tracked origin and the grip is mirrored too - identical
	// offsets can only ever fit one hand.
	#ifdef VENDOR_GALAXYXR
	// measured asymmetric residual of vrlink's controller pose, validated
	// against camera passthrough (virtual model overlaid on the physical
	// controller): 5deg yaw + 0.5cm lateral, mirrored per hand. the large
	// hand-symmetric piece (22deg pitch, 5cm Z) is the fixed gripConvention
	// transform; this residual rides the mirror-aware offset layer instead
	// because a hand-dependent fixed transform would make the grip-component
	// rebase non-pure-X (unverifiable euler-order assumptions). unlike the
	// convention piece, the residual cannot double-apply anywhere: the grip
	// components contain no yaw/lateral terms to duplicate, and the rebase
	// algebra delivers it exactly once to grip-pose-selecting bindings
	// (conjugated by the 22deg pitch: the X-translation is exactly
	// invariant, the yaw axis tilts sub-perceptibly). safe always-on.
	bool mirrorOffsetsForRightHand = true;
	double rotationOffsetDeg[3] = {0, 5, 0};
	double positionOffsetCm[3] = {0.5, 0, 0};
	#else
	bool mirrorOffsetsForRightHand = false;
	double rotationOffsetDeg[3] = {0, 0, 0};
	double positionOffsetCm[3] = {0, 0, 0};
	#endif
	// per-hand residual trims (2026-08-24): vrlink's left and right raw
	// origins are not exact mirror images (field: left yaw off, left
	// translated slightly right, with only a Z trim active). these are
	// applied UNMIRRORED, per hand, after the shared (mirrored) offsets
	// above. same axis conventions as the shared offsets. live reloaded.
	double leftRotationOffsetDeg[3] = {0, 0, 0};
	double leftPositionOffsetCm[3] = {0, 0, 0};
	double rightRotationOffsetDeg[3] = {0, 0, 0};
	double rightPositionOffsetCm[3] = {0, 0, 0};
	ControllerAlignerConfig aligner = {};
};

struct CustomShaderConfig{
	// if shaders should be replaced in the compositor
	bool enable = false;
	bool enableForMeganeX8K = true;
	bool enableForDreamAir = true;
	bool enableForOther = false;
	// contrast with 50 being normal
	double contrast = 50;
	// the point from 0-100% of white that the contrast is centered around
	double contrastMidpoint = 50;
	// if the contrast should be done in linear space instead of gamma
	bool contrastLinear = false;
	// if per eye contrast should be applied
	bool contrastPerEye = false;
	bool contrastPerEyeLinear = false;
	double contrastLeft = 50;
	double contrastMidpointLeft = 50;
	double contrastRight = 50;
	double contrastMidpointRight = 50;
	// increase or decrease the variation of the colors
	double saturation = 50;
	// gamma of the output
	double gamma = 2.2;
	// if the subpixels should be offset
	bool subpixelShift = true;
	// if the mura correction should be skipped
	bool disableMuraCorrection = false;
	// if the black levels should be skipped
	bool disableBlackLevels = false;
	// if the colors should be corrected to display the srgb input as srgb on the display
	bool srgbColorCorrection = false;
	// if the white point correction should be applied to the srgb color correction
	bool srgbWhitePointCorrection = false;
	// a 3x3 matrix to apply to the linear colors
	// if this is an array of 9 flat elements it will override the headset's default matrix
	std::vector<double> srgbColorCorrectionMatrix = {};
	// correct color uniformity issues of the lenses on the MeganeX
	bool lensColorCorrection = true;
	// if a 10 bit input will be dithered down to 8 bit
	bool dither10Bit = false;
	// if the filter should be enabled for overlays (defaults false to avoid performance hit when no overlay is shown)
	bool enableFilterForOverlay = false;
	// if the filter should be enabled when the SteamVR dashboard is open
	bool enableFilterForDashboard = true;
	// filters on the sampling of the texture,  "None", "NearestNeighbor", "FXAA2", "FXAA2CAS", "LumaSharpen", and "CAS"
	std::string samplingFilter = "None";
	// FXAA2 filter parameters
	double samplingFilterFXAA2SharpenStrength = 1.0;
	double samplingFilterFXAA2SharpenClamp = 0.05;
	// FXAA2CAS filter parameters
	double samplingFilterFXAA2CASStrength = 1.0;
	double samplingFilterFXAA2CASContrast = 1.0;
	// luma sharpen filter parameters
	double samplingFilterLumaSharpenStrength = 2.0;
	double samplingFilterLumaSharpenClamp = 0.1;
	int samplingFilterLumaSharpenPattern = 1;
	double samplingFilterLumaSharpenRadius = 1.0;
 	// CAS filter parameters
 	double samplingFilterCASStrength = 1.0;
 	double samplingFilterCASContrast = 1.0;
	// color multiplier for tint adjustments
	ConfigColor colorMultiplier = {1.0, 1.0, 1.0};
};


class Config{
public:
	enum HeadsetType{
		None = 0,
		Other = 1,
		MeganeX8K = 2,
		Vive = 3,
		DreamAir = 4,
	};
	
	class BaseHeadsetConfig{
	public:
		// if the headset should be shimmed by this driver
		bool enable = true;
		// the type of headset this is
		HeadsetType headsetType = HeadsetType::None;
		// ipd in mm
		double ipd = 63.0;
		// ipd offset from the ipd value in mm
		double ipdOffset = 0.0;
		// horizontal offset in mm to shift both eyes to the right
		double horizontalIPDOffset = 0.0;
		// minimum black levels from 0 to 1
		double blackLevel = 0;
		// tint the display this color
		ConfigColor colorMultiplier = {};
		// distortion profile to use
		std::string distortionProfile = "None";
		// amount to zoom in the distortion profile
		double distortionZoom = 1.0;
		// amount to zoom in the FOV, the fov is divided by this value
		double fovZoom = 1.0;
		// amount to zoom in the FOV using tangent-based scaling for flatter perception
		double flatFovZoom = 1.0;
		// multiplier for the subpixel offsets
		double subpixelShift = 1.0;
		// subpixel offsets in pixel units for each color channel [offsetXRed, offsetYRed, offsetXGreen, offsetYGreen, offsetXBlue, offsetYBlue]
		std::vector<double> subpixelOffsets = {0, 0, 0, 0, 0, 0};
		// width of one eye in pixels
		int resolutionX = 3840;
		// height of one eye in pixels
		int resolutionY = 3552;
		// clockwise rotation of the image on the right display, 0:0, 1:90, 2:180, 3:270
		int displayRotation = 0;
		// max horizontal fov
		double maxFovX = 100.0;
		// max vertical fov
		double maxFovY = 96.0;
		// distortion mesh resolution
		int distortionMeshResolution = 127;
		// if the fov should be slightly adjusted each session to prevent sharp burn in along the edges
		bool fovBurnInPrevention = true;
		// if the distortion profile should clamp the image to the bounds of the display or if it will instead render an image at whatever FOV is set
		bool fovClamping = true;
		// device type used to filter distortion profiles in the GUI
		std::string distortionProfileDeviceType = "";
		// multiply 100% render resolution width
		double renderResolutionMultiplierX = 1.0;
		// multiply 100% render resolution height
		double renderResolutionMultiplierY = 1.0;
		// percent of 1:1 resolution to apply the super sampling downscale filter at, this is really high to allow for subpixel sampling
		double superSamplingFilterPercent = 500;
		// seconds of latency to the display
		double secondsFromVsyncToPhotons = 0.007;
		// seconds from the the first to last line of the display
		double secondsFromPhotonsToVblank = 0.0025;
		// angle in degrees for each eye to be rotated outwards
		double eyeRotation = 0.0;
		// disable eyes as much as possible. 0:both enabled 1:left disabled 2:right disabled 3:both disabled
		int disableEye = 0;
		// if the fov should be decreased for the disabled eye, this causes problems in some apps
		bool disableEyeDecreaseFov = false;
		// if a vive link box should be used for bluetooth
		bool useViveBluetooth = false;
		// if the display is in direct mode or false if it is on the desktop
		bool directMode = true;
		// if the icons in the SteamVR status window should be modified
		bool replaceIcons = true;
		// the edid for the headset
		int edidVendorId = 0;
		// the edid for the headset
		int edidProductId = 0;
		// if non zero, override the edid vendor id
		int edidVendorIdOverride = 0;
		// if non zero, override the edid product id
		int edidProductIdOverride = 0;
		// DSC Version
		int dscVersion = -1;
		// DSC Slice count
		int dscSliceCount = -1;
		// DSC bits per pixel
		int dscBPPx16 = -1;
		// if the driver should be enabled for every hmd
		bool forceEnable = false;
		// if parallel projection should be used for rendering
		bool parallelProjection = true;
		// if eye tracking should be enabled
		bool enableEyeTracking = false;
		// Config struct for the hidden area mesh
		HiddenAreaMeshConfig hiddenArea;
		// config for dimming the display when stationary
		StationaryDimmingConfig stationaryDimming = {};
	};
	
	class MeganeX8KConfig : public BaseHeadsetConfig{
	public:
		MeganeX8KConfig(){
			headsetType = HeadsetType::MeganeX8K;
			distortionProfile = "MeganeX8K Default";
			distortionProfileDeviceType = "MeganeX8K";
			edidVendorId = 0xcc4c; // SFL
			displayRotation = 1;
			subpixelOffsets = {-0.33 / 3552.0, 0, 0, 0, 0.33 / 3552.0, 0};
		}
	};
	// config for the MeganeX superlight 8K
	MeganeX8KConfig meganeX8K = {};
	
	class DreamAirConfig : public BaseHeadsetConfig{
		public:
		DreamAirConfig(){
			headsetType = HeadsetType::DreamAir;
			distortionProfile = "Dream Air Default";
			distortionProfileDeviceType = "DreamAir";
			maxFovX = 96;
			maxFovY = 86;
			edidVendorId = 53826; // PVR
			displayRotation = 3;
			subpixelOffsets = {0.33 / 3552.0, 0, 0, 0, -0.33 / 3552.0, 0};
			eyeRotation = 2;
			enableEyeTracking = true;
		}
	};
	// config for the Dream Air
	DreamAirConfig dreamAir = {};
	
	class FakeHeadsetConfig : public BaseHeadsetConfig{
		public:
		FakeHeadsetConfig(){
			enable = false;
			headsetType = HeadsetType::Other;
			distortionProfile = "MeganeX8K Default";
			displayRotation = 0;
			// use a 1080p monitor
			directMode = false;
			resolutionX = 960;
			resolutionY = 1080;
		}
	};
	// config for the fake headset
	FakeHeadsetConfig fakeHeadset = {};
	
	class GeneralHeadsetConfig{
	public:
		// if a vive link box should be used for bluetooth
		bool useViveBluetooth = false;
	};
	GeneralHeadsetConfig generalHeadset = {};
	
	CustomShaderConfig customShader = {};
	
	// processing of direct mode layer textures before a streaming driver (e.g.
	// vrlink / Steam Link) consumes them. this is the always on path for
	// headsets whose driver composites frames itself, where the compositor
	// shader replacement only runs while the dashboard is open.
	StreamFrameConfig streamFrame = {};
	GalaxyXrConfig galaxyXr = {};
	
	// streamed controller pose adjustments
	ControllersConfig controllers = {};
	
	// if devices should always be reported as tracking
	bool forceTracking = false;
	
	// if the screenshot requests should cause full compositor debug screenshots to be taken
	bool takeCompositorScreenshots = false;
	
	// makes the diver only do things related to closed source functionality if it exits
	// this allows for a driver built from source to run along side the driver with proprietary code
	bool onlyHandlePrivateFunctionality = false;
	
	// reload the config every time a file is changed in the distortions directory
	// this is for manual json editing, utilities should touch the main settings file when done modifying distortions instead
	// this is now enabled by default
	// bool watchDistortionProfiles = false;
	
	// if the config has been changes and should be reloaded
	// this will be set the false at the end of RunFrame
	bool hasBeenUpdated = true;
	
};

// config for a single custom distortion profile
class DistortionProfileConfig{
public:
	// name of distortion profile, this will be it's filename
	std::string name = "None";
	// the headset device this profile is for, empty for all devices, or "MeganeX8K" for the MeganeX superlight 8K
	std::string device = "";
	// description to display
	std::string description = "";
	// author of the distortion profile
	std::string author = "";
	// the date when it was created
	double creationDate = 0;
	// last time it was modified, used for reloading if changed
	double modifiedTime = 0;
	// type of distortion, None or RadialBezier
	std::string type = "None";
	// main distortion
	std::vector<double> distortions = {};
	// additional distortion to apply to the red channel
	std::vector<double> distortionsRed = {};
	// additional distortion to apply to the blue channel
	std::vector<double> distortionsBlue = {};
	// offset image outwards on the display using the same 0 to 100 scale 
	float offsetX = 0.0f;
	// offset image upwards on the display using the same 0 to 100 scale
	float offsetY = 0.0f;
	// if legacy smoothing should be used for bezier curves
	bool legacySmoothing = false;
	// amount to smooth the curve from 0 to 1 for legacy smoothing
	double smoothAmount = 0.66;
};

// global config object
extern Config driverConfig;

// config from before the last reload
extern Config driverConfigOld;

// config with default values
extern Config defaultDriverConfig;

// lock for the config to prevent updates while reading
extern std::mutex driverConfigLock;

// version of the application
extern std::string driverVersion;


#if __has_include("../Driver/HidModifierPrivate.cpp")
#define HAS_PRIVATE 1
#endif