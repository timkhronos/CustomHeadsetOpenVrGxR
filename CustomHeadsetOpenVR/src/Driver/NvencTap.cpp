#define NOMINMAX
#include "NvencTap.h"
#include "NvencPostPack.h"
#include "DriverLog.h"
#include "../../../ThirdParty/minhook/include/MinHook.h"
#include "../../../ThirdParty/nvenc/nvEncodeAPI.h"
#include "../Config/Config.h"
#include <chrono>
#include <set>
#include <map>
#include <cstdio>
#include <cstring>
#include <algorithm>

static double NowSecondsNv(){
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
}

// shared state between the shims and the public object
struct NvencTapShims {
	static NVENCSTATUS NVENCAPI CreateInstance(NV_ENCODE_API_FUNCTION_LIST* list);
	static NVENCSTATUS NVENCAPI InitializeEncoder(void* encoder, NV_ENC_INITIALIZE_PARAMS* params);
	static NVENCSTATUS NVENCAPI ReconfigureEncoder(void* encoder, NV_ENC_RECONFIGURE_PARAMS* params);
	static NVENCSTATUS NVENCAPI RegisterResource(void* encoder, NV_ENC_REGISTER_RESOURCE* params);
	static NVENCSTATUS NVENCAPI EncodePicture(void* encoder, NV_ENC_PIC_PARAMS* params);
	static NVENCSTATUS NVENCAPI LockBitstream(void* encoder, NV_ENC_LOCK_BITSTREAM* params);
	// session upgrade (11.1 -> 12.1) re-tagging shims
	static NVENCSTATUS NVENCAPI OpenEncodeSessionEx(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* params, void** encoder);
	static NVENCSTATUS NVENCAPI DestroyEncoder(void* encoder);
	static NVENCSTATUS NVENCAPI GetEncodeCaps(void* encoder, GUID encodeGUID, NV_ENC_CAPS_PARAM* params, int* capsVal);
	static NVENCSTATUS NVENCAPI GetEncodePresetConfig(void* encoder, GUID encodeGUID, GUID presetGUID, NV_ENC_PRESET_CONFIG* params);
	static NVENCSTATUS NVENCAPI GetEncodePresetConfigEx(void* encoder, GUID encodeGUID, GUID presetGUID, NV_ENC_TUNING_INFO tuning, NV_ENC_PRESET_CONFIG* params);
	static NVENCSTATUS NVENCAPI CreateInputBuffer(void* encoder, NV_ENC_CREATE_INPUT_BUFFER* params);
	static NVENCSTATUS NVENCAPI CreateBitstreamBuffer(void* encoder, NV_ENC_CREATE_BITSTREAM_BUFFER* params);
	static NVENCSTATUS NVENCAPI CreateMVBuffer(void* encoder, NV_ENC_CREATE_MV_BUFFER* params);
	static NVENCSTATUS NVENCAPI RegisterAsyncEvent(void* encoder, NV_ENC_EVENT_PARAMS* params);
	static NVENCSTATUS NVENCAPI UnregisterAsyncEvent(void* encoder, NV_ENC_EVENT_PARAMS* params);
	static NVENCSTATUS NVENCAPI MapInputResource(void* encoder, NV_ENC_MAP_INPUT_RESOURCE* params);
	static NVENCSTATUS NVENCAPI LockInputBuffer(void* encoder, NV_ENC_LOCK_INPUT_BUFFER* params);
	static NVENCSTATUS NVENCAPI GetSequenceParams(void* encoder, NV_ENC_SEQUENCE_PARAM_PAYLOAD* params);
	static NVENCSTATUS NVENCAPI RunMotionEstimationOnly(void* encoder, NV_ENC_MEONLY_PARAMS* params);
	// post-pack bookkeeping (registered -> D3D texture, mapped -> registered)
	static NVENCSTATUS NVENCAPI UnregisterResource(void* encoder, NV_ENC_REGISTERED_PTR reg);
	static NVENCSTATUS NVENCAPI UnmapInputResource(void* encoder, NV_ENC_INPUT_PTR mapped);
};

namespace {

typedef NVENCSTATUS (NVENCAPI *PFN_CreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
PFN_CreateInstance origCreateInstance = nullptr;
PNVENCINITIALIZEENCODER origInitializeEncoder = nullptr;
PNVENCRECONFIGUREENCODER origReconfigureEncoder = nullptr;
PNVENCREGISTERRESOURCE origRegisterResource = nullptr;
PNVENCENCODEPICTURE origEncodePicture = nullptr;
PNVENCLOCKBITSTREAM origLockBitstream = nullptr;
// not shimmed, only captured: used by the preset merge to ask the driver
// for the canonical config of the target preset (see NvencTapConfig::presetMerge)
PNVENCGETENCODEPRESETCONFIGEX origGetPresetConfigEx = nullptr;
PNVENCGETENCODECAPS origGetEncodeCaps = nullptr;
PNVENCOPENENCODESESSIONEX origOpenEncodeSessionEx = nullptr;
PNVENCDESTROYENCODER origDestroyEncoder = nullptr;
PNVENCGETENCODEPRESETCONFIG origGetPresetConfig = nullptr;
PNVENCCREATEINPUTBUFFER origCreateInputBuffer = nullptr;
PNVENCCREATEBITSTREAMBUFFER origCreateBitstreamBuffer = nullptr;
PNVENCCREATEMVBUFFER origCreateMVBuffer = nullptr;
PNVENCREGISTERASYNCEVENT origRegisterAsyncEvent = nullptr;
PNVENCUNREGISTERASYNCEVENT origUnregisterAsyncEvent = nullptr;
PNVENCMAPINPUTRESOURCE origMapInputResource = nullptr;
PNVENCLOCKINPUTBUFFER origLockInputBuffer = nullptr;
PNVENCGETSEQUENCEPARAMS origGetSequenceParams = nullptr;
PNVENCRUNMOTIONESTIMATIONONLY origRunMotionEstimationOnly = nullptr;
PNVENCUNREGISTERRESOURCE origUnregisterResource = nullptr;
PNVENCUNMAPINPUTRESOURCE origUnmapInputResource = nullptr;

// ---- post-pack resource bookkeeping ----
// vrlink registers a ring of D3D11 textures (nvEncRegisterResource ->
// registered handle), maps one per frame (nvEncMapInputResource ->
// mapped/input pointer) and encodes it (nvEncEncodePicture inputBuffer ==
// mapped). two maps turn the input pointer back into the ID3D11Texture2D
// so the post-pack pass can run on it right before the encoder reads it.
std::mutex resLock;
struct RegInfo { void* texture; uint32_t fmt; uint32_t type; };
std::map<void*, RegInfo> registered;   // NV_ENC_REGISTERED_PTR -> texture
std::map<void*, void*> mappedToReg;    // NV_ENC_INPUT_PTR (mapped) -> NV_ENC_REGISTERED_PTR
bool LookupInputTexture(void* input, void** texture, uint32_t* fmt){
	std::lock_guard<std::mutex> g(resLock);
	auto m = mappedToReg.find(input);
	if(m == mappedToReg.end()){ return false; }
	auto r = registered.find(m->second);
	if(r == registered.end() || r->second.type != (uint32_t)NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX){ return false; }
	*texture = r->second.texture; *fmt = r->second.fmt; return true;
}

// split-frame experiment latch: once the 12.1-versioned structs are rejected
// by the session we stop trying (per-frame reconfigures would otherwise
// fail-and-retry forever)
std::atomic<bool> splitRejected{false};
std::atomic<int> capsLogged{0};
// SDK 12.1 struct versions (NVENCAPI_VERSION 12|1<<24 = 0x0100000C)
constexpr uint32_t kApi121 = 12u | (1u << 24);
constexpr uint32_t kInit121 = kApi121 | (6u << 16) | (0x7u << 28) | (1u << 31);   // NV_ENC_INITIALIZE_PARAMS_VER 12.1
constexpr uint32_t kConfig121 = kApi121 | (8u << 16) | (0x7u << 28) | (1u << 31); // NV_ENC_CONFIG_VER 12.1
constexpr uint32_t kRc121 = kApi121 | (1u << 16) | (0x7u << 28);                   // NV_ENC_RC_PARAMS_VER 12.1
constexpr uint32_t kReconf121 = kApi121 | (1u << 16) | (0x7u << 28) | (1u << 31); // NV_ENC_RECONFIGURE_PARAMS_VER 12.1
std::atomic<int> bigFrameLogged{0};
// submit timestamps keyed by output bitstream pointer (vrlink uses a small
// ring of bitstream buffers; async encode returns before completion)
std::mutex submitLock;
struct Submit { void* out; double t; };
Submit submits[16];
int submitHead = 0;

std::mutex statsLock;
NvencTapStats stats;
// ---- session upgrade state ----
std::mutex upgradedLock;
std::set<void*> upgradedSessions;         // encoder handles opened as 12.1
std::atomic<bool> sessionUpgradeRejected{false};
std::atomic<int> sessionNoteLogged{0};
constexpr uint32_t kApi111 = 11u | (1u << 24);
// sizes of the two structs that grew 11.1 -> 12.1 (x64, both compilers use
// natural alignment for these; verified by compiling both headers)
constexpr size_t kPicParams11Size = 3344;   // 12.1: 3360
constexpr size_t kLockBitstream11Size = 1544; // 12.1: 1552
constexpr size_t kScratch = 4096;

bool UpgradeWanted(){ return driverConfig.streamFrame.nvencSplitMode > 0 && !sessionUpgradeRejected.load(); }
bool Upgraded(void* encoder){ std::lock_guard<std::mutex> g(upgradedLock); return upgradedSessions.count(encoder) != 0; }
inline bool Is111(uint32_t v){ return (v & 0xFFFFu) == 11u; }
inline uint32_t Tag121(uint32_t digit, bool b31){ return kApi121 | (digit << 16) | (0x7u << 28) | (b31 ? (1u << 31) : 0u); }
// re-tag one version word for the duration of a call; restores on scope exit
struct Retag {
	uint32_t* slot[6]; uint32_t old[6]; int n = 0;
	void add(uint32_t* p, uint32_t digit, bool b31){
		if(!p || n >= 6 || !Is111(*p)){ return; }
		slot[n] = p; old[n] = *p; *p = Tag121(digit, b31); n++;
	}
	~Retag(){ for(int i = n - 1; i >= 0; i--){ *slot[i] = old[i]; } if(n){ std::lock_guard<std::mutex> g(statsLock); stats.retagCalls++; } }
};
void RetagInitParams(Retag &r, NV_ENC_INITIALIZE_PARAMS* p){
	if(!p){ return; }
	r.add(&p->version, 6, true);
	if(p->encodeConfig){ r.add(&p->encodeConfig->version, 8, true); r.add(&p->encodeConfig->rcParams.version, 1, false); }
}
std::atomic<bool> versionWarned{false};
std::atomic<int> initLogged{0};
std::atomic<int> reconfLogged{0};
std::atomic<int> registerLogged{0};
std::atomic<int> encodeLogged{0};

// NVENCAPI_STRUCT_VERSION(ver) = NVENCAPI_VERSION | (ver<<16) | (0x7<<28),
// NVENCAPI_VERSION = MAJOR | (MINOR<<24). bits 0..15 carry the major.
uint32_t ApiMajor(uint32_t structVersion){ return structVersion & 0xFFFFu; }
uint32_t StructVer(uint32_t structVersion){ return (structVersion >> 16) & 0xFFu; }
bool ApiSupported(uint32_t major){ return major >= 9 && major <= 13; }

bool IsHevc(const GUID &g){ return memcmp(&g, &NV_ENC_CODEC_HEVC_GUID, sizeof(GUID)) == 0; }
bool IsH264(const GUID &g){ return memcmp(&g, &NV_ENC_CODEC_H264_GUID, sizeof(GUID)) == 0; }

const char* PresetName(const GUID &g){
	if(memcmp(&g, &NV_ENC_PRESET_P1_GUID, sizeof(GUID)) == 0) return "P1";
	if(memcmp(&g, &NV_ENC_PRESET_P2_GUID, sizeof(GUID)) == 0) return "P2";
	if(memcmp(&g, &NV_ENC_PRESET_P3_GUID, sizeof(GUID)) == 0) return "P3";
	if(memcmp(&g, &NV_ENC_PRESET_P4_GUID, sizeof(GUID)) == 0) return "P4";
	if(memcmp(&g, &NV_ENC_PRESET_P5_GUID, sizeof(GUID)) == 0) return "P5";
	if(memcmp(&g, &NV_ENC_PRESET_P6_GUID, sizeof(GUID)) == 0) return "P6";
	if(memcmp(&g, &NV_ENC_PRESET_P7_GUID, sizeof(GUID)) == 0) return "P7";
	return "other/legacy";
}
const char* RcName(uint32_t m){
	switch(m){ case NV_ENC_PARAMS_RC_CONSTQP: return "CONSTQP"; case NV_ENC_PARAMS_RC_VBR: return "VBR";
		case NV_ENC_PARAMS_RC_CBR: return "CBR"; default: return "other"; }
}

// VirtualQuery-guarded hex dump for offline decoding
void SafeHexDump(const char* label, const void* ptr, size_t want){
	if(!ptr){ return; }
	MEMORY_BASIC_INFORMATION mbi = {};
	if(VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0){ return; }
	if(mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))){ return; }
	size_t avail = (size_t)((const char*)mbi.BaseAddress + mbi.RegionSize - (const char*)ptr);
	size_t len = (std::min)(want, avail);
	const unsigned char* p = (const unsigned char*)ptr;
	char line[3 * 32 + 8];
	for(size_t off = 0; off < len; off += 32){
		size_t n = (std::min<size_t>)(32, len - off);
		char* w = line;
		for(size_t i = 0; i < n; i++){ w += snprintf(w, 4, "%02x ", p[off + i]); }
		DriverLog("NvencTap: %s +0x%03x: %s", label, (unsigned)off, line);
	}
}

// ---------------------------------------------------------------------------
// snapshot / override / restore of the caller-owned NV_ENC_INITIALIZE_PARAMS
// and the NV_ENC_CONFIG it points at. Only the fields we touch are saved.
// ---------------------------------------------------------------------------
struct Snapshot {
	bool valid = false;
	NV_ENC_INITIALIZE_PARAMS* initPtr = nullptr;
	NV_ENC_CONFIG* cfgPtr = nullptr;
	NV_ENC_RC_PARAMS rc{};
	GUID preset{};
	uint32_t fpsNum = 0, fpsDen = 0;
	// split experiment: the three version words and the init bitfield word
	bool split = false;
	uint32_t initVer = 0, cfgVer = 0, rcVer = 0, initFlags = 0;
	uint32_t hevcLevel = 0, hevcTier = 0, hevcRefs = 0;
	NV_ENC_CONFIG_HEVC_VUI_PARAMETERS hevcVui{};
	bool hevc = false;
};
const GUID* PresetGuid(int p){
	switch(p){ case 1: return &NV_ENC_PRESET_P1_GUID; case 2: return &NV_ENC_PRESET_P2_GUID; case 3: return &NV_ENC_PRESET_P3_GUID;
		case 4: return &NV_ENC_PRESET_P4_GUID; case 5: return &NV_ENC_PRESET_P5_GUID; case 6: return &NV_ENC_PRESET_P6_GUID;
		case 7: return &NV_ENC_PRESET_P7_GUID; default: return nullptr; }
}

// ---------------------------------------------------------------------------
// canonical preset config fetch, for the preset merge. cached per
// (preset, tuning, codec): reconfigures are frequent and the answer is
// deterministic. struct versions are tried in three flavours because vrlink
// may be compiled against an older API than our vendored SDK 13.1 header:
//   1. caller-matched: NVENCAPI_VERSION bits lifted from the caller's own
//      struct version (major at 0..15, minor at 24..27), inner NV_ENC_CONFIG
//      version copied verbatim from the caller's accepted config
//   2. fully vendored NV_ENC_PRESET_CONFIG_VER / NV_ENC_CONFIG_VER
//   3. caller-matched with the older preset-config struct digit (4)
// a rejected flavour costs one cheap NV_ENC_ERR_INVALID_VERSION; if all
// three fail the merge is skipped and logged, GUID-swap behaviour remains.
// ---------------------------------------------------------------------------
std::mutex presetCacheLock;
struct PresetCache {
	bool valid = false;
	int preset = 0;
	NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_UNDEFINED;
	GUID codec{};
	NV_ENC_PRESET_CONFIG pc{};
} presetCache;
std::atomic<int> presetDiffLogged{0};
std::atomic<int> presetFetchFailLogged{0};
std::atomic<int> presetAutoLogged{0};

const NV_ENC_PRESET_CONFIG* FetchPresetConfig(void* encoder, const NV_ENC_INITIALIZE_PARAMS* p, const NV_ENC_CONFIG* callerCfg, int preset){
	const GUID* pg = PresetGuid(preset);
	if(!pg || !origGetPresetConfigEx || !encoder || ApiMajor(p->version) < 10){ return nullptr; }
	{
		std::lock_guard<std::mutex> g(presetCacheLock);
		if(presetCache.valid && presetCache.preset == preset && presetCache.tuning == p->tuningInfo
				&& memcmp(&presetCache.codec, &p->encodeGUID, sizeof(GUID)) == 0){
			return &presetCache.pc;
		}
	}
	// NVENCAPI_VERSION bits of the caller: major 0..15, minor 24..27
	const uint32_t callerApi = (p->version & 0xFFFFu) | (p->version & 0x0F000000u);
	const uint32_t verFlavours[3][2] = {
		{ callerApi | (5u << 16) | (0x7u << 28) | (1u << 31), callerCfg->version },
		{ NV_ENC_PRESET_CONFIG_VER, NV_ENC_CONFIG_VER },
		{ callerApi | (4u << 16) | (0x7u << 28) | (1u << 31), callerCfg->version },
	};
	for(int f = 0; f < 3; f++){
		NV_ENC_PRESET_CONFIG pc = {};
		pc.version = verFlavours[f][0];
		pc.presetCfg.version = verFlavours[f][1];
		NVENCSTATUS st = origGetPresetConfigEx(encoder, p->encodeGUID, *pg, p->tuningInfo, &pc);
		if(st == NV_ENC_SUCCESS){
			std::lock_guard<std::mutex> g(presetCacheLock);
			presetCache.valid = true; presetCache.preset = preset; presetCache.tuning = p->tuningInfo;
			presetCache.codec = p->encodeGUID; presetCache.pc = pc;
			{ std::lock_guard<std::mutex> s(statsLock); stats.presetFetchOk++; stats.presetCfgVerUsed = pc.version; }
			return &presetCache.pc;
		}
		if(st != NV_ENC_ERR_INVALID_VERSION && f == 0){
			// non-version failure: the other flavours will not do better
			if(presetFetchFailLogged.fetch_add(1) < 3){
				DriverLog("NvencTap: nvEncGetEncodePresetConfigEx(P%d, tuning %u) failed (%u) — preset merge unavailable, GUID swap only", preset, (unsigned)p->tuningInfo, (unsigned)st);
			}
			std::lock_guard<std::mutex> s(statsLock); stats.presetFetchFail++;
			return nullptr;
		}
	}
	if(presetFetchFailLogged.fetch_add(1) < 3){
		DriverLog("NvencTap: nvEncGetEncodePresetConfigEx(P%d) rejected all struct-version flavours (caller api bits 0x%08x, vendored 0x%08x) — preset merge unavailable", preset, callerApi, (unsigned)NV_ENC_PRESET_CONFIG_VER);
	}
	std::lock_guard<std::mutex> s(statsLock); stats.presetFetchFail++;
	return nullptr;
}

void Describe(const char* tag, const NV_ENC_INITIALIZE_PARAMS* p, bool full){
	const NV_ENC_CONFIG* c = p->encodeConfig;
	DriverLog("NvencTap: %s api=%u v=%u %ux%u dar=%ux%u fps=%u/%u codec=%s preset=%s tuning=%u async=%u ptd=%u vidmem=%u",
		tag, ApiMajor(p->version), StructVer(p->version), p->encodeWidth, p->encodeHeight, p->darWidth, p->darHeight,
		p->frameRateNum, p->frameRateDen, IsHevc(p->encodeGUID) ? "HEVC" : IsH264(p->encodeGUID) ? "H264" : "?",
		PresetName(p->presetGUID), (unsigned)p->tuningInfo, p->enableEncodeAsync, p->enablePTD, p->enableOutputInVidmem);
	if(!c){ DriverLog("NvencTap:   encodeConfig = null (preset defaults)"); return; }
	const NV_ENC_RC_PARAMS &rc = c->rcParams;
	DriverLog("NvencTap:   cfg v=%u gop=%u intervalP=%d rc=%s avg=%.1fM max=%.1fM vbv=%.2fMbit init=%u minQP=%u/%u maxQP=%u/%u AQ=%u(%u) tAQ=%u lookahead=%u(%u) multipass=%u zeroReorder=%u nonRefP=%u",
		StructVer(c->version), c->gopLength, c->frameIntervalP, RcName(rc.rateControlMode),
		rc.averageBitRate / 1e6, rc.maxBitRate / 1e6, rc.vbvBufferSize / 1e6, rc.vbvInitialDelay,
		rc.enableMinQP, rc.minQP.qpInterP, rc.enableMaxQP, rc.maxQP.qpInterP, rc.enableAQ, rc.aqStrength,
		rc.enableTemporalAQ, rc.enableLookahead, rc.lookaheadDepth, (unsigned)rc.multiPass, rc.zeroReorderDelay, rc.enableNonRefP);
	if(IsHevc(p->encodeGUID)){
		const NV_ENC_CONFIG_HEVC &h = c->encodeCodecConfig.hevcConfig;
		const NV_ENC_CONFIG_HEVC_VUI_PARAMETERS &v = h.hevcVUIParameters;
		DriverLog("NvencTap:   hevc level=%u tier=%u cu=%u..%u chroma=%u idr=%u intraRefresh=%u/%u refDPB=%u slices=%u/%u outBitDepth=%u inBitDepth=%u repeatSPSPPS=%u",
			h.level, h.tier, (unsigned)h.minCUSize, (unsigned)h.maxCUSize, h.chromaFormatIDC, h.idrPeriod, h.enableIntraRefresh, h.intraRefreshPeriod,
			h.maxNumRefFramesInDPB, h.sliceMode, h.sliceModeData, (unsigned)h.outputBitDepth, (unsigned)h.inputBitDepth, h.repeatSPSPPS);
		// the black-floor suspects: range flag and colour signalling
		DriverLog("NvencTap:   hevc VUI present=%u fullRange=%u colourDesc=%u primaries=%u transfer=%u matrix=%u  <- fullRange=0 means LIMITED range (16..235): a full-range source is crushed at black",
			v.videoSignalTypePresentFlag, v.videoFullRangeFlag, v.colourDescriptionPresentFlag, (unsigned)v.colourPrimaries,
			(unsigned)v.transferCharacteristics, (unsigned)v.colourMatrix);
	}
	if(full){ SafeHexDump("init", p, sizeof(NV_ENC_INITIALIZE_PARAMS)); if(c){ SafeHexDump("cfg", c, 0x200); } }
}

// remaining space for the override-description builder; 0 once full so the
// following snprintf calls become no-ops instead of taking a negative size
inline size_t WhatRem(const char* w, const char* what, size_t whatLen){
	return w < what + whatLen ? (size_t)(what + whatLen - w) : 0;
}

// returns true if anything was changed
bool ApplyOverrides(void* encoder, NV_ENC_INITIALIZE_PARAMS* p, const NvencTapConfig &cfg, Snapshot &snap, char* what, size_t whatLen){
	snap = Snapshot{};
	what[0] = 0;
	NV_ENC_CONFIG* c = p->encodeConfig;
	if(!c){ return false; }
	if(!ApiSupported(ApiMajor(p->version)) || !ApiSupported(ApiMajor(c->version))){ return false; }
	snap.valid = true; snap.initPtr = p; snap.cfgPtr = c; snap.rc = c->rcParams; snap.preset = p->presetGUID; snap.hevc = IsHevc(p->encodeGUID);
	snap.fpsNum = p->frameRateNum; snap.fpsDen = p->frameRateDen;
	// the bitfield word right after enablePTD (reportSliceOffsets.. in
	// every SDK; splitEncodeMode occupies bits 5..8 from 12.1 on)
	uint32_t* initFlags = (uint32_t*)&p->enablePTD + 1;
	snap.initVer = p->version; snap.cfgVer = c->version; snap.rcVer = c->rcParams.version; snap.initFlags = *initFlags;
	if(snap.hevc){
		snap.hevcLevel = c->encodeCodecConfig.hevcConfig.level; snap.hevcTier = c->encodeCodecConfig.hevcConfig.tier;
		snap.hevcRefs = c->encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB;
		snap.hevcVui = c->encodeCodecConfig.hevcConfig.hevcVUIParameters;
	}
	bool changed = false;
	char* w = what;
	int presetWanted = cfg.preset;
	if(presetWanted == 0 && cfg.presetAuto){
		uint32_t engines; { std::lock_guard<std::mutex> g(statsLock); engines = stats.numEncoderEngines; }
		presetWanted = engines >= 3 ? 7 : engines == 2 ? 5 : engines == 1 ? 4 : 0;
		if(presetAutoLogged.fetch_add(1) < 1){
			DriverLog("NvencTap: preset AUTO -> %s (%u NVENC engine%s)", presetWanted ? (presetWanted == 7 ? "P7" : presetWanted == 5 ? "P5" : "P4") : "leave vrlink's", engines, engines == 1 ? "" : "s");
		}
	}
	if(const GUID* pg = PresetGuid(presetWanted)){
		if(memcmp(&p->presetGUID, pg, sizeof(GUID)) != 0){
			w += snprintf(w, WhatRem(w, what, whatLen), "preset %s->P%d; ", PresetName(p->presetGUID), presetWanted);
			p->presetGUID = *pg; changed = true;
		}
		// GUID swap alone only moves NVENC's hardware-internal effort knobs
		// (per the header docs an explicit encodeConfig is never overridden
		// by presetGUID). the merge adopts the visible whitelist from the
		// canonical preset config for vrlink's own tuning. deterministic:
		// init and every reconfigure derive the same values, so the running
		// encoder state and reconfigure params never disagree.
		if(cfg.presetMerge){
			if(const NV_ENC_PRESET_CONFIG* pcp = FetchPresetConfig(encoder, p, c, presetWanted)){
				const NV_ENC_CONFIG &pcc = pcp->presetCfg;
				const NV_ENC_RC_PARAMS &prc = pcc.rcParams;
				NV_ENC_RC_PARAMS &rc = c->rcParams;
				if(presetDiffLogged.fetch_add(1) < 1){
					DriverLog("NvencTap: canonical P%d@tuning%u vs vrlink: rc=%s/%s multipass=%u/%u AQ=%u(%u)/%u(%u) tAQ=%u/%u lookahead=%u(%u)/%u(%u) intervalP=%d/%d nonRefP=%u/%u zeroReorder=%u/%u refDPB=%u/%u  (preset/vrlink; whitelist merged: multipass, tAQ, AQ-if-unset, refDPB)",
						presetWanted, (unsigned)p->tuningInfo, RcName(prc.rateControlMode), RcName(rc.rateControlMode),
						(unsigned)prc.multiPass, (unsigned)rc.multiPass, prc.enableAQ, prc.aqStrength, rc.enableAQ, rc.aqStrength,
						prc.enableTemporalAQ, rc.enableTemporalAQ, prc.enableLookahead, prc.lookaheadDepth, rc.enableLookahead, rc.lookaheadDepth,
						pcc.frameIntervalP, c->frameIntervalP, prc.enableNonRefP, rc.enableNonRefP, prc.zeroReorderDelay, rc.zeroReorderDelay,
						snap.hevc ? pcc.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB : 0, snap.hevc ? c->encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB : 0);
					if(prc.enableLookahead && prc.lookaheadDepth){
						DriverLog("NvencTap: canonical P%d wants lookahead depth %u — NOT adopted (adds %u frames of latency)", presetWanted, prc.lookaheadDepth, prc.lookaheadDepth);
					}
				}
				if(rc.multiPass != prc.multiPass){
					w += snprintf(w, WhatRem(w, what, whatLen), "multipass %u->%u; ", (unsigned)rc.multiPass, (unsigned)prc.multiPass);
					rc.multiPass = prc.multiPass; changed = true;
				}
				if(rc.enableTemporalAQ != prc.enableTemporalAQ){
					w += snprintf(w, WhatRem(w, what, whatLen), "tAQ %u->%u; ", rc.enableTemporalAQ, prc.enableTemporalAQ);
					rc.enableTemporalAQ = prc.enableTemporalAQ; changed = true;
				}
				// spatial AQ from the preset only when nothing else (manual
				// field or tier) claims it; the explicit AQ block below wins
				if(cfg.aqStrength <= 0 && (rc.enableAQ != prc.enableAQ || rc.aqStrength != prc.aqStrength)){
					w += snprintf(w, WhatRem(w, what, whatLen), "AQ %u(%u)->%u(%u) [preset]; ", rc.enableAQ, rc.aqStrength, prc.enableAQ, prc.aqStrength);
					rc.enableAQ = prc.enableAQ; rc.aqStrength = prc.aqStrength; changed = true;
				}
				if(snap.hevc){
					uint32_t prefs = pcc.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB;
					NV_ENC_CONFIG_HEVC &h = c->encodeCodecConfig.hevcConfig;
					if(prefs && h.maxNumRefFramesInDPB != prefs){
						w += snprintf(w, WhatRem(w, what, whatLen), "refDPB %u->%u; ", h.maxNumRefFramesInDPB, prefs);
						h.maxNumRefFramesInDPB = prefs; changed = true;
					}
				}
			}
		}
	}
	if(snap.hevc){
		NV_ENC_CONFIG_HEVC_VUI_PARAMETERS &v = c->encodeCodecConfig.hevcConfig.hevcVUIParameters;
		if((cfg.vuiFullRange == 0 || cfg.vuiFullRange == 1) && v.videoFullRangeFlag != (uint32_t)cfg.vuiFullRange){
			w += snprintf(w, WhatRem(w, what, whatLen), "vuiFullRange %u->%d; ", v.videoFullRangeFlag, cfg.vuiFullRange);
			v.videoFullRangeFlag = (uint32_t)cfg.vuiFullRange; v.videoSignalTypePresentFlag = 1; changed = true;
		}
		if(cfg.vuiMatrix >= 0 && (uint32_t)v.colourMatrix != (uint32_t)cfg.vuiMatrix){
			w += snprintf(w, WhatRem(w, what, whatLen), "vuiMatrix %u->%d; ", (uint32_t)v.colourMatrix, cfg.vuiMatrix);
			v.colourMatrix = (NV_ENC_VUI_MATRIX_COEFFS)cfg.vuiMatrix;
			v.colourDescriptionPresentFlag = 1; v.videoSignalTypePresentFlag = 1; changed = true;
		}
		if(cfg.vuiPrimaries >= 0 && (uint32_t)v.colourPrimaries != (uint32_t)cfg.vuiPrimaries){
			w += snprintf(w, WhatRem(w, what, whatLen), "vuiPrimaries %u->%d; ", (uint32_t)v.colourPrimaries, cfg.vuiPrimaries);
			v.colourPrimaries = (NV_ENC_VUI_COLOR_PRIMARIES)cfg.vuiPrimaries;
			v.colourDescriptionPresentFlag = 1; v.videoSignalTypePresentFlag = 1; changed = true;
		}
		if(cfg.vuiTransfer >= 0 && (uint32_t)v.transferCharacteristics != (uint32_t)cfg.vuiTransfer){
			w += snprintf(w, WhatRem(w, what, whatLen), "vuiTransfer %u->%d; ", (uint32_t)v.transferCharacteristics, cfg.vuiTransfer);
			v.transferCharacteristics = (NV_ENC_VUI_TRANSFER_CHARACTERISTIC)cfg.vuiTransfer;
			v.colourDescriptionPresentFlag = 1; v.videoSignalTypePresentFlag = 1; changed = true;
		}
	}
	if(cfg.fixLevel && snap.hevc){
		NV_ENC_CONFIG_HEVC &h = c->encodeCodecConfig.hevcConfig;
		if(h.level != NV_ENC_LEVEL_AUTOSELECT || h.tier != NV_ENC_TIER_HEVC_HIGH){
			w += snprintf(w, WhatRem(w, what, whatLen), "level %u->auto tier %u->high; ", h.level, h.tier);
			h.level = NV_ENC_LEVEL_AUTOSELECT; h.tier = NV_ENC_TIER_HEVC_HIGH; changed = true;
		}
	}
	if(cfg.splitMode > 0 && !splitRejected.load() && snap.hevc){
		uint32_t mode = (uint32_t)(std::min)(15, cfg.splitMode);
		if(mode > 4 && mode != 15){ mode = 1; }
		if(ApiMajor(p->version) == 11){
			// not upgraded: the session was opened before the tap hooked
			// it, or the 12.1 open was rejected. U0 proved in-place struct
			// bumps are refused (INVALID_VERSION), so nothing to try here.
			if(sessionNoteLogged.fetch_add(1) < 1){
				DriverLog("NvencTap: split-frame: session is 11.1 (not upgraded) — splitEncodeMode unavailable on this encoder");
			}
		}else if(ApiMajor(p->version) >= 12 && StructVer(p->version) >= 6 && p->splitEncodeMode != mode){
			// native field (a future vrlink built against 12.1+)
			w += snprintf(w, WhatRem(w, what, whatLen), "SPLIT %u->%u; ", p->splitEncodeMode, mode);
			*initFlags = (snap.initFlags & ~(0xFu << 5)) | (mode << 5);
			snap.split = true; changed = true;
		}
	}
	if(cfg.forceFps > 0 && (p->frameRateNum != (uint32_t)cfg.forceFps || p->frameRateDen != 1)){
		w += snprintf(w, WhatRem(w, what, whatLen), "fps %u/%u->%d/1; ", p->frameRateNum, p->frameRateDen, cfg.forceFps);
		p->frameRateNum = (uint32_t)cfg.forceFps; p->frameRateDen = 1; changed = true;
	}
	if(cfg.bitrateMbit > 0){
		NV_ENC_RC_PARAMS &rc = c->rcParams;
		int64_t target = cfg.bitrateMbit * 1000000ll;
		if(cfg.bitrateScale && cfg.vrlinkClampMbit > 0 && rc.averageBitRate > 0){
			// scale vrlink's request: its clamp maps to our target, a
			// backoff below the clamp stays a proportional backoff
			target = (int64_t)((double)rc.averageBitRate * (double)cfg.bitrateMbit / (double)cfg.vrlinkClampMbit);
			target = (std::min<int64_t>)(target, cfg.bitrateMbit * 1000000ll);
		}
		uint32_t avg = (uint32_t)(std::min<int64_t>)(target, 2000000000ll);
		double head = 1.0 + (std::max)(0, cfg.maxBitrateHeadroomPct) / 100.0;
		uint32_t mx = (uint32_t)(std::min<double>)(avg * head, 2000000000.0);
		if(rc.averageBitRate != avg || rc.maxBitRate != mx){
			double ratio = rc.averageBitRate ? (double)avg / rc.averageBitRate : 1.0;
			w += snprintf(w, WhatRem(w, what, whatLen), "avg %.0fM->%.0fM max %.0fM->%.0fM; ", rc.averageBitRate / 1e6, avg / 1e6, rc.maxBitRate / 1e6, mx / 1e6);
			rc.averageBitRate = avg;
			rc.maxBitRate = mx;
			if(rc.vbvBufferSize){ rc.vbvBufferSize = (uint32_t)(std::min<double>)(rc.vbvBufferSize * ratio, 2000000000.0); }
			changed = true;
		}
	}
	if(cfg.vbvFrames > 0){
		NV_ENC_RC_PARAMS &rc = c->rcParams;
		// nominal rate, never vrlink's per-call estimate (see forceFps)
		double fps = cfg.forceFps > 0 ? (double)cfg.forceFps : 90.0;
		uint32_t vbv = (uint32_t)(std::min<double>)(rc.averageBitRate / fps * cfg.vbvFrames, 2000000000.0);
		if(vbv && rc.vbvBufferSize != vbv){
			w += snprintf(w, WhatRem(w, what, whatLen), "vbv %.2fM->%.2fM(%d fr); ", rc.vbvBufferSize / 1e6, vbv / 1e6, cfg.vbvFrames);
			rc.vbvBufferSize = vbv; changed = true;
		}
	}
	if(cfg.maxQp > 0){
		NV_ENC_RC_PARAMS &rc = c->rcParams;
		uint32_t q = (uint32_t)(std::min)(51, cfg.maxQp);
		if(!rc.enableMaxQP || rc.maxQP.qpInterP != q){
			w += snprintf(w, WhatRem(w, what, whatLen), "maxQP %u(%u)->%u; ", rc.enableMaxQP, rc.maxQP.qpInterP, q);
			rc.enableMaxQP = 1; rc.maxQP.qpInterP = q; rc.maxQP.qpInterB = q; rc.maxQP.qpIntra = q; changed = true;
		}
	}
	if(cfg.minQp > 0 || cfg.minQpIntra > 0){
		NV_ENC_RC_PARAMS &rc = c->rcParams;
		uint32_t q = (uint32_t)(std::min)(51, cfg.minQp > 0 ? cfg.minQp : 1);
		uint32_t qi = (uint32_t)(std::min)(51, cfg.minQpIntra > 0 ? cfg.minQpIntra : (int)q);
		if(!rc.enableMinQP || rc.minQP.qpInterP != q || rc.minQP.qpIntra != qi){
			w += snprintf(w, WhatRem(w, what, whatLen), "minQP %u(P%u/I%u)->P%u/I%u; ", rc.enableMinQP, rc.minQP.qpInterP, rc.minQP.qpIntra, q, qi);
			rc.enableMinQP = 1; rc.minQP.qpInterP = q; rc.minQP.qpInterB = q; rc.minQP.qpIntra = qi; changed = true;
		}
	}
	if(cfg.forceCbr){
		NV_ENC_RC_PARAMS &rc = c->rcParams;
		uint8_t kf = (uint8_t)(std::max)(1, (std::min)(4, cfg.lowDelayKfScale));
		if(rc.rateControlMode != NV_ENC_PARAMS_RC_CBR || rc.lowDelayKeyFrameScale != kf){
			w += snprintf(w, WhatRem(w, what, whatLen), "rc %s->CBR lowDelayKF %u->%u; ", RcName(rc.rateControlMode), rc.lowDelayKeyFrameScale, (unsigned)kf);
			rc.rateControlMode = NV_ENC_PARAMS_RC_CBR; rc.lowDelayKeyFrameScale = kf;
			if(rc.maxBitRate != rc.averageBitRate){ rc.maxBitRate = rc.averageBitRate; }
			changed = true;
		}
	}
	if(cfg.aqStrength > 0){
		NV_ENC_RC_PARAMS &rc = c->rcParams;
		uint32_t s = (uint32_t)(std::min)(15, cfg.aqStrength);
		if(!rc.enableAQ || rc.aqStrength != s){
			w += snprintf(w, WhatRem(w, what, whatLen), "AQ %u(%u)->on(%u); ", rc.enableAQ, rc.aqStrength, s);
			rc.enableAQ = 1; rc.aqStrength = s; changed = true;
		}
	}
	return changed;
}

void Restore(const Snapshot &snap){
	if(!snap.valid || !snap.cfgPtr){ return; }
	snap.cfgPtr->rcParams = snap.rc;
	if(snap.initPtr){ snap.initPtr->presetGUID = snap.preset; snap.initPtr->frameRateNum = snap.fpsNum; snap.initPtr->frameRateDen = snap.fpsDen; }
	if(snap.split && snap.initPtr){
		snap.initPtr->version = snap.initVer; snap.cfgPtr->version = snap.cfgVer; snap.cfgPtr->rcParams.version = snap.rcVer; // no-ops on the native path
		*((uint32_t*)&snap.initPtr->enablePTD + 1) = snap.initFlags;
	}
	if(snap.hevc){
		snap.cfgPtr->encodeCodecConfig.hevcConfig.level = snap.hevcLevel;
		snap.cfgPtr->encodeCodecConfig.hevcConfig.tier = snap.hevcTier;
		snap.cfgPtr->encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = snap.hevcRefs;
		snap.cfgPtr->encodeCodecConfig.hevcConfig.hevcVUIParameters = snap.hevcVui;
	}
}

// vrlink's own request, sampled before any override
void RecordRequest(const NV_ENC_INITIALIZE_PARAMS* p){
	if(!p->encodeConfig || !p->encodeConfig->rcParams.averageBitRate){ return; }
	double m = p->encodeConfig->rcParams.averageBitRate / 1e6;
	std::lock_guard<std::mutex> g(statsLock);
	stats.ivReqCount++; stats.ivReqSumMbit += m;
	if(m < stats.ivReqMinMbit){ stats.ivReqMinMbit = m; }
	if(m > stats.ivReqMaxMbit){ stats.ivReqMaxMbit = m; }
}

void RecordLast(const NV_ENC_INITIALIZE_PARAMS* p){
	std::lock_guard<std::mutex> g(statsLock);
	stats.apiMajor = ApiMajor(p->version);
	stats.lastWidth = p->encodeWidth; stats.lastHeight = p->encodeHeight;
	if(p->encodeConfig){
		stats.lastAvgBitrate = p->encodeConfig->rcParams.averageBitRate;
		stats.lastMaxBitrate = p->encodeConfig->rcParams.maxBitRate;
		if(IsHevc(p->encodeGUID)){
			stats.lastLevel = p->encodeConfig->encodeCodecConfig.hevcConfig.level;
			stats.lastTier = p->encodeConfig->encodeCodecConfig.hevcConfig.tier;
			stats.lastBitDepth = (uint32_t)p->encodeConfig->encodeCodecConfig.hevcConfig.outputBitDepth;
		}
	}
}

// common body for init and reconfigure: override -> call -> restore ->
// retry pristine on failure
template<typename Call>
NVENCSTATUS Guarded(const char* tag, void* encoder, NV_ENC_INITIALIZE_PARAMS* p, Call call, bool logThis,
		uint32_t &calls, uint32_t &overridden, uint32_t &failedThenOk, uint32_t &failed, uint32_t* failedStock,
		uint32_t* outerVersion = nullptr){
	NvencTapConfig cfg = NvencTap::Get().GetConfig();
	{ std::lock_guard<std::mutex> g(statsLock); calls++; }
	RecordRequest(p);
	if(logThis){ Describe(tag, p, cfg.verbose); }
	if(!ApiSupported(ApiMajor(p->version))){
		if(!versionWarned.exchange(true)){
			DriverLog("NvencTap: unsupported NVENC API major %u (struct version 0x%08x) — observe only", ApiMajor(p->version), p->version);
		}
		RecordLast(p);
		return call();
	}
	Snapshot snap; char what[256];
	bool changed = cfg.enabled && ApplyOverrides(encoder, p, cfg, snap, what, sizeof(what));
	if(changed){
		if(logThis){ DriverLog("NvencTap: %s override: %s", tag, what); }
		RecordLast(p);
		uint32_t outerSaved = 0;
		if(snap.split && outerVersion){ outerSaved = *outerVersion; *outerVersion = kReconf121; }
		NVENCSTATUS st = call();
		Restore(snap);
		if(snap.split && outerVersion){ *outerVersion = outerSaved; }
		if(st == NV_ENC_SUCCESS){
			std::lock_guard<std::mutex> g(statsLock); overridden++;
			if(snap.split){ stats.splitApplied++; if(stats.splitState != 1){ stats.splitState = 1; DriverLog("NvencTap: SPLIT-FRAME: 12.1-versioned structs ACCEPTED by the 11.1 session on %s — explicit split mode is live", tag); } }
			return st;
		}
		if(snap.split){
			splitRejected.store(true);
			{ std::lock_guard<std::mutex> g(statsLock); stats.splitState = 2; stats.splitStatus = (uint32_t)st; }
			DriverLog("NvencTap: SPLIT-FRAME: %s REJECTED the 12.1-versioned structs (status %u%s) — latched off for this session; other overrides continue", tag, (unsigned)st,
				st == NV_ENC_ERR_INVALID_VERSION ? " = INVALID_VERSION: the session is pinned to its 11.1 apiVersion, next step is bumping nvEncOpenEncodeSessionEx" : "");
		}
		DriverLog("NvencTap: %s WITH override failed (%u) — retrying with vrlink's pristine params", tag, (unsigned)st);
		NVENCSTATUS st2 = call();
		std::lock_guard<std::mutex> g(statsLock);
		if(st2 == NV_ENC_SUCCESS){ failedThenOk++; } else { failed++; }
		return st2;
	}
	RecordLast(p);
	NVENCSTATUS st = call();
	if(st != NV_ENC_SUCCESS){
		std::lock_guard<std::mutex> g(statsLock);
		if(failedStock){ (*failedStock)++; } else { failed++; }
	}
	return st;
}

} // namespace

// ---------------------------------------------------------------------------
NVENCSTATUS NVENCAPI NvencTapShims::InitializeEncoder(void* encoder, NV_ENC_INITIALIZE_PARAMS* params){
	if(!params){ return origInitializeEncoder(encoder, params); }
	// re-tag first so the caps probe below and Guarded see the session's version
	Retag rt; if(Upgraded(encoder)){ RetagInitParams(rt, params); }
	// one-shot: how many NVENC engines does this session see (NV_ENC_CAPS_
	// NUM_ENCODER_ENGINES is a 12.0 enum value; NV_ENC_CAPS_PARAM itself is
	// unchanged since 11.1, so we ask with the caller's own struct version)
	if(origGetEncodeCaps && capsLogged.fetch_add(1) < 1){
		NV_ENC_CAPS_PARAM cp = {};
		cp.version = ((params->version & 0xFFFFu) | (params->version & 0x0F000000u)) | (1u << 16) | (0x7u << 28);
		cp.capsToQuery = NV_ENC_CAPS_NUM_ENCODER_ENGINES;
		int v = 0;
		NVENCSTATUS cs = origGetEncodeCaps(encoder, params->encodeGUID, &cp, &v);
		DriverLog("NvencTap: NV_ENC_CAPS_NUM_ENCODER_ENGINES -> %d (status %u) | frame %ux%u: implicit split needs height>=2112, NVENCs>=2, preset P1..P4 at LL/ULL",
			v, (unsigned)cs, params->encodeWidth, params->encodeHeight);
		std::lock_guard<std::mutex> g(statsLock); stats.numEncoderEngines = cs == NV_ENC_SUCCESS ? (uint32_t)v : 0;
	}
	bool logThis = initLogged.fetch_add(1) < 4 || NvencTap::Get().GetConfig().verbose;
	return Guarded("nvEncInitializeEncoder", encoder, params, [&]{ return origInitializeEncoder(encoder, params); }, logThis,
		stats.initCalls, stats.initOverridden, stats.initFailedThenPristineOk, stats.initFailed, nullptr);
}

NVENCSTATUS NVENCAPI NvencTapShims::ReconfigureEncoder(void* encoder, NV_ENC_RECONFIGURE_PARAMS* params){
	if(!params){ return origReconfigureEncoder(encoder, params); }
	NvencTapConfig cfg = NvencTap::Get().GetConfig();
	bool logThis = reconfLogged.fetch_add(1) < 4 || cfg.verbose;
	Retag rt; if(Upgraded(encoder)){ rt.add(&params->version, 1, true); RetagInitParams(rt, &params->reInitEncodeParams); }
	NVENCSTATUS st = Guarded("nvEncReconfigureEncoder", encoder, &params->reInitEncodeParams,
		[&]{ return origReconfigureEncoder(encoder, params); }, logThis,
		stats.reconfCalls, stats.reconfOverridden, stats.reconfFailedThenPristineOk, stats.reconfFailed,
		cfg.enabled ? nullptr : &stats.reconfFailedStock, &params->version);
	if(logThis){ DriverLog("NvencTap: nvEncReconfigureEncoder -> %u (reset=%u forceIDR=%u)", (unsigned)st, params->resetEncoder, params->forceIDR); }
	return st;
}

NVENCSTATUS NVENCAPI NvencTapShims::RegisterResource(void* encoder, NV_ENC_REGISTER_RESOURCE* params){
	{ std::lock_guard<std::mutex> g(statsLock); stats.registerCalls++; }
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 4, false); }
	if(params && registerLogged.fetch_add(1) < 12){
		DriverLog("NvencTap: nvEncRegisterResource encoder=%p type=%u %ux%u pitch=%u fmt=0x%x usage=%u res=%p — a LAYER-sized DX resource here is the direct-encode substitution site",
			encoder, (unsigned)params->resourceType, params->width, params->height, params->pitch,
			(unsigned)params->bufferFormat, (unsigned)params->bufferUsage, params->resourceToRegister);
	}
	NVENCSTATUS st = origRegisterResource(encoder, params);
	if(st == NV_ENC_SUCCESS && params && params->registeredResource){
		std::lock_guard<std::mutex> g(resLock);
		registered[params->registeredResource] = { params->resourceToRegister, (uint32_t)params->bufferFormat, (uint32_t)params->resourceType };
	}
	return st;
}
NVENCSTATUS NVENCAPI NvencTapShims::UnregisterResource(void* encoder, NV_ENC_REGISTERED_PTR reg){
	{ std::lock_guard<std::mutex> g(resLock); registered.erase(reg); }
	return origUnregisterResource(encoder, reg);
}
NVENCSTATUS NVENCAPI NvencTapShims::UnmapInputResource(void* encoder, NV_ENC_INPUT_PTR mapped){
	{ std::lock_guard<std::mutex> g(resLock); mappedToReg.erase(mapped); }
	return origUnmapInputResource(encoder, mapped);
}

NVENCSTATUS NVENCAPI NvencTapShims::EncodePicture(void* encoder, NV_ENC_PIC_PARAMS* params){
	{ std::lock_guard<std::mutex> g(statsLock); stats.encodeCalls++; }
	if(params && params->outputBitstream){
		std::lock_guard<std::mutex> g(submitLock);
		submits[submitHead] = { params->outputBitstream, NowSecondsNv() };
		submitHead = (submitHead + 1) % 16;
	}
	if(params && encodeLogged.fetch_add(1) < 3){
		DriverLog("NvencTap: nvEncEncodePicture encoder=%p %ux%u pitch=%u in=%p out=%p fmt=0x%x picType=%u struct=%u frameIdx=%u flags=0x%x",
			encoder, params->inputWidth, params->inputHeight, params->inputPitch, params->inputBuffer, params->outputBitstream,
			(unsigned)params->bufferFmt, (unsigned)params->pictureType, (unsigned)params->pictureStruct, params->frameIdx, params->encodePicFlags);
	}
	// post-pack pass on the packed frame, before the encoder reads it
	if(params && params->inputBuffer){
		void* tex = nullptr; uint32_t fmt = 0;
		if(LookupInputTexture(params->inputBuffer, &tex, &fmt)){ NvencPostPack::Process(tex, fmt); }
	}
	double t0 = NowSecondsNv();
	NVENCSTATUS st;
	if(params && Upgraded(encoder) && Is111(params->version)){
		// NV_ENC_PIC_PARAMS grew 11.1 -> 12.1: call with a zero-extended
		// scratch copy so the driver never reads past vrlink's struct
		alignas(16) unsigned char scratch[kScratch] = {};
		memcpy(scratch, params, kPicParams11Size);
		((NV_ENC_PIC_PARAMS*)scratch)->version = Tag121(6, true);
		st = origEncodePicture(encoder, (NV_ENC_PIC_PARAMS*)scratch);
		{ std::lock_guard<std::mutex> g(statsLock); stats.retagCalls++; }
	}else{
		st = origEncodePicture(encoder, params);
	}
	double ms = (NowSecondsNv() - t0) * 1000.0;
	if(params && params->inputBuffer){ // skip the EOS/flush calls (in=null)
		std::lock_guard<std::mutex> g(statsLock);
		stats.ivSubmitSamples++; stats.ivSubmitSumMs += ms; if(ms > stats.ivSubmitMaxMs){ stats.ivSubmitMaxMs = ms; } if(ms >= 10.0){ stats.ivSubmitOver10++; }
	}
	return st;
}

NVENCSTATUS NVENCAPI NvencTapShims::LockBitstream(void* encoder, NV_ENC_LOCK_BITSTREAM* params){
	NVENCSTATUS st;
	if(params && Upgraded(encoder) && Is111(params->version)){
		// NV_ENC_LOCK_BITSTREAM grew 11.1 -> 12.1 and has [out] fields: call
		// on a scratch copy, copy the 11.1 extent back
		alignas(16) unsigned char scratch[kScratch] = {};
		memcpy(scratch, params, kLockBitstream11Size);
		uint32_t saved = params->version;
		((NV_ENC_LOCK_BITSTREAM*)scratch)->version = Tag121(1, true);
		st = origLockBitstream(encoder, (NV_ENC_LOCK_BITSTREAM*)scratch);
		memcpy(params, scratch, kLockBitstream11Size);
		params->version = saved;
		{ std::lock_guard<std::mutex> g(statsLock); stats.retagCalls++; }
	}else{
		st = origLockBitstream(encoder, params);
	}
	if(st != NV_ENC_SUCCESS || !params){ return st; }
	double latMs = -1;
	if(params->outputBitstream){
		double now = NowSecondsNv();
		std::lock_guard<std::mutex> g(submitLock);
		for(int i = 0; i < 16; i++){
			if(submits[i].out == params->outputBitstream){ latMs = (now - submits[i].t) * 1000.0; submits[i].out = nullptr; break; }
		}
	}
	uint32_t n = params->bitstreamSizeInBytes;
	bool idr = params->pictureType == NV_ENC_PIC_TYPE_IDR || params->pictureType == NV_ENC_PIC_TYPE_I;
	bool newMax = false;
	{
		std::lock_guard<std::mutex> g(statsLock);
		stats.lockCalls++;
		stats.lastFrameBytes = n; stats.sumFrameBytes += n;
		if(n > stats.maxFrameBytes){ stats.maxFrameBytes = n; newMax = true; }
		if(idr){ stats.idrFrames++; if(n > stats.maxFrameBytesIdr){ stats.maxFrameBytesIdr = n; } }
		int b = n < 262144 ? 0 : n < 524288 ? 1 : n < 1048576 ? 2 : n < 1572864 ? 3 : n < 2097152 ? 4 : n < 4194304 ? 5 : 6;
		stats.sizeBucket[b]++;
		stats.lastAvgQp = params->frameAvgQP; if(params->frameAvgQP > stats.maxAvgQp){ stats.maxAvgQp = params->frameAvgQP; }
		if(latMs >= 0 && latMs < 5000){
			stats.latSamples++; stats.latSumMs += latMs; if(latMs > stats.latMaxMs){ stats.latMaxMs = latMs; }
			stats.ivLatSamples++; stats.ivLatSumMs += latMs; if(latMs > stats.ivLatMaxMs){ stats.ivLatMaxMs = latMs; } if(latMs >= 11.1){ stats.ivLatOver11++; }
			int lb = latMs < 5 ? 0 : latMs < 8 ? 1 : latMs < 11 ? 2 : latMs < 16 ? 3 : latMs < 33 ? 4 : 5;
			stats.latBucket[lb]++;
		}
	}
	// every new record above 1 MB is worth a line: the last few of these
	// before a "Packet too big" fault bracket vrlink's send limit
	if(newMax && n >= 1048576 && bigFrameLogged.fetch_add(1) < 40){
		DriverLog("NvencTap: new largest encoded frame: %u bytes (%.2f MB) picType=%u%s frameIdx=%u avgQP=%u slices=%u",
			n, n / 1048576.0, (unsigned)params->pictureType, idr ? " IDR/I" : "", params->frameIdx, params->frameAvgQP, params->numSlices);
	}
	return st;
}

// ---- session upgrade shims ----
NVENCSTATUS NVENCAPI NvencTapShims::OpenEncodeSessionEx(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* params, void** encoder){
	if(!params || !encoder || !UpgradeWanted() || params->apiVersion != kApi111){
		return origOpenEncodeSessionEx(params, encoder);
	}
	uint32_t savedVer = params->version, savedApi = params->apiVersion;
	params->apiVersion = kApi121;
	if(Is111(params->version)){ params->version = Tag121(1, false); }
	NVENCSTATUS st = origOpenEncodeSessionEx(params, encoder);
	params->version = savedVer; params->apiVersion = savedApi;
	if(st == NV_ENC_SUCCESS){
		{ std::lock_guard<std::mutex> g(upgradedLock); upgradedSessions.insert(*encoder); }
		{ std::lock_guard<std::mutex> g(statsLock); stats.sessionUpgrade = 1; }
		DriverLog("NvencTap: SESSION UPGRADE: nvEncOpenEncodeSessionEx accepted apiVersion 12.1 (encoder %p) — vrlink's 11.1 structs will be re-tagged as 12.1 on every call; splitEncodeMode is now reachable", *encoder);
		return st;
	}
	sessionUpgradeRejected.store(true);
	{ std::lock_guard<std::mutex> g(statsLock); stats.sessionUpgrade = 2; stats.sessionUpgradeStatus = (uint32_t)st; }
	DriverLog("NvencTap: SESSION UPGRADE: nvEncOpenEncodeSessionEx REJECTED apiVersion 12.1 (status %u) — opening pristine 11.1, upgrade latched off", (unsigned)st);
	return origOpenEncodeSessionEx(params, encoder);
}
NVENCSTATUS NVENCAPI NvencTapShims::DestroyEncoder(void* encoder){
	{ std::lock_guard<std::mutex> g(upgradedLock); upgradedSessions.erase(encoder); }
	return origDestroyEncoder(encoder);
}
NVENCSTATUS NVENCAPI NvencTapShims::GetEncodeCaps(void* encoder, GUID encodeGUID, NV_ENC_CAPS_PARAM* params, int* capsVal){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 1, false); }
	return origGetEncodeCaps(encoder, encodeGUID, params, capsVal);
}
NVENCSTATUS NVENCAPI NvencTapShims::GetEncodePresetConfig(void* encoder, GUID encodeGUID, GUID presetGUID, NV_ENC_PRESET_CONFIG* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 4, true); rt.add(&params->presetCfg.version, 8, true); }
	return origGetPresetConfig(encoder, encodeGUID, presetGUID, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::GetEncodePresetConfigEx(void* encoder, GUID encodeGUID, GUID presetGUID, NV_ENC_TUNING_INFO tuning, NV_ENC_PRESET_CONFIG* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 4, true); rt.add(&params->presetCfg.version, 8, true); }
	return origGetPresetConfigEx(encoder, encodeGUID, presetGUID, tuning, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::CreateInputBuffer(void* encoder, NV_ENC_CREATE_INPUT_BUFFER* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 1, false); }
	return origCreateInputBuffer(encoder, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::CreateBitstreamBuffer(void* encoder, NV_ENC_CREATE_BITSTREAM_BUFFER* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 1, false); }
	return origCreateBitstreamBuffer(encoder, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::CreateMVBuffer(void* encoder, NV_ENC_CREATE_MV_BUFFER* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 1, false); }
	return origCreateMVBuffer(encoder, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::RegisterAsyncEvent(void* encoder, NV_ENC_EVENT_PARAMS* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 1, false); }
	return origRegisterAsyncEvent(encoder, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::UnregisterAsyncEvent(void* encoder, NV_ENC_EVENT_PARAMS* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 1, false); }
	return origUnregisterAsyncEvent(encoder, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::MapInputResource(void* encoder, NV_ENC_MAP_INPUT_RESOURCE* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 4, false); }
	NVENCSTATUS st = origMapInputResource(encoder, params);
	if(st == NV_ENC_SUCCESS && params && params->mappedResource){
		std::lock_guard<std::mutex> g(resLock);
		mappedToReg[params->mappedResource] = params->registeredResource;
	}
	return st;
}
NVENCSTATUS NVENCAPI NvencTapShims::LockInputBuffer(void* encoder, NV_ENC_LOCK_INPUT_BUFFER* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 1, false); }
	return origLockInputBuffer(encoder, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::GetSequenceParams(void* encoder, NV_ENC_SEQUENCE_PARAM_PAYLOAD* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 1, false); }
	return origGetSequenceParams(encoder, params);
}
NVENCSTATUS NVENCAPI NvencTapShims::RunMotionEstimationOnly(void* encoder, NV_ENC_MEONLY_PARAMS* params){
	Retag rt; if(params && Upgraded(encoder)){ rt.add(&params->version, 3, false); }
	return origRunMotionEstimationOnly(encoder, params);
}

NVENCSTATUS NVENCAPI NvencTapShims::CreateInstance(NV_ENCODE_API_FUNCTION_LIST* list){
	// present the function list as 12.1 when an upgrade is wanted: same
	// size (12.1 only appends pointers into 11.1's reserved tail), and it
	// keeps the runtime's view of this client consistent with the session
	uint32_t listSaved = list ? list->version : 0;
	bool listBumped = false;
	if(list && UpgradeWanted() && Is111(list->version)){ list->version = Tag121(2, false); listBumped = true; }
	NVENCSTATUS status = origCreateInstance(list);
	if(listBumped && status != NV_ENC_SUCCESS){
		DriverLog("NvencTap: SESSION UPGRADE: NvEncodeAPICreateInstance refused a 12.1 function list (%u) — retrying as 11.1, upgrade latched off", (unsigned)status);
		list->version = listSaved; sessionUpgradeRejected.store(true);
		status = origCreateInstance(list);
	}
	if(status != NV_ENC_SUCCESS || !list){
		DriverLog("NvencTap: NvEncodeAPICreateInstance status=%u (list not wrapped)", (unsigned)status);
		return status;
	}
	uint32_t major = ApiMajor(list->version);
	if(!ApiSupported(major)){
		DriverLog("NvencTap: function list version 0x%08x (API major %u) outside 9..13 — NOT wrapped", list->version, major);
		return status;
	}
	// wrap by member name. the driver returns the same function pointers
	// for every instance, so the static originals are stable; a re-call on
	// an already-wrapped list is guarded by pointer identity.
#define WRAP(member, shim, orig) \
	if(list->member && list->member != &NvencTapShims::shim){ orig = list->member; list->member = &NvencTapShims::shim; }
	WRAP(nvEncInitializeEncoder, InitializeEncoder, origInitializeEncoder);
	WRAP(nvEncReconfigureEncoder, ReconfigureEncoder, origReconfigureEncoder);
	WRAP(nvEncRegisterResource, RegisterResource, origRegisterResource);
	WRAP(nvEncEncodePicture, EncodePicture, origEncodePicture);
	WRAP(nvEncLockBitstream, LockBitstream, origLockBitstream);
	WRAP(nvEncOpenEncodeSessionEx, OpenEncodeSessionEx, origOpenEncodeSessionEx);
	WRAP(nvEncDestroyEncoder, DestroyEncoder, origDestroyEncoder);
	WRAP(nvEncGetEncodeCaps, GetEncodeCaps, origGetEncodeCaps);
	WRAP(nvEncGetEncodePresetConfig, GetEncodePresetConfig, origGetPresetConfig);
	WRAP(nvEncGetEncodePresetConfigEx, GetEncodePresetConfigEx, origGetPresetConfigEx);
	WRAP(nvEncCreateInputBuffer, CreateInputBuffer, origCreateInputBuffer);
	WRAP(nvEncCreateBitstreamBuffer, CreateBitstreamBuffer, origCreateBitstreamBuffer);
	WRAP(nvEncCreateMVBuffer, CreateMVBuffer, origCreateMVBuffer);
	WRAP(nvEncRegisterAsyncEvent, RegisterAsyncEvent, origRegisterAsyncEvent);
	WRAP(nvEncUnregisterAsyncEvent, UnregisterAsyncEvent, origUnregisterAsyncEvent);
	WRAP(nvEncMapInputResource, MapInputResource, origMapInputResource);
	WRAP(nvEncLockInputBuffer, LockInputBuffer, origLockInputBuffer);
	WRAP(nvEncGetSequenceParams, GetSequenceParams, origGetSequenceParams);
	WRAP(nvEncRunMotionEstimationOnly, RunMotionEstimationOnly, origRunMotionEstimationOnly);
	WRAP(nvEncUnregisterResource, UnregisterResource, origUnregisterResource);
	WRAP(nvEncUnmapInputResource, UnmapInputResource, origUnmapInputResource);
#undef WRAP
	if(listBumped){ list->version = listSaved; } // vrlink may compare its own word
	splitRejected.store(false);
	{ std::lock_guard<std::mutex> g(statsLock); stats.apiMajor = major; }
	DriverLog("NvencTap: function list wrapped (API major %u, list version 0x%08x): init/reconfigure/register/encode/lockBitstream", major, list->version);
	return status;
}

// ---------------------------------------------------------------------------
NvencTap& NvencTap::Get(){
	static NvencTap instance;
	return instance;
}

void NvencTap::SetConfig(const NvencTapConfig &c){
	std::lock_guard<std::mutex> g(cfgLock);
	cfg = c;
}
NvencTapConfig NvencTap::GetConfig(){
	std::lock_guard<std::mutex> g(cfgLock);
	return cfg;
}
NvencTapStats NvencTap::GetStats(){
	std::lock_guard<std::mutex> g(statsLock);
	return stats;
}

void NvencTap::TryInstall(){
	if(installed.load(std::memory_order_relaxed) || failedPermanently.load(std::memory_order_relaxed)){ return; }
	double now = NowSecondsNv();
	if(now - lastAttempt < 1.0){ return; }
	lastAttempt = now;
	// pre-load the API DLL ourselves so the hook is in place before vrlink
	// (same process) loads it and calls CreateInstance at headset connect.
	// the handle is intentionally leaked: it must outlive the hook.
	HMODULE nv = GetModuleHandleA("nvEncodeAPI64.dll");
	if(!nv){ nv = LoadLibraryA("nvEncodeAPI64.dll"); }
	if(!nv){
		// no NVIDIA driver on this machine (AMD path uses AMF); retry a few
		// times in case the system DLL search is slow, then give up quietly
		static int misses = 0;
		if(++misses > 10){ failedPermanently.store(true); DriverLog("NvencTap: nvEncodeAPI64.dll not loadable — NVENC not present, tap idle"); }
		return;
	}
	void* target = (void*)GetProcAddress(nv, "NvEncodeAPICreateInstance");
	if(!target){
		failedPermanently.store(true);
		DriverLog("NvencTap: NvEncodeAPICreateInstance export not found — tap unavailable");
		return;
	}
	// MinHook may or may not have been initialised by another module yet
	MH_STATUS mi = MH_Initialize();
	if(mi != MH_OK && mi != MH_ERROR_ALREADY_INITIALIZED){
		failedPermanently.store(true);
		DriverLog("NvencTap: MH_Initialize failed (%d)", (int)mi);
		return;
	}
	MH_STATUS mh = MH_CreateHook(target, (void*)&NvencTapShims::CreateInstance, (void**)&origCreateInstance);
	if(mh != MH_OK && mh != MH_ERROR_ALREADY_CREATED){
		failedPermanently.store(true);
		DriverLog("NvencTap: MH_CreateHook failed (%d)", (int)mh);
		return;
	}
	if(MH_EnableHook(target) != MH_OK){
		failedPermanently.store(true);
		DriverLog("NvencTap: MH_EnableHook failed");
		return;
	}
	installed.store(true);
	DriverLog("NvencTap: installed on NvEncodeAPICreateInstance (nvEncodeAPI64.dll pre-loaded by us; encoders created from now on are wrapped)");
}

void NvencTap::MaybeHeartbeat(){
	double now = NowSecondsNv();
	if(now - lastHeartbeat < 10.0){ return; }
	lastHeartbeat = now;
	NvencTapStats s = GetStats();
	NvencTapConfig c = GetConfig();
	if(!installed.load() || (s.initCalls == 0 && s.reconfCalls == 0)){ return; }
	DriverLog("NvencTap: heartbeat api=%u enabled=%d %ux%u avg=%.0fM max=%.0fM level=%u tier=%u bitDepth=%u | init %u (ovr %u, ovrFail->ok %u, fail %u) | reconf %u (ovr %u, ovrFail->ok %u, fail %u, stockFail %u) | register %u encode %u",
		s.apiMajor, (int)c.enabled, s.lastWidth, s.lastHeight, s.lastAvgBitrate / 1e6, s.lastMaxBitrate / 1e6, s.lastLevel, s.lastTier, s.lastBitDepth,
		s.initCalls, s.initOverridden, s.initFailedThenPristineOk, s.initFailed,
		s.reconfCalls, s.reconfOverridden, s.reconfFailedThenPristineOk, s.reconfFailed, s.reconfFailedStock,
		s.registerCalls, s.encodeCalls);
	if(s.presetFetchOk || s.presetFetchFail){
		DriverLog("NvencTap: preset merge: fetch ok %u fail %u (cfgVer 0x%08x)", s.presetFetchOk, s.presetFetchFail, s.presetCfgVerUsed);
	}
	{
		NvencPostPackStats pp = NvencPostPack::GetStats();
		if(pp.frames || pp.skipped || pp.disabled){
			DriverLog("NvencPostPack: last 10s: processed %u skipped %u | pass mean=%.3fms max=%.3fms%s", pp.frames, pp.skipped,
				pp.frames ? pp.sumMs / pp.frames : 0.0, pp.maxMs, pp.disabled ? " | DISABLED (see earlier line)" : "");
			NvencPostPack::ResetIntervalStats();
		}
	}
	if(c.splitMode > 0 || s.splitState || s.sessionUpgrade){
		DriverLog("NvencTap: split-frame: mode %d state %s applied %u (status %u) engines=%u | session upgrade %s (status %u) retagged calls %u", c.splitMode,
			s.splitState == 1 ? "ACCEPTED" : s.splitState == 2 ? "REJECTED" : "untried", s.splitApplied, s.splitStatus, s.numEncoderEngines,
			s.sessionUpgrade == 1 ? "12.1 LIVE" : s.sessionUpgrade == 2 ? "REJECTED" : "not attempted", s.sessionUpgradeStatus, s.retagCalls);
	}
	if(s.ivReqCount || s.ivLatSamples){
		DriverLog("NvencTap: last 10s: vrlink asked avg=%.0fM min=%.0fM max=%.0fM (n=%u) | submit->lock mean=%.2fms max=%.1fms over-11.1ms=%u/%u (%.0f%%) | SUBMIT BLOCKED mean=%.2fms max=%.1fms over-10ms=%u/%u (vrlink watchdog resets on >10)",
			s.ivReqCount ? s.ivReqSumMbit / s.ivReqCount : 0.0, s.ivReqCount ? s.ivReqMinMbit : 0.0, s.ivReqMaxMbit, s.ivReqCount,
			s.ivLatSamples ? s.ivLatSumMs / s.ivLatSamples : 0.0, s.ivLatMaxMs, s.ivLatOver11, s.ivLatSamples,
			s.ivLatSamples ? 100.0 * s.ivLatOver11 / s.ivLatSamples : 0.0,
			s.ivSubmitSamples ? s.ivSubmitSumMs / s.ivSubmitSamples : 0.0, s.ivSubmitMaxMs, s.ivSubmitOver10, s.ivSubmitSamples);
		std::lock_guard<std::mutex> g(statsLock);
		stats.ivReqCount = 0; stats.ivReqSumMbit = 0; stats.ivReqMinMbit = 1e9; stats.ivReqMaxMbit = 0;
		stats.ivLatSamples = 0; stats.ivLatOver11 = 0; stats.ivLatSumMs = 0; stats.ivLatMaxMs = 0;
		stats.ivSubmitSamples = 0; stats.ivSubmitOver10 = 0; stats.ivSubmitSumMs = 0; stats.ivSubmitMaxMs = 0;
	}
	if(s.lockCalls){
		DriverLog("NvencTap: frames %u avg=%.0fKB max=%.2fMB maxIDR=%.2fMB idr=%u avgQP=%u(max %u) | size buckets <256K:%u <512K:%u <1M:%u <1.5M:%u <2M:%u <4M:%u >=4M:%u",
			s.lockCalls, s.sumFrameBytes / (double)s.lockCalls / 1024.0, s.maxFrameBytes / 1048576.0, s.maxFrameBytesIdr / 1048576.0, s.idrFrames,
			s.lastAvgQp, s.maxAvgQp, s.sizeBucket[0], s.sizeBucket[1], s.sizeBucket[2], s.sizeBucket[3], s.sizeBucket[4], s.sizeBucket[5], s.sizeBucket[6]);
	}
	if(s.latSamples){
		DriverLog("NvencTap: encode latency n=%u mean=%.2fms max=%.1fms | <5:%u <8:%u <11:%u(frame@90) <16:%u <33:%u >=33:%u",
			s.latSamples, s.latSumMs / s.latSamples, s.latMaxMs,
			s.latBucket[0], s.latBucket[1], s.latBucket[2], s.latBucket[3], s.latBucket[4], s.latBucket[5]);
	}
}
