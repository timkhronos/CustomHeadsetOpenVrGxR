#include "GalaxyXR.h"
#include <algorithm>
#include <vector>
#include <chrono>
#include "../Config/ConfigLoader.h"
#include "../Config/StreamTiers.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include "nlohmann/json.hpp"

// helper: set a string property only when it differs, returns true if written
static bool SetStringIfDifferent(vr::PropertyContainerHandle_t container, vr::ETrackedDeviceProperty prop, const std::string &value){
	std::string current = vr::VRProperties()->GetStringProperty(container, prop);
	if(current == value){
		return false;
	}
	vr::VRProperties()->SetStringProperty(container, prop, value.c_str());
	return true;
}

// prefix is one of headset_galaxy_xr_status / left_galaxy_xr_status /
// right_galaxy_xr_status (icon set by Vilkka, see icons/galaxy_xr/CREDITS.txt)
// returns true if any icon property was (re)written. the "ready" path is
// also the drift sentinel RunFrame polls: vrlink picks controller status
// icons from the HMD family and can rewrite them after we did (field
// 2026-08-25: with a samsung-default hmd_config the controller icons did
// not stick while models and bindings did), and that rewrite does not
// always reach us as a PropertyChanged event.
static bool SetDeviceIcons(vr::PropertyContainerHandle_t container, const std::string &prefix){
	std::string base = "{" + driverConfigLoader.info.driverName + "}/icons/galaxy_xr/" + prefix;
	bool wrote = false;
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceOff_String,            base + "_off.png");
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceSearching_String,      base + "_searching.gif");
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceSearchingAlert_String, base + "_searching_alert.gif");
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceReady_String,          base + "_ready.png");
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceReadyAlert_String,     base + "_ready_alert.png");
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceNotReady_String,       base + "_error.png");
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceStandby_String,        base + "_standby.png");
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceStandbyAlert_String,   base + "_standby_alert.png");
	wrote |= SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceAlertLow_String,       base + "_ready_low.png");
	return wrote;
}
static std::string ExpectedReadyIcon(const std::string &prefix){
	return "{" + driverConfigLoader.info.driverName + "}/icons/galaxy_xr/" + prefix + "_ready.png";
}

// the Galaxy XR native per-eye render geometry; see GalaxyXrConfig::nativeResolution
static const int kGalaxyXrRenderWidth = 3552;
static const int kGalaxyXrRenderHeight = 3840;

// tier table lives in Config/StreamTiers.h (shared with the NVENC tap
// resolution in FrameProcessor)
static const std::initializer_list<int> kAllTierWidths = {1536, 2048, 2560, 3072, 3200, 3584, 4032};
static const std::initializer_list<int> kAllTierBandwidths = {250, 300, 350, 400, 450, 500};


// remove a vrlink int key only when it holds a value we could have written,
// so user- or tool-owned values are never clobbered
static void RemoveIntIfOurs(const char* key, const std::vector<int> &ourValues){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	int32_t v = vr::VRSettings()->GetInt32("driver_vrlink", key, &err);
	if(err != vr::VRSettingsError_None){ return; }
	for(int ours : ourValues){
		if(v == ours){
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", key);
			return;
		}
	}
}

// write encodeWidth + streamFormatWidth (always equal, see kStreamTiers) and
// the bandwidth pair, with vrlink's automatic pickers off
struct VrlinkPathProbe {
	bool done = false;
	double lastAttempt = 0;
};
static VrlinkPathProbe pathProbe;

static void WriteStreamKeys(int width, int bandwidth, int streamFormatWidth = 0){
	if(streamFormatWidth <= 0){ streamFormatWidth = width; }
	vr::VRSettings()->SetInt32("driver_vrlink", "encodeWidth", width);
	vr::VRSettings()->SetInt32("driver_vrlink", "streamFormatWidth", streamFormatWidth);
	vr::VRSettings()->SetBool("driver_vrlink", "automaticStreamFormatWidth", false);
	vr::VRSettings()->SetBool("driver_vrlink", "automaticBandwidth", false);
	vr::VRSettings()->SetInt32("driver_vrlink", "recommendedBandwidthMbit", bandwidth);
	vr::VRSettings()->SetInt32("driver_vrlink", "targetBandwidth", bandwidth);
}

// v3: the effective tile width and bandwidth for the current mode. encode
// width is a constant (inert in foveated mode); the Advanced bandwidth
// override wins over tier/custom and drives pacer AND encoder.
int GalaxyXR_EffectiveTileWidth(){
	const auto &g = driverConfig.galaxyXr;
	const GxrStreamTier* tier = FindGxrStreamTier(g.streamQuality);
	int w = g.streamQuality == "custom" ? g.customStreamFormatWidth : (tier ? tier->width : 1536);
	return std::max(512, std::min(2048, w));
}
int GalaxyXR_EffectiveBandwidthMbit(){
	const auto &g = driverConfig.galaxyXr;
	if(driverConfig.streamFrame.nvencBandwidthOverrideMbit > 0){ return std::max(10, std::min(2000, driverConfig.streamFrame.nvencBandwidthOverrideMbit)); }
	const GxrStreamTier* tier = FindGxrStreamTier(g.streamQuality);
	int bw = g.streamQuality == "custom" ? g.customBandwidthMbit : (tier ? tier->bandwidthMbit : 350);
	return std::max(10, std::min(2000, bw));
}

static void ApplyStreamQualitySetting(){
	pathProbe.done = false;
	const auto &g = driverConfig.galaxyXr;
	const GxrStreamTier* tier = FindGxrStreamTier(g.streamQuality);
	if(g.streamQuality == "custom" || tier){
		int encodeW = std::max(512, std::min(8192, g.customEncodeWidth > 0 ? g.customEncodeWidth : 3072));
		int tile = GalaxyXR_EffectiveTileWidth();
		int bw = GalaxyXR_EffectiveBandwidthMbit();
		WriteStreamKeys(encodeW, bw, tile);
		DriverLog("GalaxyXR: stream quality '%s'%s: tile (streamFormatWidth) %d, %d Mbit/s%s, encodeWidth %d (inert); encoder settings are global (see NvencTap); effective next start/connect",
			tier ? tier->name : "custom", (tier && tier->name != g.streamQuality) ? " (mapped from legacy name)" : "", tile, bw,
			driverConfig.streamFrame.nvencBandwidthOverrideMbit > 0 ? " (Advanced bandwidth override)" : "", encodeW);
	}else{
		// default (or unknown): remove tier keys we own so vrlink built-in
		// defaults apply, matching the community tool's Default mode.
		// custom values (current and legacy) are included so leaving custom
		// mode cleans up too
		std::vector<int> widths(kAllTierWidths);
		widths.push_back(g.customEncodeWidth);
		widths.push_back(g.customStreamFormatWidth);
		widths.push_back(3072);
		std::vector<int> bws(kAllTierBandwidths);
		bws.push_back(g.customBandwidthMbit);
		bws.push_back(driverConfig.streamFrame.nvencBandwidthOverrideMbit);
		RemoveIntIfOurs("encodeWidth", widths);
		RemoveIntIfOurs("streamFormatWidth", widths);
		RemoveIntIfOurs("recommendedBandwidthMbit", bws);
		RemoveIntIfOurs("targetBandwidth", bws);
		vr::EVRSettingsError err = vr::VRSettingsError_None;
		bool a = vr::VRSettings()->GetBool("driver_vrlink", "automaticStreamFormatWidth", &err);
		if(err == vr::VRSettingsError_None && !a){
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "automaticStreamFormatWidth");
		}
		err = vr::VRSettingsError_None;
		a = vr::VRSettings()->GetBool("driver_vrlink", "automaticBandwidth", &err);
		if(err == vr::VRSettingsError_None && !a){
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "automaticBandwidth");
		}
	}
}

// ---------------------------------------------------------------------------
// vrlink runtime path recon. vrlink publishes (not reads) a handful of values
// under /driver_vrlink/ that the SteamVR settings UI binds to:
//   min_stream_format_width / max_stream_format_width  (bounds of the
//       "foveated stream format width" slider, step 64) - this is the
//       "1536 foveated transport maximum" the community observed; it is a
//       cap the driver reports, not a settings key anyone writes
//   foveation_enabled / effective_foveated_resolution
//   network_tested_bandwidth (detent on the bandwidth slider)
// values exist only once the headset is connected, so this polls until they
// resolve and logs them once. compare max_stream_format_width against the
// streamFormatWidth we wrote to see whether the request is being clamped.
// ---------------------------------------------------------------------------
// IVRPaths is not in the vendored openvr_driver.h. The probe is opt-in:
// define GXR_VRPATHS_PROBE to build it against this local declaration of
// IVRPaths_001 (layout from the upstream openvr_driver.h; unverified here).
#ifdef GXR_VRPATHS_PROBE
namespace gxrpaths {
	typedef uint64_t PathHandle_t;
	struct PathRead_t {
		PathHandle_t ulPath;
		void *pvBuffer;
		uint32_t unBufferSize;
		vr::PropertyTypeTag_t unTag;
		uint32_t unRequiredBufferSize;
		vr::ETrackedPropertyError eError;
		const char *pszPath;
	};
	struct PathWrite_t;
	class IVRPaths {
	public:
		virtual vr::ETrackedPropertyError ReadPathBatch(vr::PropertyContainerHandle_t ulRootHandle, PathRead_t *pBatch, uint32_t unBatchEntryCount) = 0;
		virtual vr::ETrackedPropertyError WritePathBatch(vr::PropertyContainerHandle_t ulRootHandle, PathWrite_t *pBatch, uint32_t unBatchEntryCount) = 0;
		virtual vr::ETrackedPropertyError StringToHandle(PathHandle_t *pHandle, char *pchPath) = 0;
		virtual vr::ETrackedPropertyError HandleToString(PathHandle_t pHandle, char *pchBuffer, uint32_t unBufferSize, uint32_t *punBufferSizeUsed) = 0;
	};
	static const char* const IVRPaths_Version = "IVRPaths_001";
	static const vr::PropertyContainerHandle_t kRoot = 1; // k_ulRootHandle upstream
}

template<typename T>
static bool ReadPathAs(gxrpaths::IVRPaths* paths, gxrpaths::PathHandle_t h, vr::PropertyTypeTag_t tag, T &v){
	gxrpaths::PathRead_t r{}; r.ulPath = h; r.unTag = tag; r.pvBuffer = &v; r.unBufferSize = sizeof(v);
	return paths->ReadPathBatch(gxrpaths::kRoot, &r, 1) == vr::TrackedProp_Success && r.eError == vr::TrackedProp_Success;
}

static bool ReadVrlinkPath(gxrpaths::IVRPaths* paths, const char* path, std::string &out){
	gxrpaths::PathHandle_t h = 0;
	std::string mutablePath = path;
	if(paths->StringToHandle(&h, &mutablePath[0]) != vr::TrackedProp_Success){ return false; }
	char buf[64];
	{ int32_t v = 0; if(ReadPathAs(paths, h, vr::k_unInt32PropertyTag, v)){ out = std::to_string(v); return true; } }
	{ double v = 0; if(ReadPathAs(paths, h, vr::k_unDoublePropertyTag, v)){ snprintf(buf, sizeof(buf), "%g", v); out = buf; return true; } }
	{ float v = 0; if(ReadPathAs(paths, h, vr::k_unFloatPropertyTag, v)){ snprintf(buf, sizeof(buf), "%g", v); out = buf; return true; } }
	{ bool v = false; if(ReadPathAs(paths, h, vr::k_unBoolPropertyTag, v)){ out = v ? "true" : "false"; return true; } }
	{ char v[128] = {}; if(ReadPathAs(paths, h, vr::k_unStringPropertyTag, v)){ out = v; return true; } }
	return false;
}

// call every frame; self-throttled. resets when stream settings change so a
// reconnect re-logs the (possibly new) cap.
static void ProbeVrlinkPaths(){
	if(pathProbe.done){ return; }
	double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if(now - pathProbe.lastAttempt < 3.0){ return; }
	pathProbe.lastAttempt = now;
	vr::EVRInitError ie = vr::VRInitError_None;
	auto* paths = (gxrpaths::IVRPaths*)vr::VRDriverContext()->GetGenericInterface(gxrpaths::IVRPaths_Version, &ie);
	if(!paths || ie != vr::VRInitError_None){ return; }
	static const char* kPaths[] = {
		"/driver_vrlink/min_stream_format_width", "/driver_vrlink/max_stream_format_width",
		"/driver_vrlink/foveation_enabled", "/driver_vrlink/effective_foveated_resolution",
		"/driver_vrlink/network_tested_bandwidth", "/driver_vrlink/client_version",
	};
	std::string line; int got = 0;
	for(const char* pth : kPaths){
		std::string v;
		if(ReadVrlinkPath(paths, pth, v)){ ++got; line += std::string(pth + 15) + "=" + v + " "; }
	}
	if(line.find("max_stream_format_width=") == std::string::npos){ return; }
	vr::EVRSettingsError se = vr::VRSettingsError_None;
	int32_t sfw = vr::VRSettings()->GetInt32("driver_vrlink", "streamFormatWidth", &se);
	DriverLog("GalaxyXR: vrlink runtime paths (%d/6): %s| our streamFormatWidth=%d", got, line.c_str(),
		se == vr::VRSettingsError_None ? sfw : -1);
	pathProbe.done = true;
}
#else
static void ProbeVrlinkPaths(){}
#endif

// ---------------------------------------------------------------------------
// vrlink per-headset profile section ("vrlink_<modelNumber>"), see
// GalaxyXrConfig::vrlinkHeadsetProfile. only writes on difference; on
// removal only removes keys still holding our values.
// ---------------------------------------------------------------------------
static void SetInt32IfDifferent(const char* section, const char* key, int32_t v){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	int32_t cur = vr::VRSettings()->GetInt32(section, key, &err);
	if(err != vr::VRSettingsError_None || cur != v){ vr::VRSettings()->SetInt32(section, key, v); }
}
static void SetBoolIfDifferent(const char* section, const char* key, bool v){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	bool cur = vr::VRSettings()->GetBool(section, key, &err);
	if(err != vr::VRSettingsError_None || cur != v){ vr::VRSettings()->SetBool(section, key, v); }
}
static void RemoveBoolIfOurs(const char* section, const char* key, bool ours){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	bool cur = vr::VRSettings()->GetBool(section, key, &err);
	if(err == vr::VRSettingsError_None && cur == ours){ vr::VRSettings()->RemoveKeyInSection(section, key); }
}
static void RemoveIntIfOursIn(const char* section, const char* key, const std::vector<int> &ours){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	int32_t cur = vr::VRSettings()->GetInt32(section, key, &err);
	if(err != vr::VRSettingsError_None){ return; }
	for(int o : ours){ if(cur == o){ vr::VRSettings()->RemoveKeyInSection(section, key); return; } }
}

// vrlink's native model number for the Galaxy XR ("ModelNumber xrvst2ue" in
// vrserver.txt). the property container is still empty when our Activate
// runs (vrlink stamps it afterwards), so this is the fallback for the
// section name.
static const char* kGalaxyXrVrlinkModelNumber = "xrvst2ue";

static void ApplyHeadsetProfileSetting(const std::string &modelNumberIn){
	const auto &g = driverConfig.galaxyXr;
	std::string modelNumber = modelNumberIn.empty() ? kGalaxyXrVrlinkModelNumber : modelNumberIn;
	if(modelNumberIn.empty()){
		DriverLog("GalaxyXR: headset profile: model number not yet in the container, using vrlink's native '%s'", kGalaxyXrVrlinkModelNumber);
	}
	// v3: the profile's max tracks the tile we write (no separate field);
	// 2048 is the hard tile ceiling anyway (4 stacked tiles, 8192 max height)
	int maxSfw = std::max(1024, GalaxyXR_EffectiveTileWidth());
	DriverLog("GalaxyXR: headset profile apply: section vrlink_%s profile=%d maxSfw=%d (tracks tile) supports10bit=%d overlay=%d",
		modelNumber.c_str(), (int)g.vrlinkHeadsetProfile, maxSfw, (int)g.profileSupports10bit, (int)g.vrlinkDebugOverlay);
	std::string section = "vrlink_" + modelNumber;
	const char* sec = section.c_str();
	if(g.vrlinkHeadsetProfile){
		SetInt32IfDifferent(sec, "recommendedRenderWidth", kGalaxyXrRenderWidth);
		SetInt32IfDifferent(sec, "recommendedRenderHeight", kGalaxyXrRenderHeight);
		SetBoolIfDifferent(sec, "supports10bit", g.profileSupports10bit);
		SetInt32IfDifferent(sec, "minStreamFormatWidth", 1024);
		SetInt32IfDifferent(sec, "maxStreamFormatWidth", maxSfw);
		SetInt32IfDifferent(sec, "minNonFoveatedStreamFormatWidth", 1024);
		SetInt32IfDifferent(sec, "maxNonFoveatedStreamFormatWidth", maxSfw);
		DriverLog("GalaxyXR: wrote vrlink headset profile [%s] (render %dx%d, supports10bit %d, streamFormatWidth 1024..%d; effective at SteamVR start). "
			"Verify in driver_vrlink.txt: 'Found settings for unknown hmd' and 'Using 10bit mode'.",
			sec, kGalaxyXrRenderWidth, kGalaxyXrRenderHeight, (int)g.profileSupports10bit, maxSfw);
	}else{
		RemoveIntIfOursIn(sec, "recommendedRenderWidth", {kGalaxyXrRenderWidth});
		RemoveIntIfOursIn(sec, "recommendedRenderHeight", {kGalaxyXrRenderHeight});
		RemoveBoolIfOurs(sec, "supports10bit", true);
		RemoveBoolIfOurs(sec, "supports10bit", false);
		RemoveIntIfOursIn(sec, "minStreamFormatWidth", {1024});
		RemoveIntIfOursIn(sec, "maxStreamFormatWidth", {maxSfw, 1536, 2048, 3072, 3200, 3584, 4096});
		RemoveIntIfOursIn(sec, "minNonFoveatedStreamFormatWidth", {1024});
		RemoveIntIfOursIn(sec, "maxNonFoveatedStreamFormatWidth", {maxSfw, 1536, 2048, 3072, 3200, 3584, 4096});
	}
	// v3: force10bit retired (the client cannot decode 10-bit); clean up a
	// value an older build may have left behind
	RemoveBoolIfOurs("driver_vrlink", "force10bit", true);
	if(g.vrlinkDebugOverlay){
		SetBoolIfDifferent("driver_vrlink", "debugRegionColoring", true);
		SetBoolIfDifferent("driver_vrlink", "showAdvancedGraphs", true);
		DriverLog("GalaxyXR: wrote driver_vrlink.debugRegionColoring/showAdvancedGraphs = true (effective at next connect)");
	}else{
		RemoveBoolIfOurs("driver_vrlink", "debugRegionColoring", true);
		RemoveBoolIfOurs("driver_vrlink", "showAdvancedGraphs", true);
	}
}

// 2026-09-03: vrlink reads steamvr.vrsettings during ITS OWN init, ~3 ms
// before our HMD shim's Activate runs (run 1: "Using defaults as unknown
// headset" 20:18:25.893, our profile write 25.896). so a connect always ran
// on the PREVIOUS session's stream/profile keys; run 1's first connect went
// 10-bit from a stale supports10bit=true. this is called from the device
// provider's Init, long before vrlink's HMD comes up, with vrlink's native
// model number as the section name. the Activate-time calls remain as the
// hot-reload path (they only write on difference).
// generic [driver_vrlink] key writer for the archaeology keys, see
// GalaxyXrConfig::vrlinkExtraKeys. logs every write; vrlink's log is the
// oracle for whether a key is read at all.
static void ApplyVrlinkExtraKeys(){
	const auto &g = driverConfig.galaxyXr;
	vr::EVRSettingsError rerr = vr::VRSettingsError_None;
	if(g.vrlinkMaxVideoQueueLatencyUs > 0){
		vr::VRSettings()->SetInt32("driver_vrlink", "maxVideoQueueLatencyUs", g.vrlinkMaxVideoQueueLatencyUs);
		DriverLog("GalaxyXR: vrlink maxVideoQueueLatencyUs = %d", g.vrlinkMaxVideoQueueLatencyUs);
	}else{
		vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "maxVideoQueueLatencyUs", &rerr);
	}
	if(g.vrlinkBackoffRecoveryCoefficient > 0.0){
		vr::VRSettings()->SetFloat("driver_vrlink", "backoffRecoveryCoefficient", (float)g.vrlinkBackoffRecoveryCoefficient);
		DriverLog("GalaxyXR: vrlink backoffRecoveryCoefficient = %g", g.vrlinkBackoffRecoveryCoefficient);
	}else{
		vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "backoffRecoveryCoefficient", &rerr);
	}
	for(const auto &e : driverConfig.galaxyXr.vrlinkExtraKeys){
		const std::string &name = std::get<0>(e); char kind = std::get<1>(e); double v = std::get<2>(e);
		if(name.empty()){ continue; }
		vr::EVRSettingsError err = vr::VRSettingsError_None;
		switch(kind){
			case 'i': vr::VRSettings()->SetInt32("driver_vrlink", name.c_str(), (int32_t)v); DriverLog("GalaxyXR: vrlink extra key %s = %d (int)", name.c_str(), (int)v); break;
			case 'f': vr::VRSettings()->SetFloat("driver_vrlink", name.c_str(), (float)v); DriverLog("GalaxyXR: vrlink extra key %s = %g (float)", name.c_str(), v); break;
			case 'b': vr::VRSettings()->SetBool("driver_vrlink", name.c_str(), v != 0.0); DriverLog("GalaxyXR: vrlink extra key %s = %s (bool)", name.c_str(), v != 0.0 ? "true" : "false"); break;
			case 'x': vr::VRSettings()->RemoveKeyInSection("driver_vrlink", name.c_str(), &err); DriverLog("GalaxyXR: vrlink extra key %s removed (%d)", name.c_str(), (int)err); break;
			default: break;
		}
	}
}

void GalaxyXR_EarlyApplyVrlinkSettings(){
	DriverLog("GalaxyXR: early vrlink settings apply (provider Init, before vrlink reads steamvr.vrsettings)");
	ApplyStreamQualitySetting();
	ApplyHeadsetProfileSetting("");
	ApplyVrlinkExtraKeys();
}

// write or remove the global vrlink render override per config. safe to call
// repeatedly; only writes on difference. changes are read by the compositor
// at SteamVR start, so mid-session toggles take effect next launch.
static void ApplyNativeResolutionSetting(){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	if(driverConfig.galaxyXr.nativeResolution){
		int32_t w = vr::VRSettings()->GetInt32("driver_vrlink", "overrideRenderWidth", &err);
		if(err != vr::VRSettingsError_None || w != kGalaxyXrRenderWidth){
			vr::VRSettings()->SetInt32("driver_vrlink", "renderWidth", kGalaxyXrRenderWidth);
			vr::VRSettings()->SetInt32("driver_vrlink", "renderHeight", kGalaxyXrRenderHeight);
			vr::VRSettings()->SetInt32("driver_vrlink", "overrideRenderWidth", kGalaxyXrRenderWidth);
			vr::VRSettings()->SetInt32("driver_vrlink", "overrideRenderHeight", kGalaxyXrRenderHeight);
			vr::VRSettings()->SetInt32("driver_vrlink", "displayFrequency", 90);
			vr::VRSettings()->SetInt32("steamvr", "preferredRefreshRate", 90);
			DriverLog("GalaxyXR: wrote driver_vrlink render %dx%d @90 (native resolution; effective next SteamVR start)",
				kGalaxyXrRenderWidth, kGalaxyXrRenderHeight);
		}
	}else{
		int32_t w = vr::VRSettings()->GetInt32("driver_vrlink", "overrideRenderWidth", &err);
		if(err == vr::VRSettingsError_None && w == kGalaxyXrRenderWidth){
			// only remove values we wrote; a different value means the user or
			// the community Apply-Settings tool owns it - leave it alone
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "renderWidth");
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "renderHeight");
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "overrideRenderWidth");
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "overrideRenderHeight");
			RemoveIntIfOurs("displayFrequency", {90});
			vr::EVRSettingsError rerr = vr::VRSettingsError_None;
			int32_t rr = vr::VRSettings()->GetInt32("steamvr", "preferredRefreshRate", &rerr);
			if(rerr == vr::VRSettingsError_None && rr == 90){
				vr::VRSettings()->RemoveKeyInSection("steamvr", "preferredRefreshRate");
			}
			DriverLog("GalaxyXR: removed driver_vrlink render override (nativeResolution off)");
		}
	}
}

// ---------------- HMD ----------------

void GalaxyXRHmdShim::PosTrackedDeviceActivate(uint32_t &unObjectId, vr::EVRInitError &returnValue){
	if(returnValue != vr::VRInitError_None){
		return;
	}
	container = vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);

	// only act on the vrlink-streamed Galaxy XR. on the wire the HMD's
	// tracking system is "oculus" (vrlink's Quest Pro profile asserts it;
	// SamsungVST only survives on the controllers), so gate on the serial
	// with the tracking system as a fallback
	// no identity gate (2026-08-26): the patched Steam Link APK says
	// SamsungVST, the stock one says GENERICHMD1 / vrlink with an empty
	// model, and neither is a promise about future builds. the user installed
	// a Galaxy XR driver and asked for the Galaxy XR identity; stamp it and
	// report what was there, so a bug report still shows the APK identity.
	std::string serial = vr::VRProperties()->GetStringProperty(container, vr::Prop_SerialNumber_String);
	std::string trackingSystem = vr::VRProperties()->GetStringProperty(container, vr::Prop_TrackingSystemName_String);
	origModelNumber = vr::VRProperties()->GetStringProperty(container, vr::Prop_ModelNumber_String);
	DriverLog("GalaxyXRHmdShim: stamping Galaxy XR identity over serial \"%s\" / tracking system \"%s\" / model \"%s\"",
		serial.c_str(), trackingSystem.c_str(), origModelNumber.c_str());
	origManufacturer = vr::VRProperties()->GetStringProperty(container, vr::Prop_ManufacturerName_String);
	origHmdInputProfile = vr::VRProperties()->GetStringProperty(container, vr::Prop_InputProfilePath_String);
	haveBackup = true;
	active = true;
	DriverLog("GalaxyXRHmdShim: activating identity override (was model=\"%s\" manufacturer=\"%s\")",
		origModelNumber.c_str(), origManufacturer.c_str());
	ApplyIdentity();
	ApplyNativeResolutionSetting();
	appliedNativeResolution = driverConfig.galaxyXr.nativeResolution;
	ApplyStreamQualitySetting();
	ApplyHeadsetProfileSetting(origModelNumber);
	appliedHeadsetProfile = driverConfig.galaxyXr.vrlinkHeadsetProfile;
	appliedProfile10bit = driverConfig.galaxyXr.profileSupports10bit;
	appliedBandwidthOverride = driverConfig.streamFrame.nvencBandwidthOverrideMbit;
	appliedDebugOverlay = driverConfig.galaxyXr.vrlinkDebugOverlay;
	appliedStreamQuality = driverConfig.galaxyXr.streamQuality;
	appliedCustomEncodeWidth = driverConfig.galaxyXr.customEncodeWidth;
	appliedCustomStreamFormatWidth = driverConfig.galaxyXr.customStreamFormatWidth;
	appliedCustomBandwidthMbit = driverConfig.galaxyXr.customBandwidthMbit;
	appliedProfileMaxSfw = GalaxyXR_EffectiveTileWidth();
	appliedExtraKeys = driverConfig.galaxyXr.vrlinkExtraKeys;
	appliedMaxVqLat = driverConfig.galaxyXr.vrlinkMaxVideoQueueLatencyUs; appliedBackoffCoef = driverConfig.galaxyXr.vrlinkBackoffRecoveryCoefficient;
}

void GalaxyXRHmdShim::ApplyIdentity(){
	if(!active){
		return;
	}
	bool wrote = false;
	wrote |= SetStringIfDifferent(container, vr::Prop_ModelNumber_String, "Galaxy XR");
	wrote |= SetStringIfDifferent(container, vr::Prop_ManufacturerName_String, "Samsung");
	if(SetDeviceIcons(container, "headset_galaxy_xr_status")){
		DriverLog("GalaxyXRHmdShim: status icons applied");
	}
	if(driverConfig.galaxyXr.nativeInputProfile){
		// repair the HMD's dangling {vrlink}/input/galaxy_xr_hmd_profile.json
		// reference with our shipped official copy
		wrote |= SetStringIfDifferent(container, vr::Prop_InputProfilePath_String,
			"{" + driverConfigLoader.info.driverName + "}/input/galaxy_xr_hmd_profile.json");
	}
	if(wrote){
		DriverLog("GalaxyXRHmdShim: identity applied");
	}
}

bool GalaxyXRHmdShim::PreTrackedDeviceDeactivate(){
	if(active && haveBackup){
		DriverLog("GalaxyXRHmdShim: restoring original identity on deactivate");
		vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, origModelNumber.c_str());
		vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, origManufacturer.c_str());
		vr::VRProperties()->SetStringProperty(container, vr::Prop_InputProfilePath_String, origHmdInputProfile.c_str());
	}
	active = false;
	return true;
}

void GalaxyXRHmdShim::RunFrame(){
	if(active){ ProbeVrlinkPaths(); }
	// config hot-reload: apply/remove the resolution override on toggle flips
	if(active && driverConfig.galaxyXr.nativeResolution != appliedNativeResolution){
		appliedNativeResolution = driverConfig.galaxyXr.nativeResolution;
		ApplyNativeResolutionSetting();
	}
	{
		const auto &g = driverConfig.galaxyXr;
		// v3: the profile's max width tracks the tile, so a stream change
		// re-applies the profile too
		int tileNow = GalaxyXR_EffectiveTileWidth();
		if(active && (g.vrlinkHeadsetProfile != appliedHeadsetProfile || tileNow != appliedProfileMaxSfw
				|| g.profileSupports10bit != appliedProfile10bit
				|| g.vrlinkDebugOverlay != appliedDebugOverlay)){
			appliedDebugOverlay = g.vrlinkDebugOverlay;
			appliedHeadsetProfile = g.vrlinkHeadsetProfile;
			appliedProfileMaxSfw = tileNow;
			appliedProfile10bit = g.profileSupports10bit;
			ApplyHeadsetProfileSetting(origModelNumber);
		}
		if(active && (g.vrlinkExtraKeys != appliedExtraKeys || g.vrlinkMaxVideoQueueLatencyUs != appliedMaxVqLat
				|| g.vrlinkBackoffRecoveryCoefficient != appliedBackoffCoef)){
			appliedExtraKeys = g.vrlinkExtraKeys; appliedMaxVqLat = g.vrlinkMaxVideoQueueLatencyUs; appliedBackoffCoef = g.vrlinkBackoffRecoveryCoefficient;
			ApplyVrlinkExtraKeys();
		}
		bool customChanged = g.streamQuality == "custom" && (g.customEncodeWidth != appliedCustomEncodeWidth
			|| g.customStreamFormatWidth != appliedCustomStreamFormatWidth || g.customBandwidthMbit != appliedCustomBandwidthMbit);
		int bwOverride = driverConfig.streamFrame.nvencBandwidthOverrideMbit;
		if(active && (g.streamQuality != appliedStreamQuality || customChanged || bwOverride != appliedBandwidthOverride)){
			appliedStreamQuality = g.streamQuality;
			appliedCustomEncodeWidth = g.customEncodeWidth;
			appliedCustomStreamFormatWidth = g.customStreamFormatWidth;
			appliedCustomBandwidthMbit = g.customBandwidthMbit;
			appliedBandwidthOverride = bwOverride;
			ApplyStreamQualitySetting();
		}
	}
}

void GalaxyXRHmdShim::HandleEvent(const vr::VREvent_t &event){
	// vrlink re-asserts properties after activation; reapply when our
	// container changes. ApplyIdentity only writes on difference, so the
	// PropertyChanged events caused by our own writes converge immediately.
	if(active && event.eventType == vr::VREvent_PropertyChanged && event.data.property.container == container){
		ApplyIdentity();
	}
}

// ---------------- Controllers ----------------

GalaxyXRControllerShim::GalaxyXRControllerShim(const std::string &serial) : serial(serial){
	isLeft = serial.find("Left") != std::string::npos;
}

void GalaxyXRControllerShim::PosTrackedDeviceActivate(uint32_t &unObjectId, vr::EVRInitError &returnValue){
	if(returnValue != vr::VRInitError_None){
		return;
	}
	container = vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);

	// no identity gate (see GalaxyXRHmdShim): stamp on request, log what
	// vrlink had put there. "oculus" / Quest2 model on the stock APK,
	// SamsungVST on the patched one.
	std::string trackingSystem = vr::VRProperties()->GetStringProperty(container, vr::Prop_TrackingSystemName_String);
	std::string origModel = vr::VRProperties()->GetStringProperty(container, vr::Prop_ModelNumber_String);
	DriverLog("GalaxyXRControllerShim: %s tracking system \"%s\" / model \"%s\"",
		serial.c_str(), trackingSystem.c_str(), origModel.c_str());
	origRenderModel = vr::VRProperties()->GetStringProperty(container, vr::Prop_RenderModelName_String);
	origInputProfile = vr::VRProperties()->GetStringProperty(container, vr::Prop_InputProfilePath_String);
	origControllerType = vr::VRProperties()->GetStringProperty(container, vr::Prop_ControllerType_String);
	haveBackup = true;
	active = true;
	DriverLog("GalaxyXRControllerShim: activating for %s (was rendermodel=\"%s\")", serial.c_str(), origRenderModel.c_str());
	ApplyIdentity();
}

// generate a uniformly scaled copy of a render model folder: OBJ vertex
// positions and the json's length-valued fields (component origins, motion
// pivot/center, press_translate) are multiplied; direction vectors (axis)
// and angles (rotate_xyz, value_mapping, joystick ranges) are copied as-is;
// all other files (mtl, textures) are copied verbatim.
// GalaxyXrConfig::simulateTouch: add or remove the oculus_touch layout in
// the shipped remapping json (in the driver's resource dir, so it survives
// rebuilds as long as the flag is set). SteamVR reads the file at startup.
static void SyncTouchLayout(){
	// 2026-09-05: the official profile's remapping shipped WITHOUT an
	// oculus_touch layout. OpenXR apps that suggest the Touch interaction
	// profile (Pools VR: "Created default binding ... Controller type
	// 'oculus_touch'") and OpenVR apps with Touch bindings then had no
	// route to galaxy_xr_controller: no controllers at all in Pools, and
	// thumbstick click / trigger touch lost through the knuckles
	// autoremapping fallback. galaxy_xr_controller declares exactly the
	// Touch component set, so the layout is the identity form
	// ("remappings": [], simulate_controller_type) the working Samsung
	// profile uses. it is now shipped in the json AND repaired here (older
	// resource dirs, or the autoremapping-style layout an earlier build
	// wrote). simulateTouch only controls the render-model / HMD
	// simulation flags on that layout.
	static bool synced = false;
	if(synced){ return; }
	synced = true;
	namespace fs = std::filesystem;
	try{
		std::string path = driverConfigLoader.info.driverResources + "/input/galaxy_xr_controller_remapping.json";
		if(!fs::exists(path)){ return; }
		nlohmann::json j;
		{
			std::ifstream in(path);
			in >> j;
		}
		if(!j.contains("layouts") || !j["layouts"].is_array()){ return; }
		nlohmann::json &layouts = j["layouts"];
		bool sim = driverConfig.galaxyXr.simulateTouch;
		nlohmann::json touch = {
			{"priority", 100}, // knuckles (110) leads, then Touch, then generic/hands/wands
			{"from_controller_type", "oculus_touch"},
			{"simulate_controller_type", true},
			{"simulate_render_model", sim},
			{"simulate_HMD", sim},
			{"remappings", nlohmann::json::array()},
		};
		int touchIndex = -1, knucklesIndex = -1;
		for(int i = 0; i < (int)layouts.size(); i++){
			std::string from = layouts[i].value("from_controller_type", "");
			if(from == "oculus_touch"){ touchIndex = i; }
			if(from == "knuckles"){ knucklesIndex = i; }
		}
		bool changed = false;
		if(touchIndex < 0){
			// right after the knuckles layout (or first if there is none)
			layouts.insert(layouts.begin() + (knucklesIndex >= 0 ? knucklesIndex + 1 : 0), touch);
			changed = true;
		}else if(layouts[touchIndex] != touch){
			layouts[touchIndex] = touch;
			changed = true;
		}
		if(changed){
			std::ofstream out(path, std::ios::trunc);
			out << j.dump(2) << "\n";
			DriverLog("GalaxyXRControllerShim: remapping oculus_touch identity layout %s (simulateTouch=%d; effective next SteamVR start)",
				touchIndex < 0 ? "added" : "repaired", sim ? 1 : 0);
		}
	}catch(const std::exception &e){
		DriverLog("GalaxyXRControllerShim: remapping sync failed: %s", e.what());
	}
}

// hand_anchor values (see GalaxyXrConfig::handAnchor*) for one hand, in
// render model units (metres / degrees), mirrored for the right hand
struct HandAnchorLocal{
	double origin[3];
	double rotateXyz[3];
};
static HandAnchorLocal HandAnchorFor(bool isLeft){
	const GalaxyXrConfig &g = driverConfig.galaxyXr;
	double m = isLeft ? 1.0 : -1.0;
	HandAnchorLocal a;
	a.origin[0] = m * g.handAnchorXCm * 0.01;
	a.origin[1] = g.handAnchorYCm * 0.01;
	a.origin[2] = g.handAnchorZCm * 0.01;
	a.rotateXyz[0] = g.handAnchorPitchDeg;
	a.rotateXyz[1] = m * g.handAnchorYawDeg;
	a.rotateXyz[2] = m * g.handAnchorRollDeg;
	return a;
}
static bool HandAnchorIsIdentity(){
	const GalaxyXrConfig &g = driverConfig.galaxyXr;
	if(g.officialComponents){ return false; } // official set always needs a generated variant
	return g.handAnchorXCm == 0.0 && g.handAnchorYCm == 0.0 && g.handAnchorZCm == 0.0
		&& g.handAnchorPitchDeg == 0.0 && g.handAnchorYawDeg == 0.0 && g.handAnchorRollDeg == 0.0
		&& g.meshOffsetXCm == 0.0 && g.meshOffsetYCm == 0.0 && g.meshOffsetZCm == 0.0;
}
// mesh counter-translation for one hand, metres, X mirrored for the right
static void MeshOffsetFor(bool isLeft, double out[3]){
	const GalaxyXrConfig &g = driverConfig.galaxyXr;
	out[0] = (isLeft ? 1.0 : -1.0) * g.meshOffsetXCm * 0.01;
	out[1] = g.meshOffsetYCm * 0.01;
	out[2] = g.meshOffsetZCm * 0.01;
}
// Samsung's official pose components (left hand, metres / degrees), copied
// from Game Link's vst_controller_left.json. all pure-X rotations except
// base (yaw 180). right hand: X negated, yaw negated.
struct OfficialComponent{
	const char* name;
	double origin[3];
	double pitchDeg;
	double yawDeg;
};
static const OfficialComponent kOfficialComponents[] = {
	{"base",             {-0.0034,   -0.0034,     0.1491},    -0.4, 180.0},
	{"tip",              { 0.011811, -0.030531,   0.020703},  -37.4,  0.0},
	{"openxr_aim",       { 0.007,    -0.03894766, 0.00949694}, -39.4, 0.0},
	{"openxr_grip",      { 0.007,    -0.000242,   0.098076},   20.6,  0.0},
	{"openxr_handmodel", {-0.01125,  -0.00182941, 0.1019482}, -39.4,  0.0},
	{"handgrip",         { 0.002469,  0.003,      0.097},       5.037, 0.0},
	{"grip",             { 0.002469,  0.003,      0.097},       5.037, 0.0},
};
// must match DeviceProvider's gripConvention constants
static const double kGripConventionPitchDeg = 22.0;
static const double kGripConventionZ = 0.05;
// rebase an official component (authored against vrlink raw) into our raw
// frame: new = T^-1 * official, T = rotate X by 22deg then translate 5cm
// along local Z. for the pure-X components the pitch simply drops by 22;
// for base (yaw 180) the X rotation conjugates through the Y flip:
// R_x(-22) R_y(180) R_x(-0.4) = R_x(-21.6) R_y(180), written assuming
// rotate_xyz applies X before Y. if base shows up mirrored in pitch the
// euler order is the other way and the sign flips (base is cosmetic).
static void OfficialComponentFor(const OfficialComponent &c, bool isLeft, bool rebase, double outOrigin[3], double outRot[3]){
	double m = isLeft ? 1.0 : -1.0;
	double o[3] = {m * c.origin[0], c.origin[1], c.origin[2]};
	double pitch = c.pitchDeg;
	double yaw = m * c.yawDeg;
	if(rebase){
		double a = -kGripConventionPitchDeg * 3.14159265358979323846 / 180.0;
		double ca = cos(a), sa = sin(a);
		double d[3] = {o[0], o[1], o[2] - kGripConventionZ};
		o[0] = d[0];
		o[1] = ca * d[1] - sa * d[2];
		o[2] = sa * d[1] + ca * d[2];
		if(fabs(c.yawDeg) > 90.0){
			pitch = -kGripConventionPitchDeg - c.pitchDeg;
		}else{
			pitch = c.pitchDeg - kGripConventionPitchDeg;
		}
	}
	// fold the pose trims (applied to raw AFTER the convention): the
	// physical points must stay put when the hand pose is trimmed
	if(rebase && driverConfig.galaxyXr.componentRebaseIncludeTrim){
		const ControllersConfig &cc = driverConfig.controllers;
		double tPos[3] = {cc.positionOffsetCm[0], cc.positionOffsetCm[1], cc.positionOffsetCm[2]};
		double tYaw = cc.rotationOffsetDeg[1];
		if(!isLeft && cc.mirrorOffsetsForRightHand){ tPos[0] = -tPos[0]; tYaw = -tYaw; }
		const double* hp = isLeft ? cc.leftPositionOffsetCm : cc.rightPositionOffsetCm;
		const double* hr = isLeft ? cc.leftRotationOffsetDeg : cc.rightRotationOffsetDeg;
		for(int i = 0; i < 3; i++){ tPos[i] = (tPos[i] + hp[i]) * 0.01; }
		tYaw += hr[1];
		// trim = translate t (local) then rotate R_y(yaw): comp' = R_y(-yaw) (comp - t)
		double b = -tYaw * 3.14159265358979323846 / 180.0;
		double cb = cos(b), sb = sin(b);
		double d[3] = {o[0] - tPos[0], o[1] - tPos[1], o[2] - tPos[2]};
		o[0] = cb * d[0] + sb * d[2];
		o[1] = d[1];
		o[2] = -sb * d[0] + cb * d[2];
		yaw += -tYaw;
	}
	// aim-family measured correction
	if(std::string(c.name) == "tip" || std::string(c.name) == "openxr_aim"){
		o[0] += m * driverConfig.galaxyXr.aimTrimXCm * 0.01;
		o[1] += driverConfig.galaxyXr.aimTrimYCm * 0.01;
		o[2] += driverConfig.galaxyXr.aimTrimZCm * 0.01;
	}
	outOrigin[0] = o[0]; outOrigin[1] = o[1]; outOrigin[2] = o[2];
	outRot[0] = pitch; outRot[1] = yaw; outRot[2] = 0.0;
}

// short stable tag of the anchor values for the variant folder name, so a
// value change produces a NEW model name and SteamVR reloads it live
static std::string HandAnchorTag(){
	if(HandAnchorIsIdentity()){ return ""; }
	const GalaxyXrConfig &g = driverConfig.galaxyXr;
	char buf[512];
	const ControllersConfig &cc = driverConfig.controllers;
	snprintf(buf, sizeof(buf), "%.1f_%.1f_%.1f_%.1f_%.1f_%.1f_m%.1f_%.1f_%.1f_o%d_gc%d_a%.1f_%.1f_%.1f_t%d_%.1f_%.1f_%.1f_%.1f_%.1f_%.1f_%.1f_%.1f_%.1f_%.1f_%.1f_%.1f_%.1f",
		g.handAnchorXCm, g.handAnchorYCm, g.handAnchorZCm,
		g.handAnchorPitchDeg, g.handAnchorYawDeg, g.handAnchorRollDeg,
		g.meshOffsetXCm, g.meshOffsetYCm, g.meshOffsetZCm,
		g.officialComponents ? 1 : 0, g.gripConvention ? 1 : 0,
		g.aimTrimXCm, g.aimTrimYCm, g.aimTrimZCm,
		g.componentRebaseIncludeTrim ? 1 : 0,
		cc.positionOffsetCm[0], cc.positionOffsetCm[1], cc.positionOffsetCm[2], cc.rotationOffsetDeg[1],
		cc.leftPositionOffsetCm[0], cc.leftPositionOffsetCm[1], cc.leftPositionOffsetCm[2], cc.leftRotationOffsetDeg[1],
		cc.rightPositionOffsetCm[0], cc.rightPositionOffsetCm[1], cc.rightPositionOffsetCm[2], cc.rightRotationOffsetDeg[1],
		cc.mirrorOffsetsForRightHand ? 1.0 : 0.0);
	std::string h = buf;
	uint32_t x = 2166136261u;
	for(char c : h){ x = (x ^ (uint8_t)c) * 16777619u; }
	snprintf(buf, sizeof(buf), "_a%08x", x);
	return buf;
}

static bool GenerateScaledRenderModel(const std::string &srcDir, const std::string &dstDir, double scale, const std::string &srcJsonName, const std::string &dstJsonName, const HandAnchorLocal *anchor, const double *meshOffset, bool isLeft){
	namespace fs = std::filesystem;
	try{
		// regenerate when the source json is newer than the variant: the
		// pose-anchor components (handgrip/openxr_grip/grip) are edited in
		// the base asset, and a cached variant must not keep serving stale
		// anchors after a rebuild
		if(fs::exists(dstDir)){
			fs::path dstJson = fs::path(dstDir) / dstJsonName;
			fs::path srcJson = fs::path(srcDir) / srcJsonName;
			if(fs::exists(dstJson) && fs::exists(srcJson)
					&& fs::last_write_time(srcJson) <= fs::last_write_time(dstJson)){
				return true;
			}
			DriverLog("GalaxyXRControllerShim: scaled render model %s is older than its source - regenerating", dstDir.c_str());
			fs::remove_all(dstDir);
		}
		std::string tmpDir = dstDir + ".tmp";
		fs::remove_all(tmpDir);
		fs::create_directories(tmpDir);
		for(const auto &entry : fs::directory_iterator(srcDir)){
			if(!entry.is_regular_file()){ continue; }
			std::string name = entry.path().filename().string();
			std::string ext = entry.path().extension().string();
			if(ext == ".obj"){
				std::ifstream in(entry.path());
				std::ofstream out(fs::path(tmpDir) / name);
				std::string line;
				while(std::getline(in, line)){
					double x, y, z;
					if(line.rfind("v ", 0) == 0 && sscanf(line.c_str(), "v %lf %lf %lf", &x, &y, &z) == 3){
						char buf[128];
						snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f", x * scale, y * scale, z * scale);
						out << buf << "\n";
					}else{
						out << line << "\n";
					}
				}
			}else if(name == srcJsonName){
				std::ifstream in(entry.path());
				nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
				// pose-anchor components (handgrip, openxr_grip, tip, aim, base:
				// entries with no mesh "filename") describe physical points
				// measured against the tracking origin in real metres; they are
				// NOT scaled with the mesh. only mesh-bearing components carry
				// their origins/pivots along with the geometry.
				std::function<void(nlohmann::json&, bool)> walk = [&](nlohmann::json &node, bool poseAnchor){
					if(node.is_object()){
						for(auto &item : node.items()){
							const std::string &key = item.key();
							nlohmann::json &val = item.value();
							if(key == "components" && val.is_object()){
								for(auto &comp : val.items()){
									bool anchor = comp.value().is_object() && !comp.value().contains("filename");
									walk(comp.value(), anchor);
								}
								continue;
							}
							if(poseAnchor && key == "origin"){
								continue;
							}
							if(val.is_array() && (key == "origin" || key == "pivot" || key == "center" || key == "press_translate")){
								for(auto &n : val){
									if(n.is_number()){ n = n.get<double>() * scale; }
								}
							}else{
								walk(val, poseAnchor);
							}
						}
					}else if(node.is_array()){
						for(auto &child : node){ walk(child, poseAnchor); }
					}
				};
				walk(j, false);
				// mesh counter-translation: shift every mesh-bearing component's
				// origin (after scaling, real metres) so the visible shell moves
				// without touching any pose anchor
				if(meshOffset && (meshOffset[0] != 0 || meshOffset[1] != 0 || meshOffset[2] != 0)
						&& j.contains("components") && j["components"].is_object()){
					for(auto &comp : j["components"].items()){
						nlohmann::json &c = comp.value();
						if(!c.is_object() || !c.contains("filename")){ continue; }
						nlohmann::json &local = c["component_local"];
						if(!local.is_object()){ local = nlohmann::json::object(); }
						nlohmann::json &origin = local["origin"];
						if(!origin.is_array() || origin.size() != 3){ origin = {0.0, 0.0, 0.0}; }
						for(int i = 0; i < 3; i++){
							origin[i] = origin[i].get<double>() + meshOffset[i];
						}
					}
				}
				// official Samsung pose components, rebased into our raw frame
				// (see GalaxyXrConfig::officialComponents). never scaled.
				if(driverConfig.galaxyXr.officialComponents && j.contains("components") && j["components"].is_object()){
					for(const OfficialComponent &c : kOfficialComponents){
						double o[3], r[3];
						OfficialComponentFor(c, isLeft, driverConfig.galaxyXr.gripConvention, o, r);
						nlohmann::json &comp = j["components"][c.name];
						comp["component_local"]["origin"] = {o[0], o[1], o[2]};
						comp["component_local"]["rotate_xyz"] = {r[0], r[1], r[2]};
					}
					DriverLog("GalaxyXRControllerShim: official pose components written (%s, rebase=%d)",
						isLeft ? "left" : "right", driverConfig.galaxyXr.gripConvention ? 1 : 0);
				}
				// hand_anchor: written from config (never scaled - real metres)
				if(anchor && j.contains("components") && j["components"].is_object()){
					nlohmann::json &comp = j["components"]["hand_anchor"];
					comp["component_local"]["origin"] = {anchor->origin[0], anchor->origin[1], anchor->origin[2]};
					comp["component_local"]["rotate_xyz"] = {anchor->rotateXyz[0], anchor->rotateXyz[1], anchor->rotateXyz[2]};
				}
				std::ofstream out(fs::path(tmpDir) / dstJsonName);
				out << j.dump(1);
			}else{
				fs::copy_file(entry.path(), fs::path(tmpDir) / name);
			}
		}
		fs::rename(tmpDir, dstDir);
		return true;
	}catch(const std::exception &e){
		DriverLog("GalaxyXR: failed to generate scaled render model %s: %s", dstDir.c_str(), e.what());
		return false;
	}
}

// purge scaled variants other than the one currently wanted (~7MB each
// while the user dials the knob)
static void PurgeStaleScaledModels(const std::string &rendermodelsDir, const std::string &keepPrefix){
	namespace fs = std::filesystem;
	try{
		for(const auto &entry : fs::directory_iterator(rendermodelsDir)){
			std::string name = entry.path().filename().string();
			if(name.rfind("vst_controller_s", 0) == 0 && name.rfind(keepPrefix, 0) != 0){
				fs::remove_all(entry.path());
			}
		}
	}catch(const std::exception &){}
}

std::string GalaxyXRControllerShim::TargetModelName(){
	// replace the dangling {vrlink}/rendermodels/vst_controller_* reference
	// (never resolves: vrlink ships no such model) with our converted asset.
	// a non-empty renderModelVariant redirects to a tuning variant folder;
	// SteamVR reloads the model whenever the name changes, which is what
	// makes live alignment iteration possible.
	// official animated Steam Link models (see resources/PERMISSIONS.md)
	std::string base = "vst_controller";
	std::string variant = driverConfig.galaxyXr.renderModelVariant;
	if(!variant.empty()){
		// a stale variant key (e.g. a tuning session that ended without
		// 'done', followed by a rebuild that purged the tune folders) must
		// not leave the controllers without a model: only honor the variant
		// when its folder actually exists, otherwise fall back and log
		std::string variantDir = driverConfigLoader.info.driverResources + "/rendermodels/" + variant + "_" + (isLeft ? "left" : "right");
		if(std::filesystem::exists(variantDir)){
			base = variant;
		}else{
			DriverLog("GalaxyXRControllerShim: renderModelVariant \"%s\" has no folder at %s - using default model",
				variant.c_str(), variantDir.c_str());
		}
	}
	// uniform whole-model-system scale: generate (once per value) a variant
	// with geometry, component origins and motion pivots scaled together,
	// and swap to it via the name-change reload. tuning variants win.
	int scalePct = (int)std::lround(driverConfig.galaxyXr.renderModelScale * 100.0);
	if(scalePct < 50 || scalePct > 200){ scalePct = 100; }
	std::string anchorTag = HandAnchorTag();
	// a generated variant is needed for a non-unit scale OR a non-identity
	// hand_anchor; the folder name carries both so either change reloads
	if(variant.empty() && (scalePct != 100 || !anchorTag.empty())){
		std::string hand = isLeft ? "left" : "right";
		std::string scaledBase = "vst_controller_s" + std::to_string(scalePct) + anchorTag;
		std::string rmDir = driverConfigLoader.info.driverResources + "/rendermodels";
		static std::mutex genMutex;
		std::lock_guard<std::mutex> lock(genMutex);
		PurgeStaleScaledModels(rmDir, scaledBase);
		HandAnchorLocal anchor = HandAnchorFor(isLeft);
		double meshOffset[3];
		MeshOffsetFor(isLeft, meshOffset);
		bool ok = GenerateScaledRenderModel(
			rmDir + "/vst_controller_" + hand,
			rmDir + "/" + scaledBase + "_" + hand,
			scalePct / 100.0,
			"vst_controller_" + hand + ".json",
			scaledBase + "_" + hand + ".json",
			&anchor, meshOffset, isLeft);
		if(ok){
			base = scaledBase;
		}
	}
	return "{" + driverConfigLoader.info.driverName + "}/rendermodels/" + base + "_" + (isLeft ? "left" : "right");
}

void GalaxyXRControllerShim::ApplyIdentity(){
	if(!active){
		return;
	}
	// the shim now also exists for input-profile-only setups, so the
	// model/icon half is gated on nativeIdentity here rather than at creation
	if(driverConfig.galaxyXr.nativeIdentity){
		std::string model = TargetModelName();
		bool wrote = SetStringIfDifferent(container, vr::Prop_RenderModelName_String, model);
		if(SetDeviceIcons(container, isLeft ? "left_galaxy_xr_status" : "right_galaxy_xr_status")){
			DriverLog("GalaxyXRControllerShim: status icons applied for %s", serial.c_str());
		}
		if(wrote){
			appliedModel = model;
			DriverLog("GalaxyXRControllerShim: rendermodel %s applied for %s", model.c_str(), serial.c_str());
		}
	}
	if(driverConfig.galaxyXr.nativeInputProfile){
		SyncTouchLayout();
		// the official native input profile: controller type
		// galaxy_xr_controller with Valve's own legacy bindings, remapping
		// and pose components. the grip-family components (handgrip,
		// openxr_grip, grip) are IDENTITY in our render model json: the raw
		// pose already carries the grip convention (gripConvention), so a
		// binding that selects /pose/handgrip (UE4 per-app bindings) or the
		// OpenXR grip pose lands on exactly the point SteamVR Home and
		// /pose/raw bindings use. note: the official remapping has no
		// oculus_touch layout, so user-made custom Touch bindings do not
		// auto-carry; per-game rebinding may be needed.
		std::string profile = "{" + driverConfigLoader.info.driverName + "}/input/galaxy_xr_controller_profile.json";
		bool wroteProfile = SetStringIfDifferent(container, vr::Prop_InputProfilePath_String, profile);
		bool wroteType = SetStringIfDifferent(container, vr::Prop_ControllerType_String, "galaxy_xr_controller");
		if(wroteProfile || wroteType){
			DriverLog("GalaxyXRControllerShim: native input profile applied for %s", serial.c_str());
		}
	}
}

void GalaxyXRControllerShim::RunFrame(){
	if(!active){
		return;
	}
	if(!driverConfig.galaxyXr.nativeIdentity){
		return;
	}
	// config hot-reload: swap the model live when the variant changes
	if(TargetModelName() != appliedModel){
		ApplyIdentity();
		return;
	}
	// icon drift poll, ~1 Hz: re-assert if vrlink rewrote the status icons
	// behind us (see SetDeviceIcons). one property read per second.
	if(++iconPollFrames >= 90){
		iconPollFrames = 0;
		std::string ready = vr::VRProperties()->GetStringProperty(container, vr::Prop_NamedIconPathDeviceReady_String);
		if(ready != ExpectedReadyIcon(isLeft ? "left_galaxy_xr_status" : "right_galaxy_xr_status")){
			DriverLog("GalaxyXRControllerShim: status icons drifted for %s (now \"%s\"), re-asserting", serial.c_str(), ready.c_str());
			ApplyIdentity();
		}
	}
}

bool GalaxyXRControllerShim::PreTrackedDeviceDeactivate(){
	if(active && haveBackup){
		vr::VRProperties()->SetStringProperty(container, vr::Prop_RenderModelName_String, origRenderModel.c_str());
		vr::VRProperties()->SetStringProperty(container, vr::Prop_InputProfilePath_String, origInputProfile.c_str());
		vr::VRProperties()->SetStringProperty(container, vr::Prop_ControllerType_String, origControllerType.c_str());
	}
	active = false;
	return true;
}

void GalaxyXRControllerShim::HandleEvent(const vr::VREvent_t &event){
	if(active && event.eventType == vr::VREvent_PropertyChanged && event.data.property.container == container){
		ApplyIdentity();
	}
}
