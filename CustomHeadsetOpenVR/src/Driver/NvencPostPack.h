#pragma once
// ============================================================================
// Post-pack processing on the packed transport frame, 2026-09-05.
//
// vrlink's foveation shader renders the frame NVENC encodes: four stacked
// square tiles of streamFormatWidth (per eye: a 1:1 gaze-tracked cut-out of
// the render target and the whole view downscaled). The NvencTap sees that
// texture at nvEncEncodePicture, right before the encoder reads it. Doing
// per-pixel work HERE instead of on the app's eye textures is ~10x fewer
// pixels (9.4 MP vs 109 MP at 200% SS), removes a full-resolution copy from
// the compositor path, sharpens the periphery AFTER its downscale (where it
// survives), and lets the fovea and periphery tiles have their own strength.
//
// Passes (luma plane; chroma only for the range remap):
//   * CAS (contrast adaptive sharpening), per tile strength
//   * limited-range remap: vrlink produces full-range BT.709; the client
//     handles full-range video imperfectly (black floor). Y -> 16..235,
//     C -> 16..240 (as fractions, so P010 works the same) and the VUI
//     full-range flag is cleared by the tap, putting the stream on the
//     decoder's well-trodden path.
//
// Mechanics: copy the packed NV12/P010 texture to a scratch, then one
// full-screen pixel-shader pass per plane from the scratch's plane SRV into
// the original's plane RTV (RTVs on video planes are guaranteed: vrlink
// itself renders into them). EncodePicture is called on vrlink's encode
// thread, so its immediate context is used under D3D11 multithread
// protection. Any failure disables the feature for the session with a log
// line; the encoder then sees vrlink's untouched frame.
// ============================================================================
#include <cstdint>

struct NvencPostPackConfig {
	bool enable = false;
	bool casEnable = true;
	float foveaStrength = 0.6f;      // 0..1
	float peripheryStrength = 0.3f;  // 0..1
	bool foveaTop = true;            // fovea tile is the upper of each eye's tile pair
	float edgeFalloff = 0.12f;       // fraction of the fovea tile over which its strength ramps down to the periphery strength at the tile border (seam softening)
	bool limitedRange = false;       // Y/C to limited range (with the VUI flag cleared by the tap)
};

struct NvencPostPackStats {
	uint32_t frames = 0, skipped = 0;
	double lastMs = 0, sumMs = 0, maxMs = 0;
	bool disabled = false;
};

// registered/mapped resource bookkeeping is done by the tap; it hands the
// D3D11 texture (ID3D11Texture2D*) here.
namespace NvencPostPack {
	void SetConfig(const NvencPostPackConfig &cfg);
	// process the packed frame before the encoder reads it. returns false if
	// nothing was done (disabled, unsupported, error).
	bool Process(void* d3d11Texture2D, uint32_t nvencBufferFormat);
	NvencPostPackStats GetStats();
	void ResetIntervalStats();
}
