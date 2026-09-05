#pragma once
#include <string>

// ============================================================================
// Galaxy XR stream quality tiers, v3 (2026-09-05).
//
// A tier is now only {tile width, bandwidth}. Everything about the encoder
// is a global default in StreamFrameConfig (P7 auto / CBR / VBV 2 / split /
// fps pin / fix level ...), the same for every tier, because none of it
// depends on bandwidth. Measured facts behind the numbers (runs A..X, S, V,
// 350/450/500/600):
//   * the transport frame is four stacked square tiles of streamFormatWidth
//     (per eye: the whole view downscaled + a 1:1 gaze-tracked cut-out of the
//     render target). 4 x 2048 = 8192 is the HEVC/NVENC height limit, so
//     2048 is the hard tile ceiling; 1536 tiles are 44% fewer pixels.
//   * encodeWidth has no observable effect in foveated mode (the sampling
//     shader never sees it); it is written as a constant for compatibility.
//   * vrlink's per-frame bitrate request is its real rate control and it
//     reacts to frame lateness (encode + transmit); at 2048 tiles every
//     preset is late and the allocation starves. 1536 tiles hold the full
//     allocation at P7 on a 3-engine GPU.
//   * vrlink's own request never exceeds 350; the tap scales it to the
//     tier bandwidth. 450 is the client's limit (its reset requests climb
//     steeply at 500 and 600 while the radio stays fine).
//   * CBR + VBV 2 caps every frame (vegetation peaks and reset IDRs) far
//     below vrlink's 2 MB send limit; spatial AQ serializes NVENC submission
//     and is never used.
// ============================================================================

struct GxrStreamTier {
	const char* name;
	int width;          // streamFormatWidth: the tile (1536 or 2048)
	int bandwidthMbit;  // targetBandwidth + recommendedBandwidthMbit + tap bitrate
};

// legacy names map onto the closest v3 tier so old settings.json files keep
// working: v1 stable/quality/high/highest/ultra, v2 default (stock vrlink,
// now equivalent to balanced with the tap on)
static const GxrStreamTier kGxrStreamTiers[] = {
	{"efficient", 1536, 300},
	{"balanced",  1536, 350},
	{"vivid",     1536, 400},
	{"sharp",     1536, 450},
	// wider fovea box and a sharper periphery for 78% more encode work; on
	// current hardware the allocator starves it (encode-limited). kept for
	// GPUs that get there.
	{"max",       2048, 450},
};

inline std::string CanonicalGxrStreamTierName(const std::string &name){
	if(name == "stable"){ return "efficient"; }
	if(name == "quality" || name == "default"){ return "balanced"; }
	if(name == "high" || name == "highest"){ return "sharp"; }
	if(name == "ultra"){ return "max"; }
	return name;
}

inline const GxrStreamTier* FindGxrStreamTier(const std::string &name){
	std::string n = CanonicalGxrStreamTierName(name);
	for(const auto &t : kGxrStreamTiers){
		if(n == t.name){ return &t; }
	}
	return nullptr;
}
