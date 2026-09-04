#include "NvencPostPack.h"
#include "DriverLog.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "D3DCompiler.lib")

namespace {

std::mutex lock;
NvencPostPackConfig cfg;
NvencPostPackStats stats;
std::atomic<int> disabledLogged{0};

// per-device resources (vrlink uses one device; rebuilt if it changes)
ID3D11Device* device = nullptr;
ID3D11DeviceContext* ctx = nullptr;   // vrlink's immediate context (execute only)
ID3D11DeviceContext* dctx = nullptr;  // our deferred context: all state changes happen here
ID3D11Multithread* mt = nullptr;
ID3D11VertexShader* vs = nullptr;
ID3D11PixelShader* psLuma = nullptr;
ID3D11PixelShader* psChroma = nullptr;
ID3D11Buffer* cb = nullptr;
ID3D11RasterizerState* rs = nullptr;
ID3D11DepthStencilState* dss = nullptr;
ID3D11BlendState* bs = nullptr;
// per-source-texture cache (vrlink registers a ring of ~10 input textures)
struct TexCache {
	ID3D11Texture2D* src = nullptr;   // vrlink's texture (not owned)
	ID3D11Texture2D* scratch = nullptr;
	ID3D11ShaderResourceView* srvY = nullptr;
	ID3D11ShaderResourceView* srvC = nullptr;
	ID3D11RenderTargetView* rtvY = nullptr;
	ID3D11RenderTargetView* rtvC = nullptr;
	UINT w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
TexCache cache[16];
int cacheCount = 0;

struct alignas(16) Constants {
	float texW, texH, tileH, foveaStrength;
	float peripheryStrength, foveaTop, limitedRange, chromaW;
	float chromaH, edgeFalloff, pad1, pad2;
};

const char* kVs = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID){
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
})";

// luma: CAS (4-tap contrast adaptive sharpening, AMD FidelityFX formulation)
// with the strength chosen by tile, then the optional limited-range remap.
const char* kPsLuma = R"(
Texture2D<float> texY : register(t0);
cbuffer C : register(b0){
	float texW, texH, tileH, foveaStrength;
	float peripheryStrength, foveaTop, limitedRange, chromaW;
	float chromaH, edgeFalloff, pad1, pad2;
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float main(VSOut i) : SV_TARGET {
	int2 p = int2(i.pos.xy);
	int2 mx = int2(texW - 1, texH - 1);
	float e = texY.Load(int3(p, 0));
	int tile = (int)floor(i.pos.y / tileH);
	bool fovea = ((tile & 1) == 0) == (foveaTop > 0.5);
	float s = peripheryStrength;
	if(fovea){
		// ramp the fovea strength down to the periphery strength over the
		// outer edgeFalloff fraction of the tile, so the seam where the client
		// composites the cut-out over the stretched periphery is not a
		// sharpness step
		float2 inTile = float2(i.pos.x, i.pos.y - tile * tileH) / tileH; // 0..1
		float2 dEdge = min(inTile, 1.0 - inTile);
		float dist = min(dEdge.x, dEdge.y);
		float t = edgeFalloff > 0.0 ? saturate(dist / edgeFalloff) : 1.0;
		t = t * t * (3.0 - 2.0 * t);
		s = lerp(peripheryStrength, foveaStrength, t);
	}
	float y = e;
	if(s > 0.0){
		float a = texY.Load(int3(clamp(p + int2(-1, 0), int2(0,0), mx), 0));
		float b = texY.Load(int3(clamp(p + int2( 1, 0), int2(0,0), mx), 0));
		float c = texY.Load(int3(clamp(p + int2( 0,-1), int2(0,0), mx), 0));
		float d = texY.Load(int3(clamp(p + int2( 0, 1), int2(0,0), mx), 0));
		float mn = min(min(min(a, b), min(c, d)), e);
		float mxv = max(max(max(a, b), max(c, d)), e);
		float rcpM = 1.0 / max(mxv, 1e-4);
		float amp = saturate(min(mn, 1.0 - mxv) * rcpM);
		amp = sqrt(amp);
		float peak = -1.0 / lerp(8.0, 5.0, saturate(s));
		float w = amp * peak;
		y = (e + (a + b + c + d) * w) / (1.0 + 4.0 * w);
	}
	if(limitedRange > 0.5){ y = 16.0 / 255.0 + saturate(y) * (219.0 / 255.0); }
	return y;
})";

// chroma: limited-range remap only (16..240 around the midpoint)
const char* kPsChroma = R"(
Texture2D<float2> texC : register(t0);
cbuffer C : register(b0){
	float texW, texH, tileH, foveaStrength;
	float peripheryStrength, foveaTop, limitedRange, chromaW;
	float chromaH, pad0, pad1, pad2;
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float2 main(VSOut i) : SV_TARGET {
	float2 c = texC.Load(int3(int2(i.pos.xy), 0));
	return 0.5 + (c - 0.5) * (224.0 / 255.0);
})";

template<typename T> void Release(T* &p){ if(p){ p->Release(); p = nullptr; } }

void ReleaseCache(){
	for(int i = 0; i < cacheCount; i++){
		Release(cache[i].scratch); Release(cache[i].srvY); Release(cache[i].srvC); Release(cache[i].rtvY); Release(cache[i].rtvC);
		cache[i].src = nullptr;
	}
	cacheCount = 0;
}
void ReleaseDevice(){
	ReleaseCache();
	Release(vs); Release(psLuma); Release(psChroma); Release(cb); Release(rs); Release(dss); Release(bs); Release(mt); Release(dctx); Release(ctx); Release(device);
}

void Disable(const char* why, HRESULT hr){
	stats.disabled = true;
	if(disabledLogged.fetch_add(1) < 1){
		DriverLog("NvencPostPack: DISABLED for this session: %s (hr 0x%08x). the encoder sees vrlink's untouched frame.", why, (unsigned)hr);
	}
}

bool Compile(const char* src, const char* name, const char* target, ID3DBlob** out){
	ID3DBlob* err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), name, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
	if(FAILED(hr)){
		DriverLog("NvencPostPack: shader %s compile failed: %s", name, err ? (const char*)err->GetBufferPointer() : "?");
		if(err){ err->Release(); }
		return false;
	}
	if(err){ err->Release(); }
	return true;
}

bool EnsureDevice(ID3D11Texture2D* tex){
	ID3D11Device* d = nullptr;
	tex->GetDevice(&d);
	if(!d){ Disable("texture has no device", E_FAIL); return false; }
	if(device == d){ d->Release(); return true; }
	ReleaseDevice();
	device = d; // keep the reference from GetDevice
	device->GetImmediateContext(&ctx);
	// all pipeline state is set on a deferred context; the command list is
	// executed on the immediate context with RestoreContextState so vrlink's
	// render-thread state is untouched
	HRESULT dh = device->CreateDeferredContext(0, &dctx);
	if(FAILED(dh) || !dctx){ Disable("CreateDeferredContext", dh); return false; }
	// EncodePicture runs on vrlink's encode thread; the immediate context is
	// shared with its render thread. multithread protection serializes both.
	if(SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt)) && mt){
		mt->SetMultithreadProtected(TRUE);
	}else{
		DriverLog("NvencPostPack: ID3D11Multithread unavailable; proceeding without context protection");
	}
	ID3DBlob* b = nullptr;
	if(!Compile(kVs, "postpack_vs", "vs_5_0", &b)){ Disable("vs compile", E_FAIL); return false; }
	HRESULT hr = device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs); b->Release(); b = nullptr;
	if(FAILED(hr)){ Disable("CreateVertexShader", hr); return false; }
	if(!Compile(kPsLuma, "postpack_ps_luma", "ps_5_0", &b)){ Disable("ps luma compile", E_FAIL); return false; }
	hr = device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psLuma); b->Release(); b = nullptr;
	if(FAILED(hr)){ Disable("CreatePixelShader luma", hr); return false; }
	if(!Compile(kPsChroma, "postpack_ps_chroma", "ps_5_0", &b)){ Disable("ps chroma compile", E_FAIL); return false; }
	hr = device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psChroma); b->Release(); b = nullptr;
	if(FAILED(hr)){ Disable("CreatePixelShader chroma", hr); return false; }
	D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(Constants); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device->CreateBuffer(&bd, nullptr, &cb);
	if(FAILED(hr)){ Disable("CreateBuffer cb", hr); return false; }
	D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE;
	device->CreateRasterizerState(&rd, &rs);
	D3D11_DEPTH_STENCIL_DESC dd = {}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
	device->CreateDepthStencilState(&dd, &dss);
	D3D11_BLEND_DESC bld = {}; bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	device->CreateBlendState(&bld, &bs);
	DriverLog("NvencPostPack: initialised on vrlink's D3D11 device %p (multithread protection %s)", device, mt ? "on" : "off");
	return true;
}

TexCache* EnsureCache(ID3D11Texture2D* tex){
	for(int i = 0; i < cacheCount; i++){ if(cache[i].src == tex){ return &cache[i]; } }
	if(cacheCount >= 16){ ReleaseCache(); }
	D3D11_TEXTURE2D_DESC d = {}; tex->GetDesc(&d);
	DXGI_FORMAT fY, fC;
	if(d.Format == DXGI_FORMAT_NV12){ fY = DXGI_FORMAT_R8_UNORM; fC = DXGI_FORMAT_R8G8_UNORM; }
	else if(d.Format == DXGI_FORMAT_P010){ fY = DXGI_FORMAT_R16_UNORM; fC = DXGI_FORMAT_R16G16_UNORM; }
	else { Disable("packed texture is not NV12/P010", (HRESULT)d.Format); return nullptr; }
	if(!(d.BindFlags & D3D11_BIND_RENDER_TARGET)){ Disable("packed texture has no RENDER_TARGET bind", (HRESULT)d.BindFlags); return nullptr; }
	TexCache &c = cache[cacheCount];
	c = TexCache{}; c.src = tex; c.w = d.Width; c.h = d.Height; c.fmt = d.Format;
	D3D11_TEXTURE2D_DESC sd = d; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_DEFAULT; sd.CPUAccessFlags = 0;
	HRESULT hr = device->CreateTexture2D(&sd, nullptr, &c.scratch);
	if(FAILED(hr)){ Disable("CreateTexture2D scratch", hr); return nullptr; }
	D3D11_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
	sv.Format = fY; hr = device->CreateShaderResourceView(c.scratch, &sv, &c.srvY);
	if(FAILED(hr)){ Disable("CreateShaderResourceView Y", hr); return nullptr; }
	sv.Format = fC; hr = device->CreateShaderResourceView(c.scratch, &sv, &c.srvC);
	if(FAILED(hr)){ Disable("CreateShaderResourceView C", hr); return nullptr; }
	D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	rv.Format = fY; hr = device->CreateRenderTargetView(tex, &rv, &c.rtvY);
	if(FAILED(hr)){ Disable("CreateRenderTargetView Y", hr); return nullptr; }
	rv.Format = fC; hr = device->CreateRenderTargetView(tex, &rv, &c.rtvC);
	if(FAILED(hr)){ Disable("CreateRenderTargetView C", hr); return nullptr; }
	cacheCount++;
	DriverLog("NvencPostPack: packed frame %ux%u %s (tile %u, %u tiles); scratch + plane views ready", d.Width, d.Height,
		d.Format == DXGI_FORMAT_NV12 ? "NV12" : "P010", d.Width, d.Width ? d.Height / d.Width : 0);
	return &c;
}

} // namespace

void NvencPostPack::SetConfig(const NvencPostPackConfig &c){
	std::lock_guard<std::mutex> g(lock);
	cfg = c;
}

NvencPostPackStats NvencPostPack::GetStats(){ std::lock_guard<std::mutex> g(lock); return stats; }
void NvencPostPack::ResetIntervalStats(){ std::lock_guard<std::mutex> g(lock); stats.frames = 0; stats.skipped = 0; stats.sumMs = 0; stats.maxMs = 0; }

bool NvencPostPack::Process(void* texPtr, uint32_t nvencBufferFormat){
	std::lock_guard<std::mutex> g(lock);
	if(!cfg.enable || stats.disabled || !texPtr){ stats.skipped++; return false; }
	bool wantLuma = cfg.casEnable && (cfg.foveaStrength > 0 || cfg.peripheryStrength > 0);
	if(!wantLuma && !cfg.limitedRange){ stats.skipped++; return false; }
	(void)nvencBufferFormat;
	ID3D11Texture2D* tex = nullptr;
	if(FAILED(((IUnknown*)texPtr)->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex){
		Disable("input resource is not an ID3D11Texture2D", E_NOINTERFACE); return false;
	}
	auto t0 = std::chrono::steady_clock::now();
	bool ok = false;
	do {
		if(!EnsureDevice(tex)){ break; }
		TexCache* c = EnsureCache(tex);
		if(!c){ break; }
		// source copy (the plane RTV and SRV cannot alias the same texture)
		dctx->CopyResource(c->scratch, tex);
		// constants
		D3D11_MAPPED_SUBRESOURCE m = {};
		if(FAILED(dctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))){ Disable("Map cb", E_FAIL); break; }
		Constants k = {};
		k.texW = (float)c->w; k.texH = (float)c->h; k.tileH = (float)c->w;
		k.foveaStrength = (std::max)(0.f, (std::min)(1.f, cfg.foveaStrength)); k.peripheryStrength = (std::max)(0.f, (std::min)(1.f, cfg.peripheryStrength));
		if(!cfg.casEnable){ k.foveaStrength = 0; k.peripheryStrength = 0; }
		k.foveaTop = cfg.foveaTop ? 1.f : 0.f; k.limitedRange = cfg.limitedRange ? 1.f : 0.f;
		k.edgeFalloff = (std::max)(0.f, (std::min)(0.5f, cfg.edgeFalloff));
		k.chromaW = (float)(c->w / 2); k.chromaH = (float)(c->h / 2);
		memcpy(m.pData, &k, sizeof(k));
		dctx->Unmap(cb, 0);
		// common state
		dctx->IASetInputLayout(nullptr);
		dctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		dctx->VSSetShader(vs, nullptr, 0);
		dctx->PSSetConstantBuffers(0, 1, &cb);
		dctx->RSSetState(rs);
		dctx->OMSetDepthStencilState(dss, 0);
		float bf[4] = {0, 0, 0, 0}; dctx->OMSetBlendState(bs, bf, 0xFFFFFFFF);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11RenderTargetView* nullRtv = nullptr;
		// luma pass (CAS + range)
		{
			D3D11_VIEWPORT vp = {0, 0, (float)c->w, (float)c->h, 0, 1}; dctx->RSSetViewports(1, &vp);
			dctx->OMSetRenderTargets(1, &c->rtvY, nullptr);
			dctx->PSSetShader(psLuma, nullptr, 0);
			dctx->PSSetShaderResources(0, 1, &c->srvY);
			dctx->Draw(3, 0);
			dctx->PSSetShaderResources(0, 1, &nullSrv);
		}
		// chroma pass (range only)
		if(cfg.limitedRange){
			D3D11_VIEWPORT vp = {0, 0, (float)(c->w / 2), (float)(c->h / 2), 0, 1}; dctx->RSSetViewports(1, &vp);
			dctx->OMSetRenderTargets(1, &c->rtvC, nullptr);
			dctx->PSSetShader(psChroma, nullptr, 0);
			dctx->PSSetShaderResources(0, 1, &c->srvC);
			dctx->Draw(3, 0);
			dctx->PSSetShaderResources(0, 1, &nullSrv);
		}
		dctx->OMSetRenderTargets(1, &nullRtv, nullptr);
		ID3D11CommandList* cl = nullptr;
		if(FAILED(dctx->FinishCommandList(FALSE, &cl)) || !cl){ Disable("FinishCommandList", E_FAIL); break; }
		ctx->ExecuteCommandList(cl, TRUE); // TRUE: save + restore vrlink's context state
		cl->Release();
		ok = true;
	} while(false);
	tex->Release();
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if(ok){ stats.frames++; stats.lastMs = ms; stats.sumMs += ms; if(ms > stats.maxMs){ stats.maxMs = ms; } }
	else { stats.skipped++; }
	return ok;
}
