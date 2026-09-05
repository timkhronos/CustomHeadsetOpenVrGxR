#include "FrameProcessor.h"
#include "DriverLog.h"
#include "ReconLogger.h"
#include "ZeroCopy.h"
#include "NvencTap.h"
#include "NvencPostPack.h"
#include "../Config/StreamTiers.h"
#include "../Headsets/GalaxyXR.h"

// shared head-direction -> per-eye viewport uv mapping. this is the exact
// math the gaze debug ring uses (verified against the runtime's foveation
// center); keeping it in one place guarantees the ring, the pupil swim
// center, the fixation dot, and the swim probe log all agree.
bool MapHeadDirToEyeUv(const FrameProcessSettings &settings, int eye,
	double dirX, double dirY, double dirZ, double &u, double &v){
	if(dirZ >= -0.1){
		return false;
	}
	double tanX = dirX / -dirZ;
	double tanY = dirY / -dirZ;
	if(settings.gazeProjValid){
		// the real asymmetric frustum from the display component. raw
		// projection tangents are y-down (top negative for the up edge),
		// head-space dirY is +up, hence the sign flip.
		const float* proj = settings.gazeProj[eye];
		double l = proj[0], r = proj[1], t = proj[2], b = proj[3];
		u = r != l ? (tanX - l) / (r - l) : 0.5;
		v = b != t ? (-tanY - t) / (b - t) : 0.5;
	}else{
		// fallback: symmetric knobs around the distortion center
		double centerX = eye == 0 ? settings.config.centerOffsetXLeft : settings.config.centerOffsetXRight;
		u = 0.5 + centerX + 0.5 * tanX / settings.config.eyeGaze.tanHalfFovX;
		v = 0.5 + settings.config.centerOffsetY - 0.5 * tanY / settings.config.eyeGaze.tanHalfFovY;
	}
	return u > -0.2 && u < 1.2 && v > -0.2 && v < 1.2;
}

// non uniform catmull rom style hermite interpolation through sorted knots,
// flat outside the knot range
double EvaluateDistortionCurve(const std::vector<StreamFrameDistortionPoint> &points, double r){
	if(points.empty()){
		return 1.0;
	}
	if(points.size() == 1 || r <= points.front().r){
		return r <= points.front().r ? points.front().scale : points.back().scale;
	}
	if(r >= points.back().r){
		return points.back().scale;
	}
	size_t i = 0;
	while(i + 2 < points.size() && r > points[i + 1].r){
		i++;
	}
	double r0 = points[i].r, r1 = points[i + 1].r;
	double v0 = points[i].scale, v1 = points[i + 1].scale;
	double h = r1 - r0;
	if(h <= 0){
		return v0;
	}
	// one sided or centered finite difference tangents
	double m0, m1;
	if(i == 0){
		m0 = (v1 - v0) / h;
	}else{
		double hr = points[i + 1].r - points[i - 1].r;
		m0 = hr > 0 ? (points[i + 1].scale - points[i - 1].scale) / hr : 0;
	}
	if(i + 2 >= points.size()){
		m1 = (v1 - v0) / h;
	}else{
		double hr = points[i + 2].r - points[i].r;
		m1 = hr > 0 ? (points[i + 2].scale - points[i].scale) / hr : 0;
	}
	double t = (r - r0) / h;
	double t2 = t * t, t3 = t2 * t;
	return (2 * t3 - 3 * t2 + 1) * v0 + (t3 - 2 * t2 + t) * h * m0
		+ (-2 * t3 + 3 * t2) * v1 + (t3 - t2) * h * m1;
}

#ifdef _WIN32

#include <d3dcompiler.h>
#include <dxgi.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include "../Config/ConfigLoader.h"

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "D3DCompiler.lib")

// log at most the first few occurrences of each kind of failure
#define PROCESSOR_ERROR(...) do{ if(errorCount++ < 20){ DriverLog(__VA_ARGS__); } }while(0)

// fullscreen triangle, no vertex buffer needed
static const char* vertexShaderSource = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID){
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
)";

// embedded fallback pixel shader. if vrlink_layer_ps.hlsl exists in the driver
// shader resources it is used instead and hot reloaded on change.
static const char* pixelShaderFallbackSource = R"(
// minimal fallback: distortion lut + saturation only. the full feature shader
// ships as vrlink_layer_ps.hlsl and is preferred when present.
cbuffer Params : register(b0){
	float saturation; float applyColor; float contrastMult; float contrastOffset;
	float contrastLinear; float outGamma; float casStrength; float casEnable;
	float annulusEnable; float annulusMin; float annulusMax; float annulusFeather;
	float ditherEnable; float lutMaxR; float aspect; float matrixEnable;
	float2 center; float2 boundsMin;
	float2 boundsSize; float2 texelSize;
	float4 colorMultiplier;
	float4 matR; float4 matG; float4 matB;
	float lutRowBase; float lutRowCount; float perAxisEnable; float dimAmount;
	float manualSrgb; float ditherLsb; float vibrance; float pad1;
	float gazeU; float gazeV; float gazeRing; float debugGrid;
	float projL; float projR; float projT; float projB;
	float gridSpacingRad; float dotU; float dotV; float dotMode;
	float overlayWarped; float tuneRingR; float tuneRingMode; float tuneRingAlpha;
	float3 hbx; float gridWorldLock;
	float3 hby; float padD;
	float3 hbz; float padE;
	float alignShiftU; float alignShiftV; float segCount; float tuneSegIdx;
	float tuneSegCount; float fxaaEnable; float padI; float padJ;
};
Texture2D<float4> tex : register(t0);
Texture2D<float4> lut : register(t1);
SamplerState samp : register(s0);

float3 LinearToSrgb(float3 c){
	c = max(c, 0.0);
	return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}
float3 SrgbToLinear(float3 c){
	c = max(c, 0.0);
	return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}

float SampleLutRow(float u, float row){
	return lut.SampleLevel(samp, float2(u, (row + 0.5) / lutRowCount), 0).x;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0{
	float2 p = uv - center;
	p.y *= aspect;
	float r = length(p);
	float u = r / lutMaxR;
	float s;
	if(perAxisEnable > 0.5){
		float wH = (p.x * p.x) / max(dot(p, p), 1e-9);
		s = SampleLutRow(u, lutRowBase) * wH + SampleLutRow(u, lutRowBase + 1) * (1.0 - wH);
	}else{
		s = SampleLutRow(u, lutRowBase);
	}
	if(annulusEnable > 0.5){
		float w = smoothstep(annulusMin - annulusFeather, annulusMin + annulusFeather, r)
			* (1.0 - smoothstep(annulusMax - annulusFeather, annulusMax + annulusFeather, r));
		s = 1.0 + (s - 1.0) * w;
	}
	p *= s;
	p.y /= aspect;
	float2 nSrc = p + center;
	float4 color = tex.SampleLevel(samp, nSrc * boundsSize + boundsMin, 0);
	if(manualSrgb > 0.5){
		color.rgb = SrgbToLinear(color.rgb);
	}
	if(any(nSrc < 0.0) || any(nSrc > 1.0)){
		color = float4(0, 0, 0, color.a);
	}
	if(applyColor > 0.5){
		float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));
		color.rgb = lerp(gray.xxx, color.rgb, saturation);
	}
	color.rgb *= 1.0 - dimAmount;
	if(manualSrgb > 0.5){
		color.rgb = LinearToSrgb(color.rgb);
	}
	return color;
}
)";

// mirrors the cbuffer in the pixel shader, hlsl packing: 4 scalars per register,
// float2 pairs share registers, float4 rows for the color matrix
struct FrameProcessorConstants{
	float saturation; float applyColor; float contrastMult; float contrastOffset;
	float contrastLinear; float outGamma; float casStrength; float casEnable;
	float annulusEnable; float annulusMin; float annulusMax; float annulusFeather;
	float ditherEnable; float lutMaxR; float aspect; float matrixEnable;
	float center[2]; float boundsMin[2];
	float boundsSize[2]; float texelSize[2];
	float colorMultiplier[4];
	float matR[4]; float matG[4]; float matB[4];
	float lutRowBase; float lutRowCountF; float perAxisEnable; float dimAmount;
	// vibrance promoted from pad0 (cbuffer size and every prior offset
	// unchanged): -1..1, 0 = off, saturation weighted by (1 - hsv sat)
	float manualSrgb; float ditherLsb; float vibrance; float pad1;
	float gazeU; float gazeV; float gazeRing; float pad2;
	float projL; float projR; float projT; float projB;
	float gridSpacingRad; float dotU; float dotV; float dotMode;
	float overlayWarped; float tuneRingR; float tuneRingMode; float tuneRingAlpha;
	// head basis columns (head axes in world) + world-locked grid flag
	float headXx, headXy, headXz, gridWorldLock;
	float headYx, headYy, headYz, padD;
	float headZx, headZy, headZz, padE;
	// per-eye whole-image alignment shift (prism correction), applied as a
	// constant offset on the source sample uv after the distortion warp.
	// cpu-resolved per eye from streamFrame.alignment.
	// band segments: segCount > 0 switches lut sampling to N angular rows
	// per eye (periodic interpolation); tuneSegIdx >= 0 restricts the
	// tuner ring to that segment's sector (-1 = full ring)
	float alignShiftU, alignShiftV, segCount, tuneSegIdx;
	// per-band layouts: the CURRENT band's segment count for the sector
	// highlight (the sampling row count above may be larger)
	float tuneSegCount, fxaaEnable, padI, padJ;
	// black floor: ramp bar enable, range remap mode (0/1/2), shadow
	// lift floor in sRGB code units (-1 = lift disabled), knee code
	float bfRampBar, bfRangeMode, bfShadowFloor, bfKnee;
	// sboys camera grid: opaque background flag; bfBlackPoint =
	// adjustable black point in sRGB code units (0 = off)
	float gridOpaque, bfBlackPoint, padL, padM;
	// displacement map: enable flag + array slice for this eye; general
	// brightness multiplier; blackout override
	float dispEnable, dispEye, brightness, blackout;
	// gray-code sweep: pattern index (-1 off), bits per axis, pattern
	// white level (linear), 1 = this eye shows the pattern (else black)
	float calibPattern, calibBits, calibLevel, calibEyeActive;
	// capture mode: desaturate + dim scene under the grid; grid line
	// level (linear) for the sboys pattern
	float gridDesat, gridLevel, captureOtherBlack, padO;
};

// map a layer texture format to the scratch format and shader mode used to
// process it. scratch must be in the same dxgi format family as the layer for
// the copies to be legal. rgba8/bgra8 use srgb typed views (hardware converts,
// shader sees linear). r10g10b10a2 has no srgb variant, so the shader decodes
// and re-encodes explicitly (manualSrgb) and dithers at the 10 bit lsb; the
// only difference vs the 8 bit path is that bilinear filtering happens on
// encoded values, which is visually negligible for near identity warps.
static bool MapLayerFormat(DXGI_FORMAT layerFormat, DXGI_FORMAT &scratchFormat, DXGI_FORMAT &rtvFormat, bool &manualSrgb, float &ditherLsb){
	// rtvFormat is the view format for rendering DIRECTLY into the layer
	// texture. a typeless layer accepts the srgb-typed cast (hardware
	// encode); a TYPED non-srgb layer only accepts a view of its own typed
	// format, so those switch to the manualSrgb path on BOTH ends: the
	// scratch input drops to the unorm type (raw bits, shader decodes) and
	// the shader encodes before the unorm RTV write. same tradeoff already
	// accepted for 10 bit (bilinear on encoded values, negligible for near
	// identity warps). field-observed game formats (29, 91) keep the
	// hardware srgb path.
	switch(layerFormat){
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			scratchFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			manualSrgb = false;
			ditherLsb = 255.0f;
			return true;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			scratchFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			manualSrgb = true;
			ditherLsb = 255.0f;
			return true;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			scratchFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			manualSrgb = false;
			ditherLsb = 255.0f;
			return true;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			scratchFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			manualSrgb = true;
			ditherLsb = 255.0f;
			return true;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			scratchFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
			rtvFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
			manualSrgb = true;
			ditherLsb = 1023.0f;
			return true;
		default:
			return false;
	}
}

static uint64_t NowMs(){
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t FrameProcessor::NowUs(){
	return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// find the dxgi adapter SteamVR renders on, from the hmd's reported luid.
// on hybrid gpu systems the default adapter can be the integrated gpu, and a
// device created there cannot open the shared layer textures (or limps across
// adapters), which shows up as "processing enabled but nothing changes".
static IDXGIAdapter1* FindAdapterForHmd(){
	uint64_t luidValue = 0;
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd);
	vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
	luidValue = vr::VRProperties()->GetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, &propError);
	if(propError != vr::TrackedProp_Success || luidValue == 0){
		DriverLog("FrameProcessor: hmd adapter luid unavailable (error %d), using default adapter", (int)propError);
		return nullptr;
	}
	IDXGIFactory1* factory = nullptr;
	if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory){
		return nullptr;
	}
	IDXGIAdapter1* found = nullptr;
	for(UINT i = 0; ; i++){
		IDXGIAdapter1* adapter = nullptr;
		if(factory->EnumAdapters1(i, &adapter) != S_OK || !adapter){
			break;
		}
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		uint64_t adapterLuid = ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) | (uint64_t)desc.AdapterLuid.LowPart;
		if(adapterLuid == luidValue){
			DriverLog("FrameProcessor: matched hmd adapter %ls", desc.Description);
			found = adapter;
			break;
		}
		adapter->Release();
	}
	factory->Release();
	if(!found){
		DriverLog("FrameProcessor: no adapter matched hmd luid %llx, using default adapter", (unsigned long long)luidValue);
	}
	return found;
}

bool FrameProcessor::EnsureDevice(){
	if(device){
		return true;
	}
	if(deviceFailed){
		return false;
	}
	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
	IDXGIAdapter1* adapter = FindAdapterForHmd();
	// with an explicit adapter the driver type must be unknown
	HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
	if(adapter){
		adapter->Release();
	}
	if(FAILED(hr)){
		deviceFailed = true;
		PROCESSOR_ERROR("FrameProcessor: failed to create D3D11 device: 0x%08X", (unsigned)hr);
		return false;
	}
	DriverLog("FrameProcessor: created D3D11 device");
	ReconLogger::Get().NoteProcessingDevice(device);
	ZeroCopyV3::Get().SetProcessingDevice(device);
	// recon (opt-in): shared-vtable context hooks are installed lazily from
	// ProcessEye once the flag is known; nothing here unless enabled.
	return true;
}

static std::string GetLayerShaderPath(){
	return driverConfigLoader.info.driverResources + "shaders/d3d11/vrlink_layer_ps.hlsl";
}

static std::string GetFxaaShaderPath(){
	return driverConfigLoader.info.driverResources + "shaders/d3d11/vrlink_fxaa_ps.hlsl";
}

static uint64_t GetFileTime(const std::string &path){
	std::error_code ec;
	auto t = std::filesystem::last_write_time(path, ec);
	if(ec){
		return 0;
	}
	return (uint64_t)t.time_since_epoch().count();
}

bool FrameProcessor::EnsureShaders(){
	// vertex shader, sampler, constant buffer: once
	if(!vertexShader){
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(vertexShaderSource, strlen(vertexShaderSource), "vs", nullptr, nullptr,
			"main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
		if(FAILED(hr) || !blob){
			if(errors){ PROCESSOR_ERROR("FrameProcessor: VS compile error: %s", (char*)errors->GetBufferPointer()); errors->Release(); }
			shaderFailed = true;
			return false;
		}
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vertexShader);
		blob->Release();
		if(FAILED(hr)){
			PROCESSOR_ERROR("FrameProcessor: CreateVertexShader failed: 0x%08X", (unsigned)hr);
			shaderFailed = true;
			return false;
		}

		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		if(FAILED(device->CreateSamplerState(&sd, &sampler))){
			shaderFailed = true;
			return false;
		}

		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = (sizeof(FrameProcessorConstants) + 15) & ~15u;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if(FAILED(device->CreateBuffer(&bd, nullptr, &constantBuffer))){
			shaderFailed = true;
			return false;
		}
	}

	// pixel shader: prefer the hlsl file, hot reload when it changes, check its
	// mtime at most once a second
	uint64_t now = NowMs();
	bool checkFile = now - lastShaderCheckMs > 1000 || !pixelShader;
	if(checkFile){
		lastShaderCheckMs = now;
		// pass 1 shader (fxaa quality mode): file only, no embedded
		// fallback — when absent or broken, quality degrades to the
		// in-pass fast path at runtime instead of failing frames
		{
			std::string fxPath = GetFxaaShaderPath();
			uint64_t fxTime = GetFileTime(fxPath);
			if(fxTime != fxaaShaderFileTime){
				fxaaShaderFileTime = fxTime;
				std::string fxSource;
				if(fxTime != 0){
					std::ifstream fxFile(fxPath, std::ios::binary);
					if(fxFile){
						fxSource.assign(std::istreambuf_iterator<char>(fxFile), std::istreambuf_iterator<char>());
					}
				}
				if(!fxSource.empty()){
					ID3DBlob* fxBlob = nullptr;
					ID3DBlob* fxErrors = nullptr;
					if(SUCCEEDED(D3DCompile(fxSource.c_str(), fxSource.size(), "vrlink_fxaa_ps", nullptr, nullptr,
						"main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &fxBlob, &fxErrors)) && fxBlob){
						ID3D11PixelShader* fxNew = nullptr;
						if(SUCCEEDED(device->CreatePixelShader(fxBlob->GetBufferPointer(), fxBlob->GetBufferSize(), nullptr, &fxNew))){
							if(fxaaShader){ fxaaShader->Release(); }
							fxaaShader = fxNew;
							hdFrameTags |= TagShaderCompile;
							DriverLog("FrameProcessor: fxaa pass shader ready (from file)");
						}
						fxBlob->Release();
					}else if(fxErrors){
						DriverLog("FrameProcessor: FXAA PS compile error: %s", (char*)fxErrors->GetBufferPointer());
					}
					if(fxErrors){ fxErrors->Release(); }
				}
			}
		}
		std::string path = GetLayerShaderPath();
		uint64_t fileTime = GetFileTime(path);
		if(pixelShader && fileTime == pixelShaderFileTime){
			return true;
		}
		std::string source = pixelShaderFallbackSource;
		if(fileTime != 0){
			std::ifstream file(path, std::ios::binary);
			if(file){
				source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}else{
				fileTime = 0;
			}
		}
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(source.c_str(), source.size(), "vrlink_layer_ps", nullptr, nullptr,
			"main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
		if(FAILED(hr) || !blob){
			if(errors){
				DriverLog("FrameProcessor: PS compile error: %s", (char*)errors->GetBufferPointer());
				errors->Release();
			}
			// keep the previous shader if there is one so live edits with a typo
			// do not black out the processing
			pixelShaderFileTime = fileTime;
			if(pixelShader){
				return true;
			}
			shaderFailed = true;
			return false;
		}
		ID3D11PixelShader* newShader = nullptr;
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &newShader);
		blob->Release();
		if(FAILED(hr)){
			PROCESSOR_ERROR("FrameProcessor: CreatePixelShader failed: 0x%08X", (unsigned)hr);
			if(pixelShader){
				return true;
			}
			shaderFailed = true;
			return false;
		}
		if(pixelShader){
			pixelShader->Release();
		}
		pixelShader = newShader;
		pixelShaderFileTime = fileTime;
		shaderFailed = false;
		hdFrameTags |= TagShaderCompile;
		// capability provenance: a stale hlsl next to a new dll fails
		// SILENTLY for appended features (cbuffer appends are layout
		// compatible), so name what this shader source actually contains
		bool hasGazeRing = source.find("gazeRing > 0.5") != std::string::npos;
		bool hasCalibDot = source.find("dotMode > 0.5") != std::string::npos;
		bool hasWarpedOverlay = source.find("overlayWarped > 0.5") != std::string::npos;
		bool hasTuneRing = source.find("tuneRingMode > 0.5") != std::string::npos;
		bool hasWorldGrid = source.find("gridWorldLock > 0.5") != std::string::npos;
		// token is the USAGE expression: the embedded fallback declares the
		// cbuffer fields (layout identity) without applying them
		bool hasAlignShift = source.find("float2(alignShiftU") != std::string::npos;
		bool hasSegments = source.find("SampleLutSegmented(") != std::string::npos;
		bool hasFxaa = source.find("fxaaEnable > 0.5") != std::string::npos;
		bool hasAuxMarkers = source.find("dotMode > 2.5") != std::string::npos;
		bool hasDispMap = source.find("dispEnable > 0.5") != std::string::npos;
		bool hasCalib = source.find("calibPattern > -0.5") != std::string::npos;
		DriverLog("FrameProcessor: pixel shader ready (%s, gaze ring support: %s, calib dot support: %s, warped overlays: %s, tuner ring: %s, world grid: %s, align shift: %s, band segments: %s, fxaa: %s, fxaaPass: %s)",
			fileTime ? "from file" : "embedded", hasGazeRing ? "yes" : "NO - stale hlsl?",
			hasCalibDot ? "yes" : "NO - stale hlsl?", hasWarpedOverlay ? "yes" : "NO - stale hlsl?",
			hasTuneRing ? "yes" : "NO - stale hlsl?", hasWorldGrid ? "yes" : "NO - stale hlsl?",
			hasAlignShift ? "yes" : "NO - stale hlsl?", hasSegments ? "yes" : "NO - stale hlsl?", hasFxaa ? "yes" : "NO - stale hlsl?", fxaaShader ? "yes" : "NO - file missing?");
		if(!hasAuxMarkers){
			DriverLog("FrameProcessor: aux markers (center cross / tip marker): NO - stale hlsl?");
		}
		if(!hasDispMap || !hasCalib){
			DriverLog("FrameProcessor: displacement map: %s, calibration patterns/blackout: %s",
				hasDispMap ? "yes" : "NO - stale hlsl?", hasCalib ? "yes" : "NO - stale hlsl?");
		}
	}
	return pixelShader != nullptr;
}

void FrameProcessor::ReleaseScratchSet(ScratchSet &set){
	if(set.fxSRV){ set.fxSRV->Release(); set.fxSRV = nullptr; }
	if(set.fxRTV){ set.fxRTV->Release(); set.fxRTV = nullptr; }
	if(set.fx){ set.fx->Release(); set.fx = nullptr; }
	if(set.inSRV){ set.inSRV->Release(); set.inSRV = nullptr; }
	if(set.in){ set.in->Release(); set.in = nullptr; }
	if(set.outRTV){ set.outRTV->Release(); set.outRTV = nullptr; }
	if(set.out){ set.out->Release(); set.out = nullptr; }
}

bool FrameProcessor::EnsureScratch(uint32_t width, uint32_t height, DXGI_FORMAT format, bool needOut){
	uint64_t key = ((uint64_t)format << 48) | ((uint64_t)width << 24) | (uint64_t)height;
	uint64_t now = NowMs();
	auto found = scratchSets.find(key);
	if(found != scratchSets.end()){
		// the out target is lazy (only the copy-back fallback needs it):
		// upgrade a direct-path set in place when a texture falls back
		if(needOut && !found->second.out){
			D3D11_TEXTURE2D_DESC outDesc = {};
			outDesc.Width = width;
			outDesc.Height = height;
			outDesc.MipLevels = 1;
			outDesc.ArraySize = 1;
			outDesc.Format = format;
			outDesc.SampleDesc.Count = 1;
			outDesc.Usage = D3D11_USAGE_DEFAULT;
			outDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
			if(FAILED(device->CreateTexture2D(&outDesc, nullptr, &found->second.out))){
				PROCESSOR_ERROR("FrameProcessor: failed to create scratchOut %ux%u", width, height);
				return false;
			}
			if(FAILED(device->CreateRenderTargetView(found->second.out, nullptr, &found->second.outRTV))){
				found->second.out->Release();
				found->second.out = nullptr;
				return false;
			}
			DriverLog("FrameProcessor: added scratchOut %ux%u format=%u (fallback path)",
				width, height, (unsigned)format);
		}
		found->second.lastUsedMs = now;
		if(cfgFxaaQuality && !found->second.fx){
			EnsureFxTexture(found->second, width, height, format);
		}
		scratchIn = found->second.in;
		scratchInSRV = found->second.inSRV;
		scratchOut = found->second.out;
		scratchOutRTV = found->second.outRTV;
		scratchFx = found->second.fx;
		scratchFxSRV = found->second.fxSRV;
		scratchFxRTV = found->second.fxRTV;
		return true;
	}

	ScratchSet set;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;

	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if(FAILED(device->CreateTexture2D(&desc, nullptr, &set.in))){
		PROCESSOR_ERROR("FrameProcessor: failed to create scratchIn %ux%u", width, height);
		return false;
	}
	if(FAILED(device->CreateShaderResourceView(set.in, nullptr, &set.inSRV))){
		ReleaseScratchSet(set);
		return false;
	}
	if(needOut){
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		if(FAILED(device->CreateTexture2D(&desc, nullptr, &set.out))){
			PROCESSOR_ERROR("FrameProcessor: failed to create scratchOut %ux%u", width, height);
			ReleaseScratchSet(set);
			return false;
		}
		if(FAILED(device->CreateRenderTargetView(set.out, nullptr, &set.outRTV))){
			ReleaseScratchSet(set);
			return false;
		}
	}
	set.lastUsedMs = now;

	// bound the cache: evict the least recently used entry beyond the cap.
	// deferred mode (default) moves the victim to pendingEvictions - the
	// map entry disappears now, but the D3D resources are released a few
	// frames later OUTSIDE the keyed-mutex hold, so an eviction never adds
	// release cost to the frame that already pays the creation stall.
	while(scratchSets.size() >= maxScratchSets){
		auto lru = scratchSets.begin();
		for(auto it = scratchSets.begin(); it != scratchSets.end(); ++it){
			if(it->second.lastUsedMs < lru->second.lastUsedMs){
				lru = it;
			}
		}
		hdEvicts++;
		if(cfgDeferEvict){
			PendingEvict pe;
			pe.set = lru->second;
			pe.frame = frameCounter;
			pendingEvictions.push_back(pe);
		}else{
			ReleaseScratchSet(lru->second);
		}
		scratchSets.erase(lru);
	}
	auto inserted = scratchSets.emplace(key, set).first;
	if(cfgFxaaQuality){
		EnsureFxTexture(inserted->second, width, height, format);
	}
	scratchIn = inserted->second.in;
	scratchInSRV = inserted->second.inSRV;
	scratchOut = inserted->second.out;
	scratchOutRTV = inserted->second.outRTV;
	scratchFx = inserted->second.fx;
	scratchFxSRV = inserted->second.fxSRV;
	scratchFxRTV = inserted->second.fxRTV;
	hdCreates++;
	hdFrameTags |= TagScratchCreate;
	DriverLog("FrameProcessor: created scratch textures %ux%u format=%u (%zu sets cached, %zu pending evict)",
		width, height, (unsigned)format, scratchSets.size(), pendingEvictions.size());
	return true;
}

void FrameProcessor::EnsureFxTexture(ScratchSet &set, uint32_t width, uint32_t height, DXGI_FORMAT format){
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	if(FAILED(device->CreateTexture2D(&desc, nullptr, &set.fx))){
		PROCESSOR_ERROR("FrameProcessor: failed to create fxaa intermediate %ux%u", width, height);
		set.fx = nullptr;
		return;
	}
	if(FAILED(device->CreateShaderResourceView(set.fx, nullptr, &set.fxSRV)) ||
		FAILED(device->CreateRenderTargetView(set.fx, nullptr, &set.fxRTV))){
		if(set.fxSRV){ set.fxSRV->Release(); set.fxSRV = nullptr; }
		set.fx->Release();
		set.fx = nullptr;
		return;
	}
	hdFrameTags |= TagScratchCreate;
	DriverLog("FrameProcessor: added fxaa intermediate %ux%u format=%u", width, height, (unsigned)format);
}

void FrameProcessor::DrainPendingEvictions(bool force){
	if(pendingEvictions.empty()){
		return;
	}
	if(force){
		for(auto &pe : pendingEvictions){
			ReleaseScratchSet(pe.set);
		}
		pendingEvictions.clear();
		return;
	}
	// steady state: release at most ONE aged victim per frame, spreading
	// release cost across frames instead of stacking it on a transition
	for(size_t i = 0; i < pendingEvictions.size(); i++){
		if(frameCounter - pendingEvictions[i].frame >= 3){
			ReleaseScratchSet(pendingEvictions[i].set);
			pendingEvictions.erase(pendingEvictions.begin() + i);
			break;
		}
	}
}

static const int lutSize = 512;
static const float lutMaxRadius = 1.0f;


// resolve the curve to use for a given eye (0/1, or -1 when not per eye) and
// axis (0 horizontal / 1 vertical, or -1 when not per axis). a missing named
// curve falls back to the base curve.
struct EffectiveCurve{
	double k1 = 0;
	double k2 = 0;
	const std::vector<StreamFrameDistortionPoint>* points = nullptr;
};
static EffectiveCurve ResolveCurve(const StreamFrameConfig &config, int eye, int axis, int seg = -1){
	EffectiveCurve result;
	result.k1 = config.k1;
	result.k2 = config.k2;
	result.points = &config.distortion.points;
	auto tryKey = [&](const std::string &tryName){
		auto found = config.distortion.curves.find(tryName);
		if(found != config.distortion.curves.end()){
			result.k1 = found->second.k1;
			result.k2 = found->second.k2;
			result.points = &found->second.points;
			return true;
		}
		return false;
	};
	// segment curves ("left#0"..) fall back to the plain per-eye curve,
	// which falls back to the base — a missing segment shows the radial
	// curve there instead of identity
	if(seg >= 0 && eye >= 0){
		if(tryKey(std::string(eye == 0 ? "left" : "right") + "#" + std::to_string(seg))){
			return result;
		}
	}
	std::string key = "";
	if(eye >= 0 && axis >= 0){
		key = std::string(eye == 0 ? "left" : "right") + (axis == 0 ? "Horizontal" : "Vertical");
	}else if(eye >= 0){
		key = eye == 0 ? "left" : "right";
	}else if(axis >= 0){
		key = axis == 0 ? "horizontal" : "vertical";
	}
	if(!key.empty()){
		tryKey(key);
	}
	return result;
}

// clamp the configured segment count: any 2..32 row count is valid (per
// band layouts flatten into arbitrary max counts); outside that = radial
static int EffectiveSegments(const StreamFrameConfig &config){
	int segs = config.distortion.segments;
	if(segs < 2 || segs > 32){
		return 1;
	}
	return segs;
}

// serialize everything the lut depends on, for change detection
static std::string BuildLutKey(const StreamFrameConfig &config){
	std::string key = config.distortion.mode;
	key += config.distortion.perEye ? "|E" : "|e";
	key += config.distortion.perAxis ? "A" : "a";
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "|g%.9g", config.distortion.gain);
	key += buffer;
	auto appendCurve = [&](const EffectiveCurve &curve){
		snprintf(buffer, sizeof(buffer), "|%.9g,%.9g", curve.k1, curve.k2);
		key += buffer;
		for(const auto &point : *curve.points){
			snprintf(buffer, sizeof(buffer), ";%.9g:%.9g", point.r, point.scale);
			key += buffer;
		}
	};
	int segs = EffectiveSegments(config);
	snprintf(buffer, sizeof(buffer), "|S%d", segs);
	key += buffer;
	int eyeCount = config.distortion.perEye ? 2 : 1;
	int axisCount = (config.distortion.perAxis && segs == 1) ? 2 : 1;
	if(segs > 1){
		for(int eye = 0; eye < eyeCount; eye++){
			for(int seg = 0; seg < segs; seg++){
				appendCurve(ResolveCurve(config, config.distortion.perEye ? eye : 0, -1, seg));
			}
		}
	}else{
		for(int eye = 0; eye < eyeCount; eye++){
			for(int axis = 0; axis < axisCount; axis++){
				appendCurve(ResolveCurve(config, config.distortion.perEye ? eye : -1, config.distortion.perAxis ? axis : -1));
			}
		}
	}
	return key;
}

bool FrameProcessor::BakeLutIfNeeded(const StreamFrameConfig &config){
	std::string key = BuildLutKey(config);
	if(lutBaked && key == lastLutKey){
		return true;
	}

	// band segments replace the perAxis pair with N angular rows per eye;
	// rows: eye major, segment (or axis) minor
	int segs = EffectiveSegments(config);
	int eyeCount = config.distortion.perEye ? 2 : 1;
	int axisCount = (config.distortion.perAxis && segs == 1) ? 2 : 1;
	int rowCount = eyeCount * (segs > 1 ? segs : axisCount);
	// recreate the texture when the row count changes
	if(lutTexture && rowCount != lutRowCount){
		if(lutSRV){ lutSRV->Release(); lutSRV = nullptr; }
		lutTexture->Release();
		lutTexture = nullptr;
	}
	if(!lutTexture){
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = lutSize;
		desc.Height = rowCount;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if(FAILED(device->CreateTexture2D(&desc, nullptr, &lutTexture))){
			PROCESSOR_ERROR("FrameProcessor: failed to create lut texture");
			return false;
		}
		if(FAILED(device->CreateShaderResourceView(lutTexture, nullptr, &lutSRV))){
			return false;
		}
		lutRowCount = rowCount;
	}

	std::vector<float> data(lutSize * rowCount);
	bool spline = config.distortion.mode == "spline";
	// row order: eye major, segment/axis minor
	int minorCount = segs > 1 ? segs : axisCount;
	for(int eye = 0; eye < eyeCount; eye++){
		for(int minor = 0; minor < minorCount; minor++){
			int axis = segs > 1 ? -1 : (config.distortion.perAxis ? minor : -1);
			int seg = segs > 1 ? minor : -1;
			int row = eye * minorCount + minor;
			EffectiveCurve curve = ResolveCurve(config, config.distortion.perEye ? eye : (segs > 1 ? 0 : -1), axis, seg);
			std::vector<StreamFrameDistortionPoint> sortedPoints = *curve.points;
			std::sort(sortedPoints.begin(), sortedPoints.end(), [](const StreamFrameDistortionPoint &a, const StreamFrameDistortionPoint &b){
				return a.r < b.r;
			});
			for(int i = 0; i < lutSize; i++){
				double r = (double)i / (lutSize - 1) * lutMaxRadius;
				double scale;
				if(spline){
					scale = EvaluateDistortionCurve(sortedPoints, r);
				}else{
					double r2 = r * r;
					scale = 1.0 + curve.k1 * r2 + curve.k2 * r2 * r2;
				}
				// perceptual-search gain: scale toward/away from identity
				scale = 1.0 + config.distortion.gain * (scale - 1.0);
				data[row * lutSize + i] = (float)scale;
			}
		}
	}
	context->UpdateSubresource(lutTexture, 0, nullptr, data.data(), lutSize * sizeof(float), 0);

	lastLutKey = key;
	lutBaked = true;
	hdFrameTags |= TagLutBake;
	DriverLog("FrameProcessor: baked distortion lut (%s, %d rows)", spline ? "spline" : "k1k2", rowCount);
	return true;
}

// Catmull-Rom weight for the 4 taps around a sample position
static inline void CatmullRomWeights(double t, double w[4]){
	double t2 = t * t, t3 = t2 * t;
	w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
	w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
	w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
	w[3] = 0.5 * (t3 - t2);
}

// bicubic (Catmull-Rom, clamped edges) upsample of one eye's control lattice
// into the dense texture; identity (zeros) when the lattice is unusable
static void UpsampleDisplacement(const std::vector<double> &lattice, int cols, int rows,
	double gain, int size, std::vector<float> &out){
	out.assign((size_t)size * size * 2, 0.0f);
	if(cols < 2 || rows < 2 || lattice.size() != (size_t)cols * rows * 2){
		return;
	}
	auto at = [&](int c, int r, int k){
		if(c < 0){ c = 0; }
		if(c > cols - 1){ c = cols - 1; }
		if(r < 0){ r = 0; }
		if(r > rows - 1){ r = rows - 1; }
		return lattice[((size_t)r * cols + c) * 2 + k];
	};
	for(int y = 0; y < size; y++){
		// texel centers map onto the lattice so that texel 0 = knot 0 and
		// texel size-1 = knot rows-1 (the shader samples with uv * (1 -
		// 1/size) + 0.5/size to hit centers exactly)
		double fy = (double)y / (size - 1) * (rows - 1);
		int r0 = (int)fy;
		if(r0 > rows - 2){ r0 = rows - 2; }
		double ty = fy - r0;
		double wy[4];
		CatmullRomWeights(ty, wy);
		for(int x = 0; x < size; x++){
			double fx = (double)x / (size - 1) * (cols - 1);
			int c0 = (int)fx;
			if(c0 > cols - 2){ c0 = cols - 2; }
			double tx = fx - c0;
			double wx[4];
			CatmullRomWeights(tx, wx);
			for(int k = 0; k < 2; k++){
				double acc = 0.0;
				for(int j = 0; j < 4; j++){
					double rowAcc = 0.0;
					for(int i = 0; i < 4; i++){
						rowAcc += wx[i] * at(c0 - 1 + i, r0 - 1 + j, k);
					}
					acc += wy[j] * rowAcc;
				}
				out[((size_t)y * size + x) * 2 + k] = (float)(acc * gain);
			}
		}
	}
}

// serialize everything the displacement texture depends on
static std::string BuildMapKey(const StreamFrameConfig &config){
	const StreamFrameDisplacementMap &map = config.distortion.map;
	std::string key = std::to_string(map.enable) + "|" + std::to_string(map.cols) + "x" + std::to_string(map.rows)
		+ "|g" + std::to_string(config.distortion.gain) + "|";
	// hash the values rather than dumping ~4k numbers into the key
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](const std::vector<double> &v){
		for(double d : v){
			uint64_t bits;
			memcpy(&bits, &d, sizeof(bits));
			h ^= bits;
			h *= 1099511628211ull;
		}
		h ^= (uint64_t)v.size();
		h *= 1099511628211ull;
	};
	mix(map.left);
	mix(map.right);
	key += std::to_string(h);
	return key;
}

bool FrameProcessor::BakeMapIfNeeded(const StreamFrameConfig &config){
	std::string key = BuildMapKey(config);
	if(mapBaked && key == lastMapKey){
		return true;
	}
	const StreamFrameDisplacementMap &map = config.distortion.map;
	bool leftOk = map.left.size() == (size_t)map.cols * map.rows * 2;
	bool rightOk = map.right.size() == (size_t)map.cols * map.rows * 2;
	dispActive = map.enable && map.cols >= 2 && map.rows >= 2 && (leftOk || rightOk);
	lastMapKey = key;
	mapBaked = true;
	if(!dispActive){
		// nothing to sample; the texture (if any) is left alone but unused
		if(map.cols > 0 || !map.left.empty() || !map.right.empty()){
			DriverLog("FrameProcessor: displacement map inactive (enable=%d cols=%d rows=%d left=%zu right=%zu values)",
				(int)map.enable, map.cols, map.rows, map.left.size(), map.right.size());
		}
		return true;
	}
	if(!dispTexture){
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = dispTexSize;
		desc.Height = dispTexSize;
		desc.MipLevels = 1;
		desc.ArraySize = 2;
		desc.Format = DXGI_FORMAT_R32G32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if(FAILED(device->CreateTexture2D(&desc, nullptr, &dispTexture))){
			PROCESSOR_ERROR("FrameProcessor: failed to create displacement texture");
			dispActive = false;
			return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = 2;
		if(FAILED(device->CreateShaderResourceView(dispTexture, &srvDesc, &dispSRV))){
			PROCESSOR_ERROR("FrameProcessor: failed to create displacement srv");
			dispTexture->Release();
			dispTexture = nullptr;
			dispActive = false;
			return false;
		}
	}
	std::vector<float> data;
	double maxAbs[2] = {0, 0};
	for(int eye = 0; eye < 2; eye++){
		const std::vector<double> &lattice = eye == 0 ? map.left : map.right;
		// a missing eye is identity (zeros), never the other eye's data
		UpsampleDisplacement((eye == 0 ? leftOk : rightOk) ? lattice : std::vector<double>(), map.cols, map.rows,
			config.distortion.gain, dispTexSize, data);
		for(float v : data){
			if(fabs(v) > maxAbs[eye]){ maxAbs[eye] = fabs(v); }
		}
		context->UpdateSubresource(dispTexture, D3D11CalcSubresource(0, eye, 1), nullptr, data.data(),
			dispTexSize * 2 * sizeof(float), 0);
	}
	hdFrameTags |= TagLutBake;
	DriverLog("FrameProcessor: baked displacement map (%dx%d lattice -> %d texels, gain %.3f, max |disp| L=%.5f R=%.5f uv, source=%s)",
		map.cols, map.rows, dispTexSize, config.distortion.gain, maxAbs[0], maxAbs[1], map.source.c_str());
	return true;
}
ID3D11Texture2D* FrameProcessor::OpenShared(vr::SharedTextureHandle_t handle){
	auto it = openedTextures.find((uint64_t)handle);
	if(it != openedTextures.end()){
		return it->second;
	}
	ID3D11Texture2D* texture = nullptr;
	// legacy shared handle (observed unTextureFlags == 0)
	HRESULT hr = device->OpenSharedResource((HANDLE)(uintptr_t)handle, __uuidof(ID3D11Texture2D), (void**)&texture);
	if(FAILED(hr) || !texture){
		PROCESSOR_ERROR("FrameProcessor: OpenSharedResource failed for %llx: 0x%08X", (unsigned long long)handle, (unsigned)hr);
		return nullptr;
	}
	openedTextures[(uint64_t)handle] = texture;
	return texture;
}

ID3D11RenderTargetView* FrameProcessor::GetLayerRTV(ID3D11Texture2D* texture, int slice, DXGI_FORMAT rtvFormat){
	if(slice < 0 || slice > 1){
		return nullptr;
	}
	auto found = layerRTVs.find(texture);
	if(found != layerRTVs.end() && found->second[slice]){
		return found->second[slice];
	}
	// the array dimension is legal for any Texture2D (a plain texture is an
	// array of 1), so one view description covers both plain and
	// single-pass-instanced layers
	D3D11_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format = rtvFormat;
	desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
	desc.Texture2DArray.MipSlice = 0;
	desc.Texture2DArray.FirstArraySlice = (UINT)slice;
	desc.Texture2DArray.ArraySize = 1;
	ID3D11RenderTargetView* rtv = nullptr;
	if(FAILED(device->CreateRenderTargetView(texture, &desc, &rtv)) || !rtv){
		return nullptr;
	}
	layerRTVs[texture][slice] = rtv;
	return rtv;
}

ID3D11ShaderResourceView* FrameProcessor::GetLayerSRV(ID3D11Texture2D* texture, int slice, DXGI_FORMAT srvFormat){
	if(slice < 0 || slice > 1){
		return nullptr;
	}
	auto found = layerSRVs.find(texture);
	if(found != layerSRVs.end() && found->second[slice]){
		return found->second[slice];
	}
	// array dimension covers plain and single-pass-instanced layers, same
	// as the RTV analog
	D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = srvFormat;
	desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	desc.Texture2DArray.MostDetailedMip = 0;
	desc.Texture2DArray.MipLevels = 1;
	desc.Texture2DArray.FirstArraySlice = (UINT)slice;
	desc.Texture2DArray.ArraySize = 1;
	ID3D11ShaderResourceView* srv = nullptr;
	if(FAILED(device->CreateShaderResourceView(texture, &desc, &srv)) || !srv){
		return nullptr;
	}
	layerSRVs[texture][slice] = srv;
	return srv;
}

void FrameProcessor::ReleaseShadowSet(ShadowSet &set){
	for(int i = 0; i < ShadowSet::slots; i++){
		if(set.tex[i]){
			set.tex[i]->Release();
			set.tex[i] = nullptr;
		}
		set.handle[i] = nullptr;
	}
}

bool FrameProcessor::EnsureShadow(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t arraySize, ShadowSet*& outSet){
	uint64_t key = ((uint64_t)width << 32) | height;
	auto found = shadowSets.find(key);
	if(found != shadowSets.end() && found->second.arraySize != arraySize){
		// layer shape changed under the same size key: rebuild
		ReleaseShadowSet(found->second);
		shadowSets.erase(found);
		found = shadowSets.end();
	}
	if(found == shadowSets.end()){
		ShadowSet set;
		set.arraySize = arraySize;
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = arraySize;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		// legacy-shared, matching the layer's own sharing mode (misc=0x2):
		// vrlink's device opens the handle in the copy detour
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
		bool ok = true;
		for(int i = 0; i < ShadowSet::slots && ok; i++){
			if(FAILED(device->CreateTexture2D(&desc, nullptr, &set.tex[i])) || !set.tex[i]){
				ok = false;
				break;
			}
			IDXGIResource* dxgi = nullptr;
			if(FAILED(set.tex[i]->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgi)) || !dxgi){
				ok = false;
				break;
			}
			HRESULT hr = dxgi->GetSharedHandle(&set.handle[i]);
			dxgi->Release();
			if(FAILED(hr) || !set.handle[i]){
				ok = false;
				break;
			}
		}
		if(!ok){
			ReleaseShadowSet(set);
			PROCESSOR_ERROR("zero-copy v3: shadow set creation failed for %ux%u", width, height);
			return false;
		}
		for(int i = 0; i < ShadowSet::slots; i++){
			ZeroCopyV3::Get().PublishShadow(width, height, i, set.handle[i]);
		}
		found = shadowSets.emplace(key, set).first;
	}
	ShadowSet &set = found->second;
	// rotate once per frame; both eyes / slices of a frame share the slot
	if(set.lastFrame != frameCounter){
		set.lastFrame = frameCounter;
		set.index = (set.index + 1) % ShadowSet::slots;
	}
	set.usedThisFrame = true;
	outSet = &set;
	return true;
}

void FrameProcessor::EvictTexture(vr::SharedTextureHandle_t handle){
	std::lock_guard<std::mutex> guard(lock);
	auto it = openedTextures.find((uint64_t)handle);
	if(it != openedTextures.end()){
		auto rtvIt = layerRTVs.find(it->second);
		if(rtvIt != layerRTVs.end()){
			for(auto *rtv : rtvIt->second){
				if(rtv){ rtv->Release(); }
			}
			layerRTVs.erase(rtvIt);
		}
		auto srvIt = layerSRVs.find(it->second);
		if(srvIt != layerSRVs.end()){
			for(auto *srv : srvIt->second){
				if(srv){ srv->Release(); }
			}
			layerSRVs.erase(srvIt);
		}
		layerRtvFailed.erase(it->second);
		layerPathLogged.erase(it->second);
		layerCopyFrame.erase(it->second);
		v3Failed.erase(it->second);
		it->second->Release();
		openedTextures.erase(it);
	}
}

void FrameProcessor::EvictAll(){
	std::lock_guard<std::mutex> guard(lock);
	// pending scratch evictions: release now - teardown is already a load
	// boundary and stranding aged victims across it would leak VRAM
	DrainPendingEvictions(true);
	for(auto &pair : layerRTVs){
		for(auto *rtv : pair.second){
			if(rtv){ rtv->Release(); }
		}
	}
	layerRTVs.clear();
	for(auto &pair : layerSRVs){
		for(auto *srv : pair.second){
			if(srv){ srv->Release(); }
		}
	}
	layerSRVs.clear();
	layerRtvFailed.clear();
	layerPathLogged.clear();
	layerCopyFrame.clear();
	v3Failed.clear();
	for(auto &pair : openedTextures){
		pair.second->Release();
	}
	openedTextures.clear();
	// scratch sets deliberately SURVIVE app teardown: this is called on
	// DestroyAllSwapTextureSets (app switches, dashboard flips), and
	// re-creating multi hundred MB sets seconds later was a visible load
	// hitch (field log: 25 re-creations in a 21 minute session). the LRU
	// cap still bounds the cache; the D3D device outlives app teardown so
	// the sets stay valid.
	scratchIn = nullptr;
	scratchInSRV = nullptr;
	scratchOut = nullptr;
	scratchOutRTV = nullptr;
}

bool FrameProcessor::ProcessEye(ID3D11Texture2D* texture, const vr::VRTextureBounds_t &bounds, int eye, int slice, const FrameProcessSettings &settings){
	D3D11_TEXTURE2D_DESC desc = {};
	texture->GetDesc(&desc);
	if(settings.config.reconLogger){
		ReconLogger::Get().SetEnabled(true);
		ReconLogger::Get().NoteLayerDimensions(desc.Width, desc.Height);
		ReconLogger::Get().InstallOnce(context);
	}
	DXGI_FORMAT mappedScratchFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT layerRtvFormat = DXGI_FORMAT_UNKNOWN;
	bool manualSrgb = false;
	float ditherLsb = 255.0f;
	if(!MapLayerFormat(desc.Format, mappedScratchFormat, layerRtvFormat, manualSrgb, ditherLsb)){
		// log each unsupported format once per session, outside the error
		// budget, so a game launched late still reports why it is untouched
		if(skippedFormats.insert((unsigned)desc.Format).second){
			DriverLog("FrameProcessor: unsupported layer format %u, this app's frames are not processed", (unsigned)desc.Format);
		}
		return false;
	}
	// zero-copy v3 tier (above direct render): sample the layer directly
	// and draw into the rotating shared shadow; vrlink's staging copy is
	// redirected to the shadow (ZeroCopy.cpp) so the layer is never
	// written and scratchIn is never copied — 4x -> 2x traffic. sticky
	// per-texture fallback to the normal tiers on any setup failure.
	bool v3Active = false;
	ShadowSet* shadow = nullptr;
	ID3D11ShaderResourceView* layerSRV = nullptr;
	ID3D11RenderTargetView* shadowRTV = nullptr;
	if(settings.config.zeroCopyV3 && v3Failed.find(texture) == v3Failed.end()){
		ReconLogger::Get().InstallOnce(context);
		layerSRV = GetLayerSRV(texture, slice, mappedScratchFormat);
		if(layerSRV && EnsureShadow(desc.Width, desc.Height, desc.Format, desc.ArraySize, shadow)){
			shadowRTV = GetLayerRTV(shadow->tex[shadow->index], slice, layerRtvFormat);
		}
		if(shadowRTV){
			v3Active = true;
		}else{
			v3Failed.insert(texture);
			DriverLog("zero-copy v3: setup failed for %ux%u layer (SRV/shadow/RTV) — normal path for this texture",
				desc.Width, desc.Height);
		}
	}
	// in-place tiers: direct render into the layer, else copy-back
	ID3D11RenderTargetView* directRTV = nullptr;
	if(!v3Active && settings.config.directRender && layerRtvFailed.find(texture) == layerRtvFailed.end()){
		directRTV = GetLayerRTV(texture, slice, layerRtvFormat);
		if(!directRTV){
			layerRtvFailed.insert(texture);
			DriverLog("FrameProcessor: layer texture refused an RTV (format=%u), falling back to copy-back path for this texture", (unsigned)desc.Format);
		}
	}
	if(layerPathLogged.insert(texture).second){
		DriverLog("FrameProcessor: %s path for %ux%u format=%u layer",
			v3Active ? "zero-copy v3" : (directRTV ? "direct render" : "copy-back"), desc.Width, desc.Height, (unsigned)desc.Format);
	}
	scratchFx = nullptr;
	scratchFxSRV = nullptr;
	scratchFxRTV = nullptr;
	if(!v3Active && !EnsureScratch(desc.Width, desc.Height, mappedScratchFormat, directRTV == nullptr)){
		return false;
	}

	// copy the layer into the scratch input (same size and format). a
	// subresource copy instead of CopyResource: for single-pass-instanced
	// apps the layer is a Texture2DArray and CopyResource against the
	// non-array scratch is an invalid call D3D DROPS SILENTLY — the shader
	// then samples a blank scratch and the copy-back blacks out slice 0
	// (the left eye) while slice 1 renders untouched (the Demeo one-eye
	// bug). the subresource form is legal for both plain and array layers.
	// v3 samples the layer directly: no copy at all.
	UINT layerSub = D3D11CalcSubresource(0, (UINT)slice, desc.MipLevels);
	if(!v3Active){
		// skip the copy if this exact subresource was already copied into
		// this scratch THIS frame (side-by-side layouts process the same
		// slice twice; nothing else writes scratchIn in between)
		uint64_t &lastFrame = layerCopyFrame[texture][slice];
		if(lastFrame != frameCounter){
			context->CopySubresourceRegion(scratchIn, 0, 0, 0, 0, texture, layerSub, nullptr);
			lastFrame = frameCounter;
		}
	}

	// constants
	const StreamFrameConfig &config = settings.config;
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if(FAILED(context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))){
		return false;
	}
	FrameProcessorConstants constants = {};
	constants.saturation = (float)(config.saturation / 50.0);
	// -100..100 config -> -1..1, clamped so a hand-edited settings.json
	// cannot push the lerp into wild extrapolation
	double vib = config.vibrance / 100.0;
	if(vib < -1.0){ vib = -1.0; }
	if(vib > 1.0){ vib = 1.0; }
	constants.vibrance = (float)vib;
	constants.applyColor = settings.applyColor ? 1.0f : 0.0f;
	// contrast precomputed like the compositor shader: col * mult + offset
	double contrastMult = config.contrast / 50.0;
	double contrastMid = config.contrastMidpoint / 100.0;
	constants.contrastMult = (float)contrastMult;
	constants.contrastOffset = (float)(-contrastMid * contrastMult + contrastMid);
	constants.contrastLinear = config.contrastLinear ? 1.0f : 0.0f;
	constants.outGamma = (float)config.gamma;
	// per-eye override resolved here, so the shader always sees one value
	double casStrengthEye = config.cas.perEye
		? (eye == 0 ? config.cas.strengthLeft : config.cas.strengthRight)
		: config.cas.strength;
	constants.casStrength = (float)casStrengthEye;
	constants.casEnable = config.cas.enable ? 1.0f : 0.0f;
	// fxaa mode resolve: quality (2) runs the separate pre-pass when its
	// shader and intermediate exist; otherwise degrade to the in-pass
	// fast path (missing/stale fxaa shader file, fx alloc failure, or the
	// v3 tier which has no scratch by design) so the feature never
	// silently disappears — the one-shot line names the reason
	bool fxaaQualityActive = false;
	if(config.fxaaMode == 2){
		if(!v3Active && fxaaShader && scratchFxSRV && scratchFxRTV){
			fxaaQualityActive = true;
		}else if(fxaaFallbackLogged.insert(texture).second){
			DriverLog("FrameProcessor: fxaa quality unavailable for this layer (%s), using fast path",
				v3Active ? "zero-copy v3 tier" : (fxaaShader ? "no fx intermediate" : "pass shader not compiled"));
		}
	}
	constants.fxaaEnable = (config.fxaaMode == 1 || (config.fxaaMode == 2 && !fxaaQualityActive)) ? 1.0f : 0.0f;
	constants.annulusEnable = config.distortion.annulus.enable ? 1.0f : 0.0f;
	constants.annulusMin = (float)config.distortion.annulus.rMin;
	constants.annulusMax = (float)config.distortion.annulus.rMax;
	constants.annulusFeather = (float)config.distortion.annulus.feather;
	constants.ditherEnable = config.dither ? 1.0f : 0.0f;
	constants.bfRampBar = config.blackFloor.rampBar ? 1.0f : 0.0f;
	constants.bfRangeMode = (float)config.blackFloor.rangeMode;
	constants.bfShadowFloor = config.blackFloor.shadowLift ? (float)config.blackFloor.floorCode : -1.0f;
	constants.bfKnee = (float)config.blackFloor.kneeCode;
	constants.bfBlackPoint = (float)config.blackFloor.blackPointCode;
	constants.lutMaxR = lutMaxRadius;
	// row order is eye major, axis minor
	int axisCount = config.distortion.perAxis ? 2 : 1;
	int activeSegs = EffectiveSegments(config);
	int rowMinor = activeSegs > 1 ? activeSegs : axisCount;
	constants.lutRowBase = config.distortion.perEye ? (float)(eye * rowMinor) : 0.0f;
	constants.lutRowCountF = (float)lutRowCount;
	constants.perAxisEnable = (config.distortion.perAxis && activeSegs == 1) ? 1.0f : 0.0f;
	constants.segCount = activeSegs > 1 ? (float)activeSegs : 0.0f;
	constants.dimAmount = (float)settings.dimAmount;
	constants.manualSrgb = manualSrgb ? 1.0f : 0.0f;
	constants.ditherLsb = ditherLsb;
	// gaze -> viewport mapping for this eye (debug ring now; the dynamic
	// pupil swim pass will reuse the same mapping). the published gaze is a
	// unit direction in HMD space; project through an assumed symmetric
	// frustum (tan half angles from config, tunable live until the ring
	// tracks the eye) around the configured distortion center.
	constants.gazeRing = 0.0f;
	// shared gaze -> viewport uv for this eye (ring and pupil swim)
	bool haveGazeUv = false;
	double gazeUvU = 0.5, gazeUvV = 0.5;
	if(settings.gazeValid){
		// mapping shared with the fixation dot and the swim probe (the
		// same math the runtime uses for GetEyeTrackedFoveationCenter)
		double u, v;
		if(MapHeadDirToEyeUv(settings, eye, settings.gazeDirX, settings.gazeDirY, settings.gazeDirZ, u, v)){
			haveGazeUv = true;
			gazeUvU = u;
			gazeUvV = v;
			if(settings.config.eyeGaze.debugRing){
				constants.gazeU = (float)u;
				constants.gazeV = (float)v;
				constants.gazeRing = 1.0f;
			}
		}
	}
	// world-locked fixation dot (VOR swim probe target), mapped per eye
	// through the same frusta as the gaze. probeCapture implies warped
	// overlays (a scoring run against an unwarped dot would be meaningless)
	// and the dot itself.
	constants.overlayWarped = (settings.config.eyeGaze.overlayWarped
		|| settings.config.eyeGaze.probeCapture) ? 1.0f : 0.0f;
	constants.dotMode = 0.0f;
	if(settings.dotValid && (settings.config.eyeGaze.calibDot || settings.config.eyeGaze.probeCapture)){
		double du, dv;
		if(MapHeadDirToEyeUv(settings, eye, settings.dotDirX, settings.dotDirY, settings.dotDirZ, du, dv)){
			constants.dotU = (float)du;
			constants.dotV = (float)dv;
			constants.dotMode = 1.0f;
		}
	}
	// auxiliary calibration markers reuse the dot constants when the probe
	// dot is not active. mode 2: cross at this eye's configured distortion
	// center. mode 3: controller tip marker with a fixed 63mm ipd parallax
	// (millimeter-level ipd error is irrelevant for the freeze judgment,
	// which is differential).
	if(constants.dotMode < 0.5 && settings.auxMarkerMode == 2){
		double cx = eye == 0 ? settings.config.centerOffsetXLeft : settings.config.centerOffsetXRight;
		constants.dotU = (float)(0.5 + cx);
		constants.dotV = (float)(0.5 + settings.config.centerOffsetY);
		constants.dotMode = 2.0f;
	}else if(constants.dotMode < 0.5 && settings.auxMarkerMode == 3){
		double eyeOffsetX = (eye == 0 ? -1.0 : 1.0) * 0.0315;
		double dirX = settings.auxHeadX - eyeOffsetX;
		double dirY = settings.auxHeadY;
		double dirZ = settings.auxHeadZ;
		double u, v;
		if(dirZ < -0.02 && MapHeadDirToEyeUv(settings, eye, dirX, dirY, dirZ, u, v)){
			constants.dotU = (float)u;
			constants.dotV = (float)v;
			constants.dotMode = 3.0f;
		}
	}
	// interactive tuner band ring, gated per eye by the edit mode so the
	// active eye selection is visible at a glance (linked = both rings)
	constants.tuneRingR = (float)settings.tuneRingR;
	bool ringThisEye = settings.tuneActive && (settings.tuneEyeMode == 0
		|| (settings.tuneEyeMode == 1 && eye == 0)
		|| (settings.tuneEyeMode == 2 && eye == 1));
	constants.tuneRingMode = ringThisEye ? 1.0f : 0.0f;
	constants.tuneSegIdx = (float)settings.tuneSegIndex;
	constants.tuneSegCount = (float)settings.tuneSegCount;
	double ringOpacity = settings.config.distortion.tune.ringOpacity;
	if(ringOpacity < 0.0){ ringOpacity = 0.0; }
	if(ringOpacity > 1.0){ ringOpacity = 1.0; }
	constants.tuneRingAlpha = (float)ringOpacity;
	// world-locked calibration grid: head basis + flag (angular mode only,
	// needs a valid submitted render pose)
	constants.gridWorldLock = (settings.config.eyeGaze.gridWorldLocked && settings.headBasisValid) ? 1.0f : 0.0f;
	constants.headXx = settings.headBasis[0][0]; constants.headXy = settings.headBasis[0][1]; constants.headXz = settings.headBasis[0][2];
	constants.headYx = settings.headBasis[1][0]; constants.headYy = settings.headBasis[1][1]; constants.headYz = settings.headBasis[1][2];
	constants.headZx = settings.headBasis[2][0]; constants.headZy = settings.headBasis[2][1]; constants.headZz = settings.headBasis[2][2];
	// grid: 0 off, 1 uv mode, 2 angular mode, 3 sboys camera pattern
	// (angular + sboys need real frusta; fall back to uv without them)
	float gridMode = 0.0f;
	if(settings.config.eyeGaze.debugGrid){
		if(settings.config.eyeGaze.gridMode == "sboys" && settings.gazeProjValid){
			gridMode = 3.0f;
		}else{
			gridMode = (settings.config.eyeGaze.gridMode == "angular" && settings.gazeProjValid) ? 2.0f : 1.0f;
		}
	}
	constants.pad2 = gridMode;
	constants.gridOpaque = settings.config.eyeGaze.gridOpaque ? 1.0f : 0.0f;
	if(settings.gazeProjValid){
		constants.projL = settings.gazeProj[eye][0];
		constants.projR = settings.gazeProj[eye][1];
		constants.projT = settings.gazeProj[eye][2];
		constants.projB = settings.gazeProj[eye][3];
	}
	double spacingDeg = settings.config.eyeGaze.gridAngularDeg;
	if(spacingDeg < 0.5){ spacingDeg = 0.5; }
	if(spacingDeg > 30.0){ spacingDeg = 30.0; }
	constants.gridSpacingRad = (float)(spacingDeg * 3.14159265358979 / 180.0);
	// ---- camera calibration support ----
	const StreamFrameCalibConfig &calib = settings.config.calib;
	bool calibEyeActive = calib.eye < 0 || calib.eye == eye;
	// capture mode preset: sboys hue grid in content space over a
	// desaturated, dimmed scene, on the calibration eye only (the other
	// eye keeps its normal grid setting so nothing surprising happens
	// there)
	constants.gridDesat = 0.0f;
	constants.captureOtherBlack = 0.0f;
	if(calib.captureMode && calibEyeActive && settings.gazeProjValid){
		constants.pad2 = 3.0f;
		constants.overlayWarped = 1.0f;
		constants.gridDesat = 1.0f;
	}else if(calib.captureMode && !calibEyeActive){
		// the non-calibration eye is black in capture mode (eye by eye
		// workflow, nothing leaks into the camera, panel spared)
		constants.captureOtherBlack = 1.0f;
	}
	{
		// pattern/grid white level: sRGB code fraction -> linear, so the
		// encoded 8 bit value is the intended code
		double c = calib.patternBrightness;
		double lin = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
		// the sweep pattern is exact; the visual grid also follows the
		// general brightness so one knob dims everything the eye sees
		double b = settings.config.brightness;
		if(b < 0.0){ b = 0.0; }
		if(b > 4.0){ b = 4.0; }
		constants.gridLevel = (float)(lin * b);
		constants.calibLevel = (float)lin;
	}
	constants.calibBits = (float)calib.patternBits;
	constants.calibEyeActive = calibEyeActive ? 1.0f : 0.0f;
	// a pattern index past the sequence end shows black (safe default for
	// an off-by-one in a tool)
	int patternCount = 2 + 4 * calib.patternBits;
	constants.calibPattern = (calib.pattern >= 0 && calib.pattern < patternCount) ? (float)calib.pattern : (calib.pattern >= 0 ? 0.0f : -1.0f);
	constants.blackout = calib.blackout ? 1.0f : 0.0f;
	{
		double b = settings.config.brightness;
		if(b < 0.0){ b = 0.0; }
		if(b > 4.0){ b = 4.0; }
		constants.brightness = (float)b;
	}
	constants.dispEnable = dispActive ? 1.0f : 0.0f;
	constants.dispEye = (float)eye;
	// state logging so "ring configured but not visible" is attributable
	// from the log alone: config parsed? gaze valid? mapping in range?
	if(eye == 0 && settings.config.eyeGaze.debugRing){
		uint64_t nowMs = NowMs();
		bool active = constants.gazeRing > 0.5f;
		if(active != gazeRingWasActive || (nowMs - lastGazeRingLogMs > 5000)){
			gazeRingWasActive = active;
			lastGazeRingLogMs = nowMs;
			DriverLog("FrameProcessor: gaze ring %s (gazeValid=%d dir=(%.3f, %.3f, %.3f) u=%.3f v=%.3f mapping=%s)",
				active ? "ACTIVE" : "requested but INACTIVE",
				(int)settings.gazeValid, settings.gazeDirX, settings.gazeDirY, settings.gazeDirZ,
				constants.gazeU, constants.gazeV,
				settings.gazeProjValid ? "projectionRaw" : "tangent knobs");
		}
	}
	double centerOffsetX = eye == 0 ? config.centerOffsetXLeft : config.centerOffsetXRight;
	double centerU = 0.5 + centerOffsetX;
	double centerV = 0.5 + config.centerOffsetY;
	// pupil swim phase A: the effective optical center follows the gaze.
	// the shift is a fraction of the gaze offset from the static center,
	// so at center gaze this reduces exactly to the static calibration.
	if(haveGazeUv && (config.pupilSwim.centerStrengthX != 0 || config.pupilSwim.centerStrengthY != 0)){
		double shiftU = config.pupilSwim.centerStrengthX * (gazeUvU - centerU);
		double shiftV = config.pupilSwim.centerStrengthY * (gazeUvV - centerV);
		if(shiftU > 0.15){ shiftU = 0.15; } if(shiftU < -0.15){ shiftU = -0.15; }
		if(shiftV > 0.15){ shiftV = 0.15; } if(shiftV < -0.15){ shiftV = -0.15; }
		centerU += shiftU;
		centerV += shiftV;
	}
	constants.center[0] = (float)centerU;
	constants.center[1] = (float)centerV;
	// alignment shift: config h is "image moves right", v is "image moves
	// up". sampling offset is the negation of the image motion (content
	// appears moved by minus the sampling displacement; v axis is down).
	{
		double alignH = eye == 0 ? config.alignment.leftH : config.alignment.rightH;
		double alignV = eye == 0 ? config.alignment.leftV : config.alignment.rightV;
		if(alignH > 0.05){ alignH = 0.05; } if(alignH < -0.05){ alignH = -0.05; }
		if(alignV > 0.05){ alignV = 0.05; } if(alignV < -0.05){ alignV = -0.05; }
		constants.alignShiftU = (float)(-alignH);
		constants.alignShiftV = (float)(alignV);
	}
	float uMin = (float)bounds.uMin, vMin = (float)bounds.vMin;
	float uSize = (float)(bounds.uMax - bounds.uMin), vSize = (float)(bounds.vMax - bounds.vMin);
	if(uSize == 0){ uSize = 1; }
	if(vSize == 0){ vSize = 1; }
	constants.boundsMin[0] = uMin;
	constants.boundsMin[1] = vMin;
	constants.boundsSize[0] = uSize;
	constants.boundsSize[1] = vSize;
	constants.texelSize[0] = 1.0f / desc.Width;
	constants.texelSize[1] = 1.0f / desc.Height;
	constants.aspect = desc.Width != 0 ? (float)((double)desc.Height * vSize / ((double)desc.Width * uSize)) : 1.0f;
	constants.colorMultiplier[0] = (float)config.colorMultiplier.r;
	constants.colorMultiplier[1] = (float)config.colorMultiplier.g;
	constants.colorMultiplier[2] = (float)config.colorMultiplier.b;
	constants.colorMultiplier[3] = 1.0f;
	if(config.srgbMatrix.size() == 9){
		constants.matrixEnable = 1.0f;
		for(int i = 0; i < 3; i++){
			constants.matR[i] = (float)config.srgbMatrix[i];
			constants.matG[i] = (float)config.srgbMatrix[3 + i];
			constants.matB[i] = (float)config.srgbMatrix[6 + i];
		}
	}else{
		constants.matrixEnable = 0.0f;
		constants.matR[0] = 1.0f;
		constants.matG[1] = 1.0f;
		constants.matB[2] = 1.0f;
	}
	memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(constantBuffer, 0);

	// draw a fullscreen triangle into the bounds region of the scratch output.
	// with full 0-1 bounds this is the whole texture; with a shared side by side
	// texture each eye only touches its own half.
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = uMin * desc.Width;
	viewport.TopLeftY = vMin * desc.Height;
	viewport.Width = uSize * desc.Width;
	viewport.Height = vSize * desc.Height;
	viewport.MaxDepth = 1.0f;
	if(fxaaQualityActive){
		// pass 1: FXAA in unwarped source space into the fx intermediate,
		// same bounds region and texel grid as the source — the main pass
		// below then samples fully AA-resolved neighborhoods
		context->ClearState();
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertexShader, nullptr, 0);
		context->PSSetShader(fxaaShader, nullptr, 0);
		ID3D11ShaderResourceView* fxSrvs[1] = { scratchInSRV };
		context->PSSetShaderResources(0, 1, fxSrvs);
		context->PSSetSamplers(0, 1, &sampler);
		context->PSSetConstantBuffers(0, 1, &constantBuffer);
		context->RSSetViewports(1, &viewport);
		context->OMSetRenderTargets(1, &scratchFxRTV, nullptr);
		context->Draw(3, 0);
	}
	context->ClearState();
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vertexShader, nullptr, 0);
	context->PSSetShader(pixelShader, nullptr, 0);
	ID3D11ShaderResourceView* srvs[3] = { fxaaQualityActive ? scratchFxSRV : (v3Active ? layerSRV : scratchInSRV), lutSRV, dispActive ? dispSRV : nullptr };
	context->PSSetShaderResources(0, 3, srvs);
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetConstantBuffers(0, 1, &constantBuffer);
	context->RSSetViewports(1, &viewport);
	ID3D11RenderTargetView* target = v3Active ? shadowRTV : (directRTV ? directRTV : scratchOutRTV);
	context->OMSetRenderTargets(1, &target, nullptr);
	context->Draw(3, 0);
	context->ClearState();

	if(!directRTV && !v3Active){
		// fallback path: copy only the bounds region back into the layer
		D3D11_BOX box = {};
		box.left = (UINT)(uMin * desc.Width);
		box.top = (UINT)(vMin * desc.Height);
		box.right = (UINT)((uMin + uSize) * desc.Width);
		box.bottom = (UINT)((vMin + vSize) * desc.Height);
		box.back = 1;
		context->CopySubresourceRegion(texture, layerSub, box.left, box.top, 0, scratchOut, 0, &box);
	}
	return true;
}

bool FrameProcessor::ProcessSceneLayer(vr::SharedTextureHandle_t leftEye, vr::SharedTextureHandle_t rightEye,
	const vr::VRTextureBounds_t &leftBounds, const vr::VRTextureBounds_t &rightBounds,
	vr::SharedTextureHandle_t syncTexture, const FrameProcessSettings &settings){
	std::lock_guard<std::mutex> guard(lock);

	// zero-copy v3 arming follows the live-reloaded flag; frameCounter
	// drives the shadow slot rotation (one advance per scene frame)
	{
		static bool prevArmed = false;
		bool nowArmed = settings.config.zeroCopyV3;
		if(nowArmed && !prevArmed){
			// fresh observation phase: un-suppress the recon copy lines so
			// the post-arming copies are visible in the log
			ReconLogger::Get().ResetSuppression();
		}
		prevArmed = nowArmed;
	}
	ZeroCopyV3::Get().SetArmed(settings.config.zeroCopyV3);
	ZeroCopyV3::Get().MaybeHeartbeat();
	{
		// v3: the encoder settings are global; the tier (or custom mode)
		// only supplies tile width + bandwidth. nvencTap is the master
		// switch: off = stock streamer.
		NvencTapConfig tc;
		tc.enabled = settings.config.nvencTap;
		tc.fixLevel = settings.config.nvencFixLevel;
		// encoder bitrate: the debug "separate" value if set, else the
		// effective pacer bandwidth (tier/custom/Advanced override)
		tc.bitrateMbit = settings.config.nvencBitrateMbit > 0 ? settings.config.nvencBitrateMbit : GalaxyXR_EffectiveBandwidthMbit();
		tc.maxQp = settings.config.nvencMaxQp;
		tc.aqStrength = settings.config.nvencAqStrength;
		tc.maxBitrateHeadroomPct = settings.config.nvencMaxBitrateHeadroomPct;
		tc.vbvFrames = settings.config.nvencVbvFrames;
		tc.forceFps = settings.config.nvencForceFps;
		tc.bitrateScale = settings.config.nvencBitrateScale;
		tc.vrlinkClampMbit = settings.config.nvencVrlinkClampMbit;
		tc.preset = settings.config.nvencPreset;
		tc.presetMerge = settings.config.nvencPresetMerge;
		// the post-pack limited-range remap needs the VUI full-range flag
		// cleared so the decoder expands 16..235 back; it overrides the
		// (graveyard) manual VUI value
		tc.vuiFullRange = settings.config.postPack.enable && settings.config.postPack.limitedRange ? 0 : settings.config.nvencVuiFullRange;
		tc.vuiMatrix = settings.config.nvencVuiMatrix;
		tc.vuiPrimaries = settings.config.nvencVuiPrimaries;
		tc.vuiTransfer = settings.config.nvencVuiTransfer;
		tc.minQp = settings.config.nvencMinQp;
		tc.minQpIntra = settings.config.nvencMinQpIntra;
		tc.forceCbr = settings.config.nvencForceCbr;
		tc.lowDelayKfScale = settings.config.nvencLowDelayKfScale;
		tc.presetAuto = settings.config.nvencPreset == 0; // 0 = by engine count (NvencTap)
		{
			const auto &pp = settings.config.postPack;
			NvencPostPackConfig pc;
			pc.enable = tc.enabled && pp.enable; pc.casEnable = pp.casEnable;
			pc.foveaStrength = (float)pp.foveaStrength; pc.peripheryStrength = (float)pp.peripheryStrength;
			pc.foveaTop = pp.foveaTop; pc.limitedRange = pp.limitedRange; pc.edgeFalloff = (float)pp.edgeFalloff;
			NvencPostPack::SetConfig(pc);
		}
		tc.splitMode = settings.config.nvencSplitMode;
		tc.qpFovea = (std::max)(-10, (std::min)(0, settings.config.nvencQpFovea));
		tc.qpPeriphery = (std::max)(0, (std::min)(10, settings.config.nvencQpPeriphery));
		tc.qpEdgeFalloff = (float)settings.config.postPack.edgeFalloff;
		tc.qpFoveaTop = settings.config.postPack.foveaTop;
		tc.verbose = settings.config.nvencVerbose;
		NvencTap::Get().SetConfig(tc);
		if(tc.enabled){
			NvencTap::Get().TryInstall();
			NvencTap::Get().MaybeHeartbeat();
		}
	}
	frameCounter++;

	// re-arm the diagnostic budget every 5 minutes so problems in apps
	// launched later in the session are not silenced by earlier errors
	uint64_t nowMs = NowMs();
	if(nowMs - lastErrorResetMs > 300000){
		lastErrorResetMs = nowMs;
		errorCount = 0;
		hdHitchLines = 0;
	}

	// ---- HITCHDIAG frame-start accounting. the gap between successive
	// entries here is the cadence the user feels; an outlier gap is
	// attributed to what the PREVIOUS frame did (its tags/acquire/work),
	// because that is the frame whose cost shows up as this gap.
	cfgDeferEvict = settings.config.deferredEviction;
	cfgFxaaQuality = settings.config.fxaaMode == 2;
	uint64_t tFrameUs = NowUs();
	double gapMs = hdLastFrameStartUs ? (tFrameUs - hdLastFrameStartUs) / 1000.0 : 0.0;
	hdLastFrameStartUs = tFrameUs;
	if(hdWindowStartUs == 0){
		hdWindowStartUs = tFrameUs;
	}
	if(gapMs > 1000.0){
		// standby / disconnect / first frame of a new app: a session
		// boundary, not a hitch - keep it out of the stats
		hdIdleBreaks++;
	}else if(gapMs > 0.0){
		hdFrames++;
		hdGapSumMs += gapMs;
		if(gapMs > hdGapMaxMs){ hdGapMaxMs = gapMs; }
		if(gapMs > 16.7){ hdOver16++; }
		if(gapMs > 33.4){ hdOver33++; }
		if(settings.config.hitchDiag && gapMs > 25.0 && hdHitchLines < 30){
			hdHitchLines++;
			DriverLog("FrameProcessor: HITCH gap=%.1fms prevAcq=%.2fms prevWork=%.2fms tags=%s%s%s%s%s",
				gapMs, hdPrevAcqMs, hdPrevWorkMs,
				hdPrevTags == 0 ? "none" : "",
				(hdPrevTags & TagScratchCreate) ? "scratchCreate " : "",
				(hdPrevTags & TagLutBake) ? "lutBake " : "",
				(hdPrevTags & TagShaderCompile) ? "shaderCompile " : "",
				(hdPrevTags & TagSyncSkip) ? "syncSkip" : "");
		}
	}
	// 2s summary window, the render-side KALDIAG
	if(tFrameUs - hdWindowStartUs >= 2000000){
		if(settings.config.hitchDiag && hdFrames > 0){
			DriverLog("FrameProcessor: HITCHDIAG frames=%u dtMean=%.2fms dtMax=%.1fms over16=%u over33=%u acqMean=%.2fms acqMax=%.1fms workMean=%.2fms workMax=%.1fms skips=%u creates=%u evicts=%u pend=%zu idle=%u",
				hdFrames, hdGapSumMs / hdFrames, hdGapMaxMs, hdOver16, hdOver33,
				hdAcqSumMs / hdFrames, hdAcqMaxMs, hdWorkSumMs / hdFrames, hdWorkMaxMs,
				hdSkips, hdCreates, hdEvicts, pendingEvictions.size(), hdIdleBreaks);
		}
		hdWindowStartUs = tFrameUs;
		hdFrames = 0;
		hdGapSumMs = 0; hdGapMaxMs = 0;
		hdAcqSumMs = 0; hdAcqMaxMs = 0;
		hdWorkSumMs = 0; hdWorkMaxMs = 0;
		hdOver16 = 0; hdOver33 = 0; hdSkips = 0;
		hdCreates = 0; hdEvicts = 0; hdIdleBreaks = 0;
	}
	hdFrameTags = 0;

	if(!EnsureDevice() || !EnsureShaders()){
		return false;
	}
	if(!BakeLutIfNeeded(settings.config)){
		return false;
	}
	// displacement map bake failure is not fatal: dispActive stays false
	// and the radial path keeps running
	BakeMapIfNeeded(settings.config);

	// acquire the frame via the sync texture keyed mutex. this guarantees the
	// app has finished rendering the layer textures, and our release followed by
	// vrlink's own acquire orders its reads after our writes.
	ID3D11Texture2D* sync = OpenShared(syncTexture);
	if(!sync){
		return false;
	}
	IDXGIKeyedMutex* mutex = nullptr;
	if(FAILED(sync->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&mutex)) || !mutex){
		PROCESSOR_ERROR("FrameProcessor: sync texture has no keyed mutex, skipping processing");
		return false;
	}
	// base timeout from config; after a skipped frame escalate so a transient
	// long hold costs one flash instead of a visible streak of them. clamped
	// to keep a misconfigured value from stalling the pipeline.
	int baseTimeout = settings.config.syncTimeoutMs;
	if(baseTimeout < 1){ baseTimeout = 1; }
	if(baseTimeout > 100){ baseTimeout = 100; }
	uint32_t timeout = (uint32_t)baseTimeout;
	if(consecutiveSyncSkips > 0){
		uint32_t escalated = (uint32_t)baseTimeout * 3;
		timeout = escalated < 15 ? 15 : escalated;
	}
	uint64_t tAcqUs = NowUs();
	HRESULT hr = mutex->AcquireSync(0, timeout);
	double acqMs = (NowUs() - tAcqUs) / 1000.0;
	hdAcqSumMs += acqMs;
	if(acqMs > hdAcqMaxMs){ hdAcqMaxMs = acqMs; }
	if(hr != S_OK){
		// timeout or abandoned: skip this frame rather than stall the pipeline
		consecutiveSyncSkips++;
		mutex->Release();
		hdSkips++;
		hdFrameTags |= TagSyncSkip;
		hdPrevTags = hdFrameTags;
		hdPrevAcqMs = acqMs;
		hdPrevWorkMs = (NowUs() - tFrameUs) / 1000.0;
		PROCESSOR_ERROR("FrameProcessor: AcquireSync returned 0x%08X, skipping frame (%d consecutive)", (unsigned)hr, consecutiveSyncSkips);
		return false;
	}
	if(consecutiveSyncSkips > 0){
		DriverLog("FrameProcessor: sync recovered after %d skipped frames", consecutiveSyncSkips);
		consecutiveSyncSkips = 0;
	}

	bool ok = true;
	ID3D11Texture2D* left = OpenShared(leftEye);
	ID3D11Texture2D* right = OpenShared(rightEye);
	// single-pass-instanced apps (Demeo, Beat Saber, ...) submit ONE
	// Texture2DArray shared by both eyes: slice 0 = left, slice 1 = right.
	// detect it from the opened texture and route each eye to its slice.
	UINT leftSlices = 1;
	if(left){
		D3D11_TEXTURE2D_DESC leftDesc = {};
		left->GetDesc(&leftDesc);
		leftSlices = leftDesc.ArraySize;
	}
	if(left && leftSlices >= 2 && (right == left || !right)){
		if(loggedArrayTextures.insert((uint64_t)leftEye).second){
			DriverLog("FrameProcessor: layer is a texture array (%u slices), processing per-slice (single-pass instanced app)", leftSlices);
		}
		ok &= ProcessEye(left, leftBounds, 0, 0, settings);
		ok &= ProcessEye(left, rightBounds, 1, 1, settings);
	}else{
		if(left){
			ok &= ProcessEye(left, leftBounds, 0, 0, settings);
		}
		bool boundsDiffer = memcmp(&leftBounds, &rightBounds, sizeof(vr::VRTextureBounds_t)) != 0;
		if(right && (right != left || boundsDiffer)){
			ok &= ProcessEye(right, rightBounds, 1, 0, settings);
		}
	}

	// submit our commands to the GPU before releasing the mutex. without this the
	// commands sit in the immediate context (this device never presents, so it
	// almost never kicks on its own), ReleaseSync signals before any of our work,
	// and the driver encodes the untouched texture.
	context->Flush();
	// publish this frame's shadows only AFTER the producing Flush, so the
	// redirect can never hand vrlink an unflushed image
	for(auto &pair : shadowSets){
		if(pair.second.usedThisFrame){
			pair.second.usedThisFrame = false;
			ZeroCopyV3::Get().MarkFresh((uint32_t)(pair.first >> 32), (uint32_t)(pair.first & 0xFFFFFFFFu), pair.second.index);
		}
	}

	mutex->ReleaseSync(0);
	mutex->Release();

	// ---- calibration handshake: count consecutive frames the current
	// pattern index has been drawn and publish it (with the projection
	// tangents and texture geometry the tools need) into diagnostic.json
	// via the 4 Hz diagnostic writer. cheap: a few field writes per frame.
	{
		int shown = settings.config.calib.pattern;
		if(shown < -1){ shown = -1; }
		if(shown == calibPatternShown){
			if(calibPatternFrames < 0xFFFFFFF0u){ calibPatternFrames++; }
		}else{
			calibPatternShown = shown;
			calibPatternFrames = 1;
		}
		ConfigLoader::DiagnosticInfo &diag = driverConfigLoader.diagnosticInfo;
		diag.streamFrameActive = true;
		diag.streamFrameCounter = frameCounter;
		diag.calibPatternShown = calibPatternShown;
		diag.calibPatternFrames = calibPatternFrames;
		diag.calibBlackout = settings.config.calib.blackout;
		diag.projValid = settings.gazeProjValid;
		if(settings.gazeProjValid){
			for(int e = 0; e < 2; e++){
				for(int i = 0; i < 4; i++){
					diag.proj[e][i] = settings.gazeProj[e][i];
				}
			}
		}
		if(left){
			D3D11_TEXTURE2D_DESC ld = {};
			left->GetDesc(&ld);
			double uSize = leftBounds.uMax - leftBounds.uMin, vSize = leftBounds.vMax - leftBounds.vMin;
			if(uSize <= 0){ uSize = 1; }
			if(vSize <= 0){ vSize = 1; }
			diag.eyeTexWidth = (uint32_t)(ld.Width * uSize + 0.5);
			diag.eyeTexHeight = (uint32_t)(ld.Height * vSize + 0.5);
			diag.eyeAspect = ld.Width ? (float)((double)ld.Height * vSize / ((double)ld.Width * uSize)) : 1.0f;
		}
		diag.mapCols = settings.config.distortion.map.cols;
		diag.mapRows = settings.config.distortion.map.rows;
		diag.mapActive = dispActive;
	}

	// deferred scratch eviction drain: the mutex is released, vrlink can
	// consume the frame - release cost here is invisible to the pipeline
	DrainPendingEvictions(false);

	double workMs = (NowUs() - tFrameUs) / 1000.0;
	hdWorkSumMs += workMs;
	if(workMs > hdWorkMaxMs){ hdWorkMaxMs = workMs; }
	hdPrevTags = hdFrameTags;
	hdPrevAcqMs = acqMs;
	hdPrevWorkMs = workMs;
	return ok;
}

#endif // _WIN32
