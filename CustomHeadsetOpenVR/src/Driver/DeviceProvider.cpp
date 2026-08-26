#include "DeviceProvider.h"
#include "DriverLog.h"
#include "DriverLockout.h"
#include "DeviceShim.h"
#include "EyeTrackingTap.h"
#include "CompositorPlugin.h"
#include "HidModifier.h"

#include "Hooking/InterfaceHookInjector.h"

#include "../Headsets/MeganeX8K.h"
#include "../Headsets/GalaxyXR.h"
#include "../Headsets/DreamAir.h"
#include "../Headsets/GenericHeadset.h"
#include "../Headsets/FakeHeadset.h"
#include "../Helpers/EyeTrackingOutput.h"

#include "../Config/ConfigLoader.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

// set when this vendor-specific driver detected that the vendor-neutral
// CustomHeadsetOpenVR driver is enabled. all driver activity is skipped.
bool lockedOut = false;

// general driver functions
vr::EVRInitError CustomHeadsetDeviceProvider::Init(vr::IVRDriverContext *pDriverContext){
	// initialise this driver
	VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
	
	// discover the name this driver is registered under (CustomHeadsetOpenVR or GalaxyXRNative)
	// so resource paths and settings sections work in both neutral and vendor builds
	vr::DriverHandle_t driverHandle = vr::VRDriverHandle();
	std::string driverName;
	uint32_t driverCount = vr::VRDriverManager()->GetDriverCount();
	for(uint32_t i = 0; i < driverCount; i++){
		char name[128];
		vr::VRDriverManager()->GetDriverName(i, name, sizeof(name));
		if(vr::VRDriverManager()->GetDriverHandle(name) == driverHandle){
			driverName = name;
			break;
		}
	}
	if(driverName.empty()){
		driverName = driverConfigLoader.info.driverName;
		DriverLog("Could not discover driver name from handle, falling back to %s", driverName.c_str());
	}
	driverConfigLoader.info.driverName = driverName;
	
	char driverPath[2048];
	vr::VRResources()->GetResourceFullPath("", "", driverPath, sizeof(driverPath));
	driverConfigLoader.info.steamvrResources = driverPath;
	vr::VRResources()->GetResourceFullPath(("{" + driverName + "}").c_str(), "", driverPath, sizeof(driverPath));
	driverConfigLoader.info.driverResources = driverPath;
	
	DriverLog("Initializing %s", driverName.c_str());
	
	// Driver lockout: When this is a vendor-specific driver (not vendor-neutral),
	// check if the vendor-neutral driver (CustomHeadsetOpenVR) is enabled.
	// If the neutral driver is enabled, this vendor driver is locked out.
	#ifndef VENDOR_NEUTRAL
	DriverLog("Running in vendor-specific driver mode");
	if(IsNeutralDriverEnabled()){
		DriverLog("Vendor-specific driver locked out because the vendor-neutral driver (CustomHeadsetOpenVR) is enabled.");
		lockedOut = true;
		// still write info.json so the GUI can see this driver and offer the one-click switch
		try{
			std::filesystem::create_directories(driverConfigLoader.GetConfigFolder());
		}catch(const std::exception& e){
			DriverLog("Failed to create config folder while locked out: %s", e.what());
		}
		driverConfigLoader.WriteInfo();
		return vr::VRInitError_None;
	}
	#endif
	
	// write a setting so that the section of this driver is always defined in the settings file for other drivers to detect
	WriteHasBeenRunSetting(driverName.c_str());
	
	driverConfigLoader.Start();
	// inject hooks into functions
	InjectHooks(this, pDriverContext);
	hidModifier.InjectHooks();
	
	// arm the host hooks. the TrackedDeviceAdded/PoseUpdated hooks are only installed when a
	// driver requests IVRServerDriverHost through the hooked GetGenericInterface. drivers fetch
	// their host interface eagerly during their own init (VR_INIT_SERVER_DRIVER_CONTEXT ->
	// InitServer), so any driver that loaded before this one (e.g. vrlink) never triggers the
	// detour, and if no driver loads after this one the host hooks are never installed and no
	// devices get wrapped. requesting the interface here goes through the now hooked vtable and
	// installs the host hooks immediately, independent of driver load order.
	vr::EVRInitError hostHookError = vr::VRInitError_None;
	pDriverContext->GetGenericInterface(vr::IVRServerDriverHost_Version, &hostHookError);
	
	// arm the eye tracking tap hooks the same way. drivers that loaded before
	// this one (vrlink) may already hold a cached IVRDriverInput pointer, but
	// the hook patches the interface object's shared vtable, so requesting it
	// once here installs the CreateEyeTrackingComponent /
	// UpdateEyeTrackingComponent detours for every caller regardless of load
	// order.
	pDriverContext->GetGenericInterface(vr::IVRDriverInput_Version, &hostHookError);
	
	// the shim classes can be used to implement entirely new headsets, not just shim existing ones
	if(driverConfig.fakeHeadset.enable){
		FakeHeadset* fakeHeadsetImplementation = new FakeHeadset();
		fakeHeadsetImplementation->deviceProvider = this;
		shims.insert(fakeHeadsetImplementation);
		vr::ITrackedDeviceServerDriver* driver = new ShimTrackedDeviceDriver(fakeHeadsetImplementation, nullptr);
		vr::VRServerDriverHost()->TrackedDeviceAdded("FakeCustomHMD", vr::TrackedDeviceClass_HMD, driver);
	}
	
	return vr::VRInitError_None;
}
const char *const *CustomHeadsetDeviceProvider::GetInterfaceVersions(){
	return vr::k_InterfaceVersions;
}
bool CustomHeadsetDeviceProvider::ShouldBlockStandbyMode(){
	return false;
}
void CustomHeadsetDeviceProvider::Cleanup(){}
void CustomHeadsetDeviceProvider::EnterStandby(){}
void CustomHeadsetDeviceProvider::LeaveStandby(){}

void DebugEventLog(const vr::VREvent_t& vrevent){
	DriverLog("Event type: %d", vrevent.eventType);
	switch(vrevent.eventType){
		case vr::VREvent_PropertyChanged:
			DriverLog("Property changed: %i", vrevent.data.property.prop);
			break;
		case vr::VREvent_Compositor_DisplayReconnected:
			DriverLog("Compositor display reconnected");
			break;
		case vr::VREvent_ProcessConnected:
			DriverLog("Process connected %i", vrevent.data.process.pid);
			break;
	}
}

void CustomHeadsetDeviceProvider::RunFrame(){
	// when locked out by the vendor-neutral driver nothing was initialized, so do nothing
	if(lockedOut){
		return;
	}
	
	// acquire driverConfig.configLock for the duration of this function
	std::lock_guard<std::mutex> lock(driverConfigLock);
	
	hidModifier.RunFrame();
	
	#ifdef HAS_PRIVATE
	if(driverConfig.onlyHandlePrivateFunctionality){
		driverConfig.hasBeenUpdated = false;
		return;
	}
	#endif
		
	// process events that were submitted for this frame.
	vr::VREvent_t vrevent{};
	while(vr::VRServerDriverHost()->PollNextEvent(&vrevent, sizeof(vr::VREvent_t))){
		// DebugEventLog(vrevent);
		if(vrevent.eventType == VREvent_VendorSpecific_ContextCollection){
			// receive and store data from successful context collection events
			vr::VREvent_Reserved_t data = vrevent.data.reserved;
			if(data.reserved0 == VREvent_VendorSpecific_ContextCollection_MagicDataNumber){
				// add context based on the event data.
				uint32_t id = static_cast<uint32_t>(data.reserved1);
				vr::IVRDriverContext* ctx = (vr::IVRDriverContext*)data.reserved2;
				// logging here seems to deadlock on occasion
				// DriverLog("Received context collection event for device with ID: %d, Context: %p", id, ctx);	
				driverContextsByDeviceId[id] = ctx;
				// send any queued events
				if(queuedEvents.find(id) != queuedEvents.end()){
					for(const auto& event : queuedEvents[id]){
						SendVendorEvent(id, event.eventType, event.eventData, event.eventTimeOffset);
					}
					queuedEvents.erase(id);
				}
			}
		}
		if(vrevent.eventType == vr::VREvent_TrackedDeviceActivated){
			// set nonNativeHeadsetFound if a device with a direct mode component is found
			vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(vrevent.trackedDeviceIndex);
			if(container){
				// DriverLog("Device %d has driver direct mode component: %s", vrevent.trackedDeviceIndex, vr::VRProperties()->GetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool) ? "true" : "false");
				if(vr::VRProperties()->GetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool)){
					driverConfigLoader.info.nonNativeHeadsetFound = true;
					driverConfigLoader.WriteInfo();
				}
			}
		}
		if(vrevent.eventType == vr::VREvent_DashboardActivated){
			if(!driverConfigLoader.info.isDashboardOpen){
				driverConfigLoader.info.isDashboardOpen = true;
				driverConfigLoader.WriteInfo();
			}
		}
		if(vrevent.eventType == vr::VREvent_DashboardDeactivated){
			if(driverConfigLoader.info.isDashboardOpen){
				driverConfigLoader.info.isDashboardOpen = false;
				driverConfigLoader.WriteInfo();
			}
		}
		if(vrevent.eventType == vr::VREvent_ProcessConnected && customShaderEnabled){
			// check new processes and inject if they are the compositor
			InjectCompositorPlugin(vrevent.data.process.pid);
		}
		for(auto shim : shims){
			shim->HandleEvent(vrevent);
		}
	}
	for(auto shim : shims){
		if(shim->shimActive){
			shim->RunFrame();
		}
	}
	if(!customShaderEnabled && IsCustomShaderEnabled()){
		// try to inject when it is first enabled
		InjectCompositorPlugin();
		customShaderEnabled = true;
	}
	eyeTrackingOutput.RunFrame();
	// clear update flag at end of frame
	driverConfig.hasBeenUpdated = false;
}

void CustomHeadsetDeviceProvider::SendContextCollectionEvents(uint32_t id){
	for(auto driverContext : driverContexts){
		vr::EVRInitError eError = vr::VRInitError_None;
		vr::IVRServerDriverHost* VRServerDriverHost =  (vr::IVRServerDriverHost *)driverContext->GetGenericInterface(vr::IVRServerDriverHost_Version, &eError);
		// store data in event
		vr::VREvent_Data_t data = {VREvent_VendorSpecific_ContextCollection_MagicDataNumber, (uint64_t)id, (uint64_t)driverContext};
		// this event will only succeed for the driver that owns the id
		VRServerDriverHost->VendorSpecificEvent(id, VREvent_VendorSpecific_ContextCollection, data, 0);
	}
}

bool CustomHeadsetDeviceProvider::SendVendorEvent(uint32_t unWhichDevice, vr::EVREventType eventType, const vr::VREvent_Data_t & eventData, double eventTimeOffset){
	if(driverContextsByDeviceId.find(unWhichDevice) != driverContextsByDeviceId.end()){
		vr::EVRInitError eError = vr::VRInitError_None;
		vr::IVRServerDriverHost* VRServerDriverHost =  (vr::IVRServerDriverHost *)driverContextsByDeviceId[unWhichDevice]->GetGenericInterface(vr::IVRServerDriverHost_Version, &eError);
		VRServerDriverHost->VendorSpecificEvent(unWhichDevice, eventType, eventData, eventTimeOffset);
		return true;
	}else{
		// try to find context and queue for later
		SendContextCollectionEvents(unWhichDevice);
		if(queuedEvents.find(unWhichDevice) == queuedEvents.end()){
			queuedEvents[unWhichDevice] = {};
		}
		queuedEvents[unWhichDevice].push_back({eventType, eventData, eventTimeOffset});
		return false;
	}
}

static vr::HmdQuaternion_t QuatMultiply(const vr::HmdQuaternion_t &a, const vr::HmdQuaternion_t &b){
	vr::HmdQuaternion_t r;
	r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	return r;
}

static vr::HmdQuaternion_t QuatFromEulerDeg(const double deg[3]){
	// intrinsic x (pitch), then y (yaw), then z (roll), in the local frame
	double rx = deg[0] * 3.14159265358979323846 / 180.0 / 2.0;
	double ry = deg[1] * 3.14159265358979323846 / 180.0 / 2.0;
	double rz = deg[2] * 3.14159265358979323846 / 180.0 / 2.0;
	vr::HmdQuaternion_t qx = {cos(rx), sin(rx), 0, 0};
	vr::HmdQuaternion_t qy = {cos(ry), 0, sin(ry), 0};
	vr::HmdQuaternion_t qz = {cos(rz), 0, 0, sin(rz)};
	return QuatMultiply(QuatMultiply(qx, qy), qz);
}

static void QuatRotateVector(const vr::HmdQuaternion_t &q, const double v[3], double out[3]){
	// out = q * v * q^-1
	double tx = 2.0 * (q.y * v[2] - q.z * v[1]);
	double ty = 2.0 * (q.z * v[0] - q.x * v[2]);
	double tz = 2.0 * (q.x * v[1] - q.y * v[0]);
	out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
	out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
	out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

int CustomHeadsetDeviceProvider::GetDeviceClass(uint32_t openVRID){
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = deviceClasses.find(openVRID);
		if(found != deviceClasses.end()){
			return found->second;
		}
	}
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(openVRID);
	vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
	int deviceClass = vr::VRProperties()->GetInt32Property(container, vr::Prop_DeviceClass_Int32, &propError);
	if(propError != vr::TrackedProp_Success){
		deviceClass = (int)vr::TrackedDeviceClass_Invalid;
	}
	std::lock_guard<std::mutex> guard(poseLogLock);
	deviceClasses[openVRID] = deviceClass;
	return deviceClass;
}

bool CustomHeadsetDeviceProvider::GetHmdProjectionRaw(int eye, float &left, float &right, float &top, float &bottom){
	if(!hmdProjectionQueried){
		hmdProjectionQueried = true;
		if(hmdDevice){
			void* component = hmdDevice->GetComponent(vr::IVRDisplayComponent_Version);
			if(component){
				vr::IVRDisplayComponent* display = (vr::IVRDisplayComponent*)component;
				for(int e = 0; e < 2; e++){
					display->GetProjectionRaw((vr::EVREye)e,
						&hmdProjection[e][0], &hmdProjection[e][1],
						&hmdProjection[e][2], &hmdProjection[e][3]);
				}
				hmdProjectionValid = true;
				DriverLog("DeviceProvider: hmd projection raw L(l=%.4f r=%.4f t=%.4f b=%.4f) R(l=%.4f r=%.4f t=%.4f b=%.4f)",
					hmdProjection[0][0], hmdProjection[0][1], hmdProjection[0][2], hmdProjection[0][3],
					hmdProjection[1][0], hmdProjection[1][1], hmdProjection[1][2], hmdProjection[1][3]);
			}else{
				DriverLog("DeviceProvider: hmd has no IVRDisplayComponent, gaze mapping falls back to tangent knobs");
			}
		}
	}
	if(!hmdProjectionValid || eye < 0 || eye > 1){
		return false;
	}
	left = hmdProjection[eye][0];
	right = hmdProjection[eye][1];
	top = hmdProjection[eye][2];
	bottom = hmdProjection[eye][3];
	return true;
}

uint32_t CustomHeadsetDeviceProvider::ResolveContainerId(vr::PropertyContainerHandle_t container){
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = containerToId.find(container);
		if(found != containerToId.end()){
			return found->second;
		}
	}
	// containers are stable per device; probe the first few ids once
	for(uint32_t id = 0; id < 16; id++){
		if(vr::VRProperties()->TrackedDeviceToPropertyContainer(id) == container){
			std::lock_guard<std::mutex> guard(poseLogLock);
			containerToId[container] = id;
			return id;
		}
	}
	std::lock_guard<std::mutex> guard(poseLogLock);
	containerToId[container] = vr::k_unTrackedDeviceIndexInvalid;
	return vr::k_unTrackedDeviceIndexInvalid;
}

static bool InputPathInteresting(const std::string &lower){
	// anything that plausibly marks holding/releasing an object. session 8
	// taught us not to guess narrowly: 20 throws produced zero release
	// edges because the filter (and boolean-only hooking) missed vrlink's
	// actual grab control.
	if(lower.find("touch") != std::string::npos){
		return false;
	}
	return lower.find("grip") != std::string::npos
		|| lower.find("trigger") != std::string::npos
		|| lower.find("squeeze") != std::string::npos
		|| lower.find("grab") != std::string::npos
		|| lower.find("pinch") != std::string::npos;
}

// classify a component path into a distortion tuner control role. exact
// suffix matches against the confirmed vrlink surface (session log): joystick
// x/y scalars + joystick/a/b/x/y click booleans + grip value scalars.
static int TunerRoleForPath(const std::string &lower, bool isScalar){
	auto endsWith = [&](const char* suffix){
		size_t len = strlen(suffix);
		return lower.size() >= len && lower.compare(lower.size() - len, len, suffix) == 0;
	};
	if(isScalar){
		if(endsWith("/input/joystick/y")){ return 1; }
		if(endsWith("/input/grip/value")){ return 6; }
		if(endsWith("/input/joystick/x")){ return 7; }
		if(endsWith("/input/trigger/value")){ return 8; }
		return 0;
	}
	if(endsWith("/input/a/click")){ return 2; }
	if(endsWith("/input/b/click")){ return 3; }
	if(endsWith("/input/x/click")){ return 4; }
	if(endsWith("/input/y/click")){ return 5; }
	if(endsWith("/input/joystick/click")){ return 9; }
	return 0;
}

void CustomHeadsetDeviceProvider::OnInputComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle){
	if(!name || handle == vr::k_ulInvalidInputComponentHandle){
		return;
	}
	InputComponentInfo info;
	info.container = container;
	info.name = name;
	std::string lower = info.name;
	for(auto &c : lower){ c = (char)tolower(c); }
	info.interesting = InputPathInteresting(lower);
	info.tunerRole = TunerRoleForPath(lower, false);
	// hand classification from the quest layout: x/y buttons exist only on
	// the left controller, a/b only on the right. once known, resolve the
	// openVR id too so pose updates can be routed per hand.
	if(info.tunerRole >= 2 && info.tunerRole <= 5){
		int hand = (info.tunerRole == 2 || info.tunerRole == 3) ? 1 : 0;
		// resolve BEFORE taking poseLogLock: ResolveContainerId takes that
		// lock itself, and std::mutex is non-recursive — nesting it here
		// deadlocked vrserver at the first x/click creation and tripped a
		// SteamVR safe-mode block (session 24 regression)
		uint32_t id = ResolveContainerId(container);
		std::lock_guard<std::mutex> handGuard(poseLogLock);
		containerHand[container] = hand;
		if(id != vr::k_unTrackedDeviceIndexInvalid){
			openVRIDHand[id] = hand;
		}
	}
	// always log creates: component names are the map of vrlink's input
	// surface, and not having them cost a session
	DriverLog("InputTap: boolean component container=%llu path=%s handle=%llu%s",
		(unsigned long long)container, name, (unsigned long long)handle,
		info.interesting ? " [watched]" : "");
	std::lock_guard<std::mutex> guard(poseLogLock);
	inputComponents[handle] = info;
}

void CustomHeadsetDeviceProvider::OnScalarComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle){
	if(!name || handle == vr::k_ulInvalidInputComponentHandle){
		return;
	}
	InputComponentInfo info;
	info.container = container;
	info.name = name;
	info.isScalar = true;
	std::string lower = info.name;
	for(auto &c : lower){ c = (char)tolower(c); }
	info.interesting = InputPathInteresting(lower);
	info.tunerRole = TunerRoleForPath(lower, true);
	DriverLog("InputTap: scalar component container=%llu path=%s handle=%llu%s",
		(unsigned long long)container, name, (unsigned long long)handle,
		info.interesting ? " [watched]" : "");
	std::lock_guard<std::mutex> guard(poseLogLock);
	inputComponents[handle] = info;
}

void CustomHeadsetDeviceProvider::OnScalarComponentUpdated(vr::VRInputComponentHandle_t handle, float value){
	// distortion tuner capture: isolated fields so the tuner never disturbs
	// the release-forensics / velocity-fix state below, and gated by an
	// atomic so the hot path costs one relaxed load when the tuner is off
	if(tunerInputActive.load(std::memory_order_relaxed)){
		std::lock_guard<std::mutex> tunerGuard(poseLogLock);
		auto found = inputComponents.find(handle);
		if(found != inputComponents.end() && found->second.tunerRole != 0){
			found->second.tunerScalar = value;
		}
	}
	bool fixOn = driverConfig.streamFrame.velocityFixMode == 2;
	bool logOn = driverConfig.streamFrame.poseLogging;
	if(!fixOn && !logOn){
		return;
	}
	vr::PropertyContainerHandle_t container = 0;
	std::string name;
	bool release = false;
	bool gestureStart = false;
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = inputComponents.find(handle);
		if(found == inputComponents.end() || !found->second.interesting){
			return;
		}
		InputComponentInfo &info = found->second;
		float previous = info.lastScalar;
		info.lastScalar = value;
		// release GESTURE start: the scalar begins falling from its held
		// plateau — the finger starts opening. this precedes every game's
		// own release threshold, so anchoring the velocity output here
		// means whatever instant the game samples, it reads the throw.
		if(info.scalarPressed && previous > 0.85f && value < previous - 0.03f){
			gestureStart = true;
			container = info.container;
		}
		// hysteresis so analog grabbing (value based grips) produces clean
		// held/released edges: pressed above 0.6, released below 0.25
		if(!info.scalarPressed && value > 0.6f){
			info.scalarPressed = true;
		}else if(info.scalarPressed && value < 0.25f){
			info.scalarPressed = false;
			release = true;
			container = info.container;
			name = info.name;
		}
	}
	if(gestureStart && fixOn){
		uint32_t id = ResolveContainerId(container);
		if(id != vr::k_unTrackedDeviceIndexInvalid){
			AnchorReleaseGesture(id);
		}
	}
	if(release && logOn){
		LogReleaseSnapshot(container, name);
	}
}

void CustomHeadsetDeviceProvider::OnBooleanComponentUpdated(vr::VRInputComponentHandle_t handle, bool value){
	if(tunerInputActive.load(std::memory_order_relaxed)){
		std::lock_guard<std::mutex> tunerGuard(poseLogLock);
		auto found = inputComponents.find(handle);
		if(found != inputComponents.end() && found->second.tunerRole != 0){
			found->second.tunerBool = value;
		}
	}
	if(!driverConfig.streamFrame.poseLogging){
		return;
	}
	vr::PropertyContainerHandle_t container = 0;
	std::string name;
	bool release = false;
	bool edge = false;
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = inputComponents.find(handle);
		if(found == inputComponents.end()){
			return;
		}
		InputComponentInfo &info = found->second;
		bool changed = !info.haveValue || info.lastValue != value;
		bool wasHeld = info.haveValue && info.lastValue;
		info.haveValue = true;
		info.lastValue = value;
		if(!changed){
			return;
		}
		container = info.container;
		name = info.name;
		edge = true;
		release = info.interesting && wasHeld && !value;
	}
	if(release){
		LogReleaseSnapshot(container, name);
	}else if(edge){
		// low rate visibility of ALL boolean edges so the actual grab
		// control names itself in the log even if the watch filter misses
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		bool doLog = false;
		{
			std::lock_guard<std::mutex> guard(poseLogLock);
			if(now - lastEdgeLogTime >= 0.2){
				lastEdgeLogTime = now;
				doLog = true;
			}
		}
		if(doLog){
			DriverLog("InputTap: edge %s -> %d (id=%u)", name.c_str(), (int)value, ResolveContainerId(container));
		}
	}
}

void CustomHeadsetDeviceProvider::LogReleaseSnapshot(vr::PropertyContainerHandle_t container, const std::string &name){
	{
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		std::lock_guard<std::mutex> guard(poseLogLock);
		if(now - lastReleaseLogTime < 0.05){
			return; // 20Hz cap
		}
		lastReleaseLogTime = now;
	}
	uint32_t id = ResolveContainerId(container);
	// RELDIAG (kalman modes, poseLogging): the missing half of PEAKDIAG.
	// PEAKDIAG scores the estimator's PEAK tracking; the game reads the
	// velocity at the RELEASE INSTANT, which sits on the post-peak
	// downslope. this logs, at the tap's release edge: reported speed
	// at release, the peak over the previous 150ms of the history ring,
	// their ratio (1.0 = release got the full peak; low = the filter
	// already followed the hand's decel/recoil), the angle between the
	// release-instant and peak velocity vectors, and how long before
	// release the peak happened. high-J configs are expected to score
	// clean on PEAKDIAG and scatter here — this line adjudicates the
	// felt-vs-tracked J paradox directly.
	if(driverConfig.streamFrame.velocityFixMode >= 4
			&& driverConfig.streamFrame.poseLogging
			&& IsStreamedController(id)){
		double relSp = 0, pkSp = 0, relOff = 0, dtPkMs = 0, dtWPkMs = 0;
		double wRel = 0, wPk = 0;
		double relDirOff = -1, relAngOff = -1;
		bool haveRel = false;
		// SKEW instrument (2026-08-16): everything the release-instant
		// tuning rests on is the delay between the TRUE release (~ the
		// raw velocity peak) and the input release event's arrival. this
		// is that number, per throw, plus the release-instant output
		// scored against the RAW peak instead of the output's own peak:
		//   skew      = t(release edge) - t(raw secant peak, window center)
		//   rel/rawPk = release output speed / raw peak speed
		//   relRawDir = angle(release output v, raw peak v)
		//   skewW / wRel/rawWPk / relRawAng: the angular analogs
		// -1 = no raw peak within the last 500ms (release without a
		// throw, or the raw ring was reset by a reinit).
		double skewMs = -1, relOverRawPk = -1, relRawDir = -1, rawPkSpOut = 0, relOutSp = -1;
		double fdV[3] = {0, 0, 0}, fdSp = -1, fdOverRawPk = -1, fdRawDir = -1;
		double skewWMs = -1, wRelOverRawWPk = -1, relRawAng = -1, rawPkWSpOut = 0;
		double nowRel = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		{
			std::lock_guard<std::mutex> rdGuard(deriveFilterLock);
			KalState &krs = kalStates[id];
			// raw-referenced release score (raw peak time is the
			// receipt-clock center of its secant window; nowRel is the
			// receipt time of the input edge — same clock, so skew is
			// the delay as the driver, and hence the game, sees it)
			if(krs.repHave){
				relOutSp = sqrt(krs.repV[0] * krs.repV[0] + krs.repV[1] * krs.repV[1] + krs.repV[2] * krs.repV[2]);
			}
			// pose-history channel: finite difference of the SUBMITTED
			// position stream over ~33ms ending at the newest submit
			// (the estimator shape most XR throw code uses). fdOut =
			// its speed, fd/rawPk and fdRawDir score it against the raw
			// peak like relOut. this is the channel this game reads.
			if(krs.subCount > 3){
				int sNew = (krs.subHead - 1 + KalState::subN) % KalState::subN;
				int sOld = -1;
				for(int i = 1; i < krs.subCount; i++){
					int idx = (sNew - i + KalState::subN) % KalState::subN;
					if(krs.subT[sNew] - krs.subT[idx] >= 0.030){ sOld = idx; break; }
				}
				if(sOld >= 0){
					double span = krs.subT[sNew] - krs.subT[sOld];
					if(span > 0.01 && span < 0.2){
						for(int a2 = 0; a2 < 3; a2++){ fdV[a2] = (krs.subP[sNew][a2] - krs.subP[sOld][a2]) / span; }
						fdSp = sqrt(fdV[0] * fdV[0] + fdV[1] * fdV[1] + fdV[2] * fdV[2]);
						if(krs.rawPkSp > 0.5 && nowRel - krs.rawPkT < 0.5){
							fdOverRawPk = fdSp / krs.rawPkSp;
							if(fdSp > 0.5){
								double cf = (fdV[0] * krs.rawPkV[0] + fdV[1] * krs.rawPkV[1] + fdV[2] * krs.rawPkV[2]) / (fdSp * krs.rawPkSp);
								if(cf > 1.0){ cf = 1.0; }
								if(cf < -1.0){ cf = -1.0; }
								fdRawDir = acos(cf) * 180.0 / 3.14159265358979323846;
							}
						}
					}
				}
			}
			if(krs.rawPkSp > 0.5 && nowRel - krs.rawPkT < 0.5 && nowRel - krs.rawPkT > -0.1){
				skewMs = (nowRel - krs.rawPkT) * 1000.0;
				rawPkSpOut = krs.rawPkSp;
				if(krs.repHave){
					double so = 0, d = 0;
					for(int a2 = 0; a2 < 3; a2++){
						so += krs.repV[a2] * krs.repV[a2];
						d += krs.repV[a2] * krs.rawPkV[a2];
					}
					relOverRawPk = sqrt(so) / krs.rawPkSp;
					if(so > 0.25){
						double c = d / (sqrt(so) * krs.rawPkSp);
						if(c > 1.0){ c = 1.0; }
						if(c < -1.0){ c = -1.0; }
						relRawDir = acos(c) * 180.0 / 3.14159265358979323846;
					}
				}
			}
			if(krs.rawPkWSp > 2.0 && nowRel - krs.rawPkWT < 0.5 && nowRel - krs.rawPkWT > -0.1){
				skewWMs = (nowRel - krs.rawPkWT) * 1000.0;
				rawPkWSpOut = krs.rawPkWSp;
				if(krs.repHave){
					double swo = 0, dw = 0;
					for(int a2 = 0; a2 < 3; a2++){
						swo += krs.repW[a2] * krs.repW[a2];
						dw += krs.repW[a2] * krs.rawPkW[a2];
					}
					wRelOverRawWPk = sqrt(swo) / krs.rawPkWSp;
					if(swo > 1.0){
						double cw = dw / (sqrt(swo) * krs.rawPkWSp);
						if(cw > 1.0){ cw = 1.0; }
						if(cw < -1.0){ cw = -1.0; }
						relRawAng = acos(cw) * 180.0 / 3.14159265358979323846;
					}
				}
			}
			// release-instant direction vs displacement truth: the Td/O
			// adjudicator (relOffPk below is blind to any direction
			// shaping — it compares two post-shaping vectors). -1 =
			// no valid snapshot or speeds below the direction floor.
			if(krs.relSnapHave){
				double so = 0, ss = 0, d = 0;
				double swo = 0, sws = 0, dw = 0;
				for(int a2 = 0; a2 < 3; a2++){
					so += krs.relOutV[a2] * krs.relOutV[a2];
					ss += krs.relSecV[a2] * krs.relSecV[a2];
					d += krs.relOutV[a2] * krs.relSecV[a2];
					swo += krs.relOutW[a2] * krs.relOutW[a2];
					sws += krs.relSecW[a2] * krs.relSecW[a2];
					dw += krs.relOutW[a2] * krs.relSecW[a2];
				}
				if(so > 1.0 && ss > 1.0){
					double c = d / sqrt(so * ss);
					if(c > 1.0){ c = 1.0; }
					if(c < -1.0){ c = -1.0; }
					relDirOff = acos(c) * 180.0 / 3.14159265358979323846;
				}
				if(swo > 4.0 && sws > 4.0){
					double cw = dw / sqrt(swo * sws);
					if(cw > 1.0){ cw = 1.0; }
					if(cw < -1.0){ cw = -1.0; }
					relAngOff = acos(cw) * 180.0 / 3.14159265358979323846;
				}
			}
			if(krs.histCount > 2){
				int newest = (krs.histHead - 1 + KalState::histSize) % KalState::histSize;
				double tNow = krs.histT[newest];
				double relV[3] = { krs.histV[newest][0], krs.histV[newest][1], krs.histV[newest][2] };
				relSp = sqrt(relV[0]*relV[0] + relV[1]*relV[1] + relV[2]*relV[2]);
				wRel = sqrt(krs.histW[newest][0]*krs.histW[newest][0]
					+ krs.histW[newest][1]*krs.histW[newest][1]
					+ krs.histW[newest][2]*krs.histW[newest][2]);
				double pkV[3] = { relV[0], relV[1], relV[2] };
				double tPk = tNow;
				double tWPk = tNow;
				for(int i = 0; i < krs.histCount; i++){
					int idx = (krs.histHead - 1 - i + 2 * KalState::histSize) % KalState::histSize;
					if(tNow - krs.histT[idx] > 0.15){ break; }
					double sp = sqrt(krs.histV[idx][0]*krs.histV[idx][0]
						+ krs.histV[idx][1]*krs.histV[idx][1]
						+ krs.histV[idx][2]*krs.histV[idx][2]);
					if(sp > pkSp){
						pkSp = sp;
						tPk = krs.histT[idx];
						for(int a2 = 0; a2 < 3; a2++){ pkV[a2] = krs.histV[idx][a2]; }
					}
					double wsp = sqrt(krs.histW[idx][0]*krs.histW[idx][0]
						+ krs.histW[idx][1]*krs.histW[idx][1]
						+ krs.histW[idx][2]*krs.histW[idx][2]);
					if(wsp > wPk){ wPk = wsp; tWPk = krs.histT[idx]; }
				}
				dtPkMs = (tNow - tPk) * 1000.0;
				dtWPkMs = (tWPk - tPk) * 1000.0;
				double d = relV[0]*pkV[0] + relV[1]*pkV[1] + relV[2]*pkV[2];
				if(relSp > 1e-3 && pkSp > 1e-3){
					double c = d / (relSp * pkSp);
					if(c > 1.0){ c = 1.0; }
					if(c < -1.0){ c = -1.0; }
					relOff = acos(c) * 180.0 / 3.14159265358979323846;
				}
				haveRel = relSp > 0.5 || pkSp > 1.0;
			}
			if(skewMs >= 0){ haveRel = true; }
		}
		// log outside the lock; bounded by the caller's 20Hz cap
		if(haveRel){
			DriverLog("PoseLog: RELDIAG id=%u mode=%d rel=%.2f pk150=%.2f rel/pk=%.2f relOffPk=%.1fdeg relDirOff=%.1fdeg relAngOff=%.1fdeg dtPk=%.0fms dtWPk=%.0fms wRel=%.1f wPk=%.1f relOut=%.2f skew=%.0fms rawPk=%.2f rel/rawPk=%.2f relRawDir=%.1fdeg fdOut=%.2f fd/rawPk=%.2f fdRawDir=%.1fdeg skewW=%.0fms rawWPk=%.1f wRel/rawWPk=%.2f relRawAng=%.1fdeg",
				id, driverConfig.streamFrame.velocityFixMode, relSp, pkSp,
				pkSp > 0.01 ? relSp / pkSp : 0.0, relOff, relDirOff, relAngOff, dtPkMs, dtWPkMs, wRel, wPk,
				relOutSp, skewMs, rawPkSpOut, relOverRawPk, relRawDir, fdSp, fdOverRawPk, fdRawDir, skewWMs, rawPkWSpOut, wRelOverRawWPk, relRawAng);
		}
	}
	// release latch trigger: arm the peak replay for this device the moment
	// the input tap reports the release. identity resolved ABOVE, outside
	// any lock; deriveFilterLock taken alone here (leaf, never nested)
	// EXPERIMENT B trigger: on release, arm the kalman rewind window
	if(driverConfig.streamFrame.velocityFixMode == 4
			&& driverConfig.streamFrame.kalmanReleaseRewindMs > 0.5
			&& IsStreamedController(id)){
		double nowRw = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		double holdS = driverConfig.streamFrame.kalmanRewindHoldMs / 1000.0;
		if(holdS < 0.02){ holdS = 0.02; }
		{
			std::lock_guard<std::mutex> rwGuard(deriveFilterLock);
			KalState &ksr = kalStates[id];
			ksr.rewindUntil = nowRw + holdS;
			ksr.rewindTarget = nowRw - driverConfig.streamFrame.kalmanReleaseRewindMs / 1000.0;
		}
		// outside the lock; bounded by the caller's release throttle
		DriverLog("VelocityFix: kalman rewind armed id=%u rewind=%.0fms hold=%.0fms",
			id, driverConfig.streamFrame.kalmanReleaseRewindMs, driverConfig.streamFrame.kalmanRewindHoldMs);
	}
	if(driverConfig.streamFrame.velocityFixMode == 3
			&& driverConfig.streamFrame.deriveReleaseLatch
			&& IsStreamedController(id)){
		double nowLatch = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		double holdS = driverConfig.streamFrame.deriveLatchHoldMs / 1000.0;
		if(holdS < 0.02){ holdS = 0.02; }
		{
			std::lock_guard<std::mutex> latchGuard(deriveFilterLock);
			deriveFilterStates[id].latchUntil = nowLatch + holdS;
		}
		// engagement confirmation, rate-limited by the caller's 20Hz release
		// throttle above; outside all locks
		DriverLog("VelocityFix: latch armed id=%u hold=%.0fms", id, holdS * 1000.0);
	}
	MotionSnapshot snap;
	bool haveSnap = false;
	double snapAge = -1;
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = motionSnapshots.find(id);
		if(found != motionSnapshots.end()){
			snap = found->second;
			haveSnap = true;
			double now = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
			snapAge = (now - snap.time) * 1000.0;
		}
	}
	if(haveSnap){
		DriverLog("ReleaseSnap: id=%u %s released: out=(%.3f, %.3f, %.3f) |out|=%.3f ang=(%.2f, %.2f, %.2f) trackingOk=%d result=%d snapAge=%.1fms",
			id, name.c_str(),
			snap.outVel[0], snap.outVel[1], snap.outVel[2], snap.outSpeed,
			snap.outAng[0], snap.outAng[1], snap.outAng[2],
			(int)snap.trackingOk, snap.result, snapAge);
	}else{
		DriverLog("ReleaseSnap: id=%u %s released: no motion snapshot yet", id, name.c_str());
	}
}

void CustomHeadsetDeviceProvider::OnPoseComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle){
	if(!name || handle == vr::k_ulInvalidInputComponentHandle){
		return;
	}
	// always log: gaze published as a pose component would be exactly the
	// "openvr paths" channel DFR tools bind (AngelDark report)
	DriverLog("InputTap: pose component container=%llu path=%s handle=%llu",
		(unsigned long long)container, name, (unsigned long long)handle);
	PoseComponentInfo info;
	info.container = container;
	info.name = name;
	// tip components: resolve which HAND this container's tip belongs to
	// from the container's own controller-role property (vrlink puts tip
	// poses on the paired hand devices, not the button controllers, so
	// button-derived hand maps can't associate them). resolved OUTSIDE
	// poseLogLock: property queries must never run under our lock.
	std::string nameStr = name;
	if(nameStr.size() >= 9 && nameStr.compare(nameStr.size() - 9, 9, "/pose/tip") == 0){
		vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
		int32_t role = vr::VRProperties()->GetInt32Property(container,
			vr::Prop_ControllerRoleHint_Int32, &propError);
		int hand = -1;
		if(propError == vr::TrackedProp_Success){
			if(role == vr::TrackedControllerRole_LeftHand){ hand = 0; }
			if(role == vr::TrackedControllerRole_RightHand){ hand = 1; }
		}
		DriverLog("InputTap: /pose/tip container=%llu role=%d -> hand=%s",
			(unsigned long long)container, (int)role,
			hand == 0 ? "LEFT" : (hand == 1 ? "RIGHT" : "UNKNOWN (aligner tip marker unavailable for it)"));
		if(hand >= 0){
			std::lock_guard<std::mutex> tipGuard(poseLogLock);
			containerTipHand[container] = hand;
		}
	}
	std::lock_guard<std::mutex> guard(poseLogLock);
	poseComponents[handle] = info;
}

// skeleton tap: track vrlink's skeletal components and offset the wrist
// bone (bone 1, root-relative) by the configured amount. this shifts the
// whole skeletal hand relative to its anchor while the device pose, render
// model, components and the grip pivot all stay put - the one degree of
// freedom nothing else reaches. hot: values read per update.
static std::mutex skeletonTapMutex;
static std::map<vr::VRInputComponentHandle_t, int> skeletonTapHands;

void CustomHeadsetDeviceProvider::OnSkeletonComponentCreated(vr::PropertyContainerHandle_t container, const char *name, const char *skeletonPath, vr::VRInputComponentHandle_t handle){
	std::string path = skeletonPath ? skeletonPath : "";
	int hand = path.find("right") != std::string::npos ? 1 : 0;
	{
		std::lock_guard<std::mutex> lock(skeletonTapMutex);
		skeletonTapHands[handle] = hand;
	}
	DriverLog("SkeletonTap: component %s (%s) hand=%s handle=%llu", name ? name : "?", path.c_str(), hand ? "right" : "left", (unsigned long long)handle);
}

bool CustomHeadsetDeviceProvider::HandleSkeletonUpdate(vr::VRInputComponentHandle_t handle, const vr::VRBoneTransform_t *bones, uint32_t count, vr::VRBoneTransform_t *outBones){
	double x = driverConfig.galaxyXr.skeletonOffsetXCm * 0.01;
	double y = driverConfig.galaxyXr.skeletonOffsetYCm * 0.01;
	double z = driverConfig.galaxyXr.skeletonOffsetZCm * 0.01;
	if(x == 0.0 && y == 0.0 && z == 0.0){
		return false;
	}
	int hand;
	{
		std::lock_guard<std::mutex> lock(skeletonTapMutex);
		auto it = skeletonTapHands.find(handle);
		if(it == skeletonTapHands.end()){
			return false;
		}
		hand = it->second;
	}
	if(hand == 1 && driverConfig.galaxyXr.skeletonOffsetMirror){
		x = -x;
	}
	for(uint32_t i = 0; i < count; i++){
		outBones[i] = bones[i];
	}
	outBones[1].position.v[0] += (float)x;
	outBones[1].position.v[1] += (float)y;
	outBones[1].position.v[2] += (float)z;
	return true;
}

void CustomHeadsetDeviceProvider::OnPoseComponentUpdated(vr::VRInputComponentHandle_t handle, const vr::HmdMatrix34_t* offset, double timeOffset){
	double now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
	std::string name;
	uint64_t updates = 0;
	bool doLog = false;
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = poseComponents.find(handle);
		if(found == poseComponents.end()){
			return;
		}
		// tip offset capture for the controller aligner: /pose/tip is the
		// controller-local tip transform vrlink itself publishes
		if(offset && found->second.name.size() >= 9
				&& found->second.name.compare(found->second.name.size() - 9, 9, "/pose/tip") == 0){
			auto handFound = containerTipHand.find(found->second.container);
			if(handFound != containerTipHand.end()){
				AlignControllerState &state = alignControllers[handFound->second];
				state.tipValid = true;
				state.tipLocal[0] = offset->m[0][3];
				state.tipLocal[1] = offset->m[1][3];
				state.tipLocal[2] = offset->m[2][3];
			}
		}
		found->second.updates++;
		// first update always, then 1 per 5s per component
		if(found->second.updates == 1 || now - found->second.lastLogTime >= 5.0){
			found->second.lastLogTime = now;
			name = found->second.name;
			updates = found->second.updates;
			doLog = true;
		}
	}
	if(doLog && offset){
		DriverLog("InputTap: pose component %s update %llu offset=(%.4f, %.4f, %.4f) fwd=(%.4f, %.4f, %.4f) timeOffset=%.4f",
			name.c_str(), (unsigned long long)updates,
			offset->m[0][3], offset->m[1][3], offset->m[2][3],
			-offset->m[0][2], -offset->m[1][2], -offset->m[2][2],
			timeOffset);
	}
}

void CustomHeadsetDeviceProvider::AnchorReleaseGesture(uint32_t openVRID){
	double now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
	std::lock_guard<std::mutex> guard(poseLogLock);
	VelFixState &state = velFixStates[openVRID];
	state.anchorTime = now;
	state.anchorHasValue = false; // next pose update seeds it
}

// ---- constant-acceleration (Singer) per-axis kalman helpers ----
// state [p, v, a] with white-jerk process noise (sigmaJ, m/s^3) and an
// exponential decay of the acceleration state toward zero (beta =
// exp(-dt/tau)): pure CA at tau -> inf; the decay is what bounds phantom
// integration across dup coasts and abrupt stops. covariance layout:
// [P00 P01 P02 P11 P12 P22] (symmetric upper triangle).
static void CaStatePredict(double dt, double tau, double &p, double &v, double &a){
	// exact Singer discretization: the acceleration decays DURING the
	// interval, so position/velocity integrate its true average
	// a * (tau/dt)(1 - e^(-dt/tau)) rather than the full initial value.
	// for dt << tau this matches the naive form; for the long-dt case
	// (a gap of missed samples resuming with a hot accel state) the
	// naive form applies the whole stale acceleration across the whole
	// gap and can overshoot position by a meter — the field-observed
	// "hand sits wrong for a moment after a throw" transient.
	if(dt <= 0){ return; }
	double e = exp(-dt / tau);
	double aAvg = a * (tau / dt) * (1.0 - e);
	p += v * dt + 0.5 * aAvg * dt * dt;
	v += aAvg * dt;
	a *= e;
}
static void CaCovPredict(double dt, double sigmaJ, double tau, double beta, bool exactCov, double P[6]){
	double q = sigmaJ * sigmaJ;
	double dt2 = dt * dt, dt3 = dt2 * dt, dt4 = dt3 * dt, dt5 = dt4 * dt;
	double P00 = P[0], P01 = P[1], P02 = P[2], P11 = P[3], P12 = P[4], P22 = P[5];
	if(exactCov){
		// consistency pass (A/B knob kalmanCaExactCov): propagate the
		// covariance with the SAME transition CaStatePredict implements —
		// F12 = tau(1 - e^(-dt/tau)) (exact Singer velocity gain) and
		// F02 = dt*F12/2 (the implemented conservative position gain) —
		// instead of the naive dt / dt^2/2. at tau near the 20ms floor
		// with ~8-11ms dt the naive form overstates how much accel
		// uncertainty flows into v/p by up to ~25%, over-weighting
		// measurements relative to the model. Q is intentionally kept in
		// the naive white-jerk form in BOTH branches (second order in
		// dt/tau) so the A/B isolates the transition alone.
		double gv = tau * (1.0 - beta);
		double gp = 0.5 * dt * gv;
		double A0 = P00 + dt * P01 + gp * P02;
		double A1 = P01 + dt * P11 + gp * P12;
		double A2 = P02 + dt * P12 + gp * P22;
		double B1 = P11 + gv * P12;
		double B2 = P12 + gv * P22;
		P[0] = A0 + dt * A1 + gp * A2 + q * dt5 / 20.0;
		P[1] = A1 + gv * A2 + q * dt4 / 8.0;
		P[2] = beta * A2 + q * dt3 / 6.0;
		P[3] = B1 + gv * B2 + q * dt3 / 3.0;
		P[4] = beta * B2 + q * dt2 / 2.0;
		P[5] = beta * beta * P22 + q * dt;
		return;
	}
	double h = 0.5 * dt2;
	P[0] = P00 + 2.0 * dt * P01 + 2.0 * h * P02 + dt2 * P11 + 2.0 * dt * h * P12 + h * h * P22 + q * dt5 / 20.0;
	P[1] = P01 + dt * P11 + h * P12 + dt * P02 + dt2 * P12 + dt * h * P22 + q * dt4 / 8.0;
	P[2] = beta * (P02 + dt * P12 + h * P22) + q * dt3 / 6.0;
	P[3] = P11 + 2.0 * dt * P12 + dt2 * P22 + q * dt3 / 3.0;
	P[4] = beta * (P12 + dt * P22) + q * dt2 / 2.0;
	P[5] = beta * beta * P22 + q * dt;
}
// scalar position-measurement update; y is the innovation. returns this
// axis' normalized innovation squared contribution (NIS telemetry). for
// the angular MEKF the "p" slot is a zero-seeded error scratch whose
// post-update value IS the orientation correction (K0 * residual).
static double CaUpdate(double y, double R, double &p, double &v, double &a, double P[6]){
	double S = P[0] + R;
	double K0 = P[0] / S, K1 = P[1] / S, K2 = P[2] / S;
	p += K0 * y; v += K1 * y; a += K2 * y;
	double P00 = P[0], P01 = P[1], P02 = P[2];
	P[0] = (1.0 - K0) * P00; P[1] = (1.0 - K0) * P01; P[2] = (1.0 - K0) * P02;
	P[3] = P[3] - K1 * P01; P[4] = P[4] - K1 * P02; P[5] = P[5] - K2 * P02;
	return y * y / S;
}
static void CaInit(double P[6], double p0Var, double v0Var, double a0Var){
	P[0] = p0Var; P[1] = 0; P[2] = 0; P[3] = v0Var; P[4] = 0; P[5] = a0Var;
}

bool CustomHeadsetDeviceProvider::HandleDevicePoseUpdated(uint32_t openVRID, vr::DriverPose_t &pose){
	// raw tracking status, captured BEFORE forceTracking can launder it.
	// the estimators gate on these: forceTracking's job is keeping
	// devices alive for SteamVR, not feeding fake-OK into filters.
	// (field 2026-08-12: vrlink zero-fills result during hard tracking
	// losses — 99% correlation with frozen payloads — so this is a live
	// loss flag, not a formality.)
	const bool rawPoseValid = pose.poseIsValid;
	const int rawResult = (int)pose.result;
	if(driverConfig.forceTracking){
		pose.poseIsValid = true;
		if(pose.result != vr::TrackingResult_Fallback_RotationOnly){
			pose.result = vr::TrackingResult_Running_OK;
		}
	}
	// every controller pose edit below (grip convention, shared offsets,
	// per-hand trims) is for the streamed Galaxy XR controllers only. a
	// native pair (Index, Vive) reports its own correct grip and must never
	// be shifted (field 2026-08-25: knuckles users saw the 22 deg / 5 cm
	// convention shift). same serial gate the velocity fix uses; cached
	// after the first read, false while the property is not readable yet.
	// property query with no lock held.
	// galaxyXr.controllerBypass: pose left as vrlink sent it (Kalman is
	// applied further down and is not part of the bypass).
	const bool streamedController = openVRID != vr::k_unTrackedDeviceIndex_Hmd
		&& !driverConfig.galaxyXr.controllerBypass
		&& GetDeviceClass(openVRID) == (int)vr::TrackedDeviceClass_Controller
		&& IsStreamedController(openVRID);
	#ifdef VENDOR_GALAXYXR
	// fixed raw->grip convention shift for the Galaxy XR controllers (see
	// GalaxyXrConfig::gripConvention): applied before the user's personal
	// trim offsets so those keep meaning small corrections. the grip-family
	// render model components (handgrip/openxr_grip/grip) are identity so
	// every pose path resolves to this same frame - do not re-add a grip
	// offset there.
	if(driverConfig.galaxyXr.gripConvention && streamedController){
		static const double kGripConventionRotDeg[3] = {22, 0, 0};
		double fixLocal[3] = {0, 0, 0.05};
		double fixWorld[3];
		QuatRotateVector(pose.qRotation, fixLocal, fixWorld);
		pose.vecPosition[0] += fixWorld[0];
		pose.vecPosition[1] += fixWorld[1];
		pose.vecPosition[2] += fixWorld[2];
		pose.qRotation = QuatMultiply(pose.qRotation, QuatFromEulerDeg(kGripConventionRotDeg));
	}
	#endif
	// controller pose offsets: local frame rotation and translation, applied
	// before velocity derivation so the ring tracks the adjusted origin.
	// (a config change mid-session moves the origin once; the teleport guard
	// resets the ring and the moment passes.)
	const ControllersConfig &controllersConfig = driverConfig.controllers;
	double rotationOffsetDeg[3];
	double positionOffsetCm[3];
	if(alignerOverrideActive.load(std::memory_order_relaxed)){
		// aligner working offsets replace the configured ones, so stick
		// edits and pivot solves are visible in the very next pose
		std::lock_guard<std::mutex> alignGuard(poseLogLock);
		for(int i = 0; i < 3; i++){
			rotationOffsetDeg[i] = alignerRotDeg[i];
			positionOffsetCm[i] = alignerPosCm[i];
		}
	}else{
		for(int i = 0; i < 3; i++){
			rotationOffsetDeg[i] = controllersConfig.rotationOffsetDeg[i];
			positionOffsetCm[i] = controllersConfig.positionOffsetCm[i];
		}
	}
	bool hasRotationOffset = rotationOffsetDeg[0] != 0
		|| rotationOffsetDeg[1] != 0 || rotationOffsetDeg[2] != 0;
	bool hasPositionOffset = positionOffsetCm[0] != 0
		|| positionOffsetCm[1] != 0 || positionOffsetCm[2] != 0;
	if((hasRotationOffset || hasPositionOffset) && streamedController){
		// mirror the left-hand-authored offsets for the right controller:
		// physical pairs are mirror images, so the tracked-origin-to-grip
		// displacement mirrors too (position X and rotation Y/Z negate)
		if(controllersConfig.mirrorOffsetsForRightHand){
			int hand = -1;
			{
				std::lock_guard<std::mutex> handGuard(poseLogLock);
				auto handFound = openVRIDHand.find(openVRID);
				if(handFound != openVRIDHand.end()){
					hand = handFound->second;
				}
			}
			if(hand == 1){
				positionOffsetCm[0] = -positionOffsetCm[0];
				rotationOffsetDeg[1] = -rotationOffsetDeg[1];
				rotationOffsetDeg[2] = -rotationOffsetDeg[2];
			}
		}
		if(hasPositionOffset){
			double local[3] = {
				positionOffsetCm[0] / 100.0,
				positionOffsetCm[1] / 100.0,
				positionOffsetCm[2] / 100.0,
			};
			double world[3];
			QuatRotateVector(pose.qRotation, local, world);
			pose.vecPosition[0] += world[0];
			pose.vecPosition[1] += world[1];
			pose.vecPosition[2] += world[2];
		}
		if(hasRotationOffset){
			pose.qRotation = QuatMultiply(pose.qRotation, QuatFromEulerDeg(rotationOffsetDeg));
		}
		if(alignerOverrideActive.load(std::memory_order_relaxed)){
			std::lock_guard<std::mutex> logGuard(poseLogLock);
			if(!alignerAppliedLogged){
				alignerAppliedLogged = true;
				DriverLog("Aligner: working offsets APPLYING to device id=%u (rot %.1f,%.1f,%.1f deg pos %.2f,%.2f,%.2f cm)",
					openVRID, rotationOffsetDeg[0], rotationOffsetDeg[1], rotationOffsetDeg[2],
					positionOffsetCm[0], positionOffsetCm[1], positionOffsetCm[2]);
			}
		}
	}
	// per-hand unmirrored trims (ControllersConfig::left*/right*): applied
	// after the shared mirrored offsets, same local-frame convention.
	if(streamedController){
		int hand = -1;
		{
			std::lock_guard<std::mutex> handGuard(poseLogLock);
			auto handFound = openVRIDHand.find(openVRID);
			if(handFound != openVRIDHand.end()){
				hand = handFound->second;
			}
		}
		const double* handRot = nullptr;
		const double* handPos = nullptr;
		if(hand == 0){
			handRot = driverConfig.controllers.leftRotationOffsetDeg;
			handPos = driverConfig.controllers.leftPositionOffsetCm;
		}else if(hand == 1){
			handRot = driverConfig.controllers.rightRotationOffsetDeg;
			handPos = driverConfig.controllers.rightPositionOffsetCm;
		}
		if(handRot && handPos){
			if(handPos[0] != 0 || handPos[1] != 0 || handPos[2] != 0){
				double local[3] = {handPos[0] / 100.0, handPos[1] / 100.0, handPos[2] / 100.0};
				double world[3];
				QuatRotateVector(pose.qRotation, local, world);
				pose.vecPosition[0] += world[0];
				pose.vecPosition[1] += world[1];
				pose.vecPosition[2] += world[2];
			}
			if(handRot[0] != 0 || handRot[1] != 0 || handRot[2] != 0){
				double rot[3] = {handRot[0], handRot[1], handRot[2]};
				pose.qRotation = QuatMultiply(pose.qRotation, QuatFromEulerDeg(rot));
			}
		}
	}
	// capture the post-offset pose per hand for the controller aligner (the
	// drawn tip marker must reflect the live working offsets)
	if(openVRID == vr::k_unTrackedDeviceIndex_Hmd && pose.poseIsValid){
		// cache head orientation for the gaze aim assist (leaf lock)
		std::lock_guard<std::mutex> hmdGuard(deriveFilterLock);
		hmdQuatForGaze = pose.qRotation;
		haveHmdQuat = true;
	}
	if(openVRID != vr::k_unTrackedDeviceIndex_Hmd && pose.poseIsValid){
		std::lock_guard<std::mutex> alignGuard(poseLogLock);
		auto handFound = openVRIDHand.find(openVRID);
		if(handFound != openVRIDHand.end()){
			AlignControllerState &state = alignControllers[handFound->second];
			state.poseValid = true;
			state.pos[0] = pose.vecPosition[0];
			state.pos[1] = pose.vecPosition[1];
			state.pos[2] = pose.vecPosition[2];
			state.rot = pose.qRotation;
			state.poseTime = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		}
	}
	// throw/velocity fix: substitute position-derived velocity when it is
	// meaningfully larger than the driver's smoothed report, so throw
	// releases carry true peak speed. cheap unsynchronized bool reads keep
	// the hot path free when both features are disabled.
	int velocityFixMode = driverConfig.streamFrame.velocityFixMode;
	// flagged-loss bookkeeping (kalman mode): the estimator gate below
	// skips flagged samples entirely, so the filter never eats them —
	// this pre-block records loss runs for KALDIAG and pins ks.have =
	// false on the first OK sample, forcing the clean reinit path even
	// for flagged losses shorter than the 200ms dt threshold. the
	// reported pose passes through untouched during loss: the device
	// zero-fills its own velocity while lost (measured), so the hand
	// parks instead of rocketing.
	if(velocityFixMode >= 4 && openVRID != vr::k_unTrackedDeviceIndex_Hmd
			&& IsStreamedController(openVRID)){
		bool trackingOk = rawPoseValid && rawResult == (int)vr::TrackingResult_Running_OK;
		double lossNow = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		std::lock_guard<std::mutex> lossGuard(deriveFilterLock);
		KalState &lks = kalStates[openVRID];
		if(!trackingOk){
			if(!lks.lost){
				lks.lost = true;
				lks.lossStartT = lossNow;
				lks.lossRuns++;
				if(driverConfig.streamFrame.poseLogging){
					DriverLog("PoseLog: KALLOSS id=%u lost (result=%d valid=%d)",
						openVRID, rawResult, (int)rawPoseValid);
				}
			}
			// flagged-loss coast: report the state predicted forward for
			// a bounded window instead of the frozen raw pose. STATELESS:
			// predicted from the last committed state each callback
			// (horizon = lossNow - lks.time), nothing is written back,
			// so nothing accumulates and reacquire's clean reinit is
			// untouched. Singer decay bounds a stale hot acceleration
			// across the horizon; velocities are reported from the same
			// prediction so the runtime and pose-history games see one
			// coherent coasted state, not a frozen hand with zero v.
			double coastWin = driverConfig.streamFrame.kalmanLossCoastMs / 1000.0;
			if(coastWin > 1.0){ coastWin = 1.0; }
			if(coastWin > 0.0005 && lks.have
					&& lossNow - lks.lossStartT <= coastWin){
				double dtL = lossNow - lks.time;
				if(dtL > 0 && dtL <= coastWin + 0.05){
					double tauL = driverConfig.streamFrame.kalmanCaAccelTauMs / 1000.0;
					if(tauL < 0.02){ tauL = 0.02; }
					if(tauL > 10.0){ tauL = 10.0; }
					bool caL = velocityFixMode == 6;
					double pL[3], vL[3], wL[3];
					double eL = exp(-dtL / tauL);
					double aFacL = (tauL / dtL) * (1.0 - eL);
					for(int a2 = 0; a2 < 3; a2++){
						pL[a2] = lks.p[a2];
						vL[a2] = lks.v[a2];
						if(caL){
							double aC = lks.ca[a2];
							CaStatePredict(dtL, tauL, pL[a2], vL[a2], aC);
							wL[a2] = lks.w[a2] + lks.caW[a2] * aFacL * dtL;
						}else{
							pL[a2] += vL[a2] * dtL;
							wL[a2] = lks.w[a2];
						}
					}
					double hL = 0.5 * dtL;
					// CA: integrate orientation by the MIDPOINT angular
					// velocity including the decaying angular acceleration,
					// matching the dup-coast branch (harmonized 2026-08-14;
					// previously integrated by the stale entry w only).
					// midpoint = w + caW*aFac*dt/2 = (w + wL)/2. stateless
					// like the rest of this block: nothing written back.
					vr::HmdQuaternion_t dqL = caL
						? vr::HmdQuaternion_t{1.0,
							0.5 * (lks.w[0] + wL[0]) * hL,
							0.5 * (lks.w[1] + wL[1]) * hL,
							0.5 * (lks.w[2] + wL[2]) * hL}
						: vr::HmdQuaternion_t{1.0, lks.w[0] * hL, lks.w[1] * hL, lks.w[2] * hL};
					vr::HmdQuaternion_t qL = QuatMultiply(dqL, lks.q);
					double qnL = sqrt(qL.w * qL.w + qL.x * qL.x + qL.y * qL.y + qL.z * qL.z);
					if(qnL > 1e-9){ qL.w /= qnL; qL.x /= qnL; qL.y /= qnL; qL.z /= qnL; }
					for(int a2 = 0; a2 < 3; a2++){
						pose.vecPosition[a2] = pL[a2];
						pose.vecVelocity[a2] = vL[a2];
						pose.vecAngularVelocity[a2] = wL[a2];
					}
					pose.qRotation = qL;
					pose.poseIsValid = true;
					pose.result = vr::TrackingResult_Running_OK;
				}
			}
		}else if(lks.lost){
			lks.lost = false;
			double lm = (lossNow - lks.lossStartT) * 1000.0;
			lks.lossMsSum += lm;
			// A short flagged loss is missing measurement time, not a new
			// trajectory.  The state was deliberately left committed at the
			// last good measurement while the loss reporter coasted it
			// statelessly.  Keep that state: the normal Kalman path below will
			// predict once across the complete device-time gap and assimilate
			// this first fresh position with the covariance grown by that gap.
			//
			// Only abandon the derivative state after a loss longer than the
			// configured coast horizon.  This avoids the old FOV-reacquire
			// failure where a 8-100 ms hole zeroed v/a exactly at release.
			double cw = driverConfig.streamFrame.kalmanLossCoastMs;
			bool longLoss = cw <= 0.0 || lm > cw;
			if(longLoss){
				lks.have = false;
				lks.reacqCheck = false;
				lks.reacqActive = false;
				lks.reacqCount = 0;
			}else{
				// Preserve the derivative state, but do not blindly trust the
				// first Running_OK position.  Source handoffs can return good
				// status with a discontinuous coordinate solution.
				lks.reacqCheck = true;
			}
			if(driverConfig.streamFrame.poseLogging){
				double coasted = lm < cw ? lm : cw;
				DriverLog("PoseLog: KALLOSS id=%u reacquired after %.0fms (coasted %.0fms) -> %s",
					openVRID, lm, coasted, longLoss ? "reinit" : "resume-state");
			}
		}
	}
	if(velocityFixMode > 0 && openVRID != vr::k_unTrackedDeviceIndex_Hmd
			&& rawPoseValid && rawResult == (int)vr::TrackingResult_Running_OK
			&& IsStreamedController(openVRID)){
		bool classicMode = velocityFixMode == 1;
		bool deriveMode = velocityFixMode == 3;
		double derivedVel[3], derivedAng[3];
		double secantVel[3] = {0, 0, 0}, secantAng[3] = {0, 0, 0};
		// runtime's own report, captured before any substitution: its
		// magnitude is heavily smoothed but its DIRECTION comes from
		// device-side sensor fusion and is a candidate direction source
		double runtimeVel[3] = { pose.vecVelocity[0], pose.vecVelocity[1], pose.vecVelocity[2] };
		double runtimeAng[3] = { pose.vecAngularVelocity[0], pose.vecAngularVelocity[1], pose.vecAngularVelocity[2] };
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		// ==== KALMAN mode: one coherent estimated state, reported whole.
		// replicates the native lighthouse ARCHITECTURE: pose, velocity and
		// angular velocity all come from a single causal estimator, so the
		// runtime's forward prediction and game-side pose-history throw
		// estimators agree by construction. per-axis constant-velocity
		// Kalman for p/v; orientation integrated by the filtered w and
		// corrected by the measurement residual (MEKF-lite). ====
		if(velocityFixMode >= 4){
			// CA experiment arms: caM (mode 5) swaps only the fast
			// magnitude channel for a CA estimator; caFull (mode 6)
			// replaces the whole state (linear AND angular) with CA.
			const bool caM = velocityFixMode == 5;
			const bool caFull = velocityFixMode == 6;
			// RTS: this callback's PREDICTED linear state/cov (captured in
			// the step branches; the filtered one is read at the ring
			// push after every state modification of the callback)
			bool rtsStep = false;
			double rtsPredX[3][3] = {};
			double rtsPredP[3][6] = {};
			// gaze fetched BEFORE the filter lock (never call out under a
			// lock); freshness guarded 100ms like the frame consumer
			bool gazeFresh = false;
			double gazeHead[3] = {0, 0, -1};
			if(driverConfig.streamFrame.kalmanGazeAssist > 0.001){
				EyeTrackingTap::Sample gs;
				if(eyeTrackingTap.GetLatestSample(gs, 0.1) && gs.valid){
					double gx = gs.targetX - gs.originX;
					double gy = gs.targetY - gs.originY;
					double gz = gs.targetZ - gs.originZ;
					double gn = sqrt(gx * gx + gy * gy + gz * gz);
					if(gn > 1e-6){
						gazeHead[0] = gx / gn; gazeHead[1] = gy / gn; gazeHead[2] = gz / gn;
						gazeFresh = true;
					}
				}
			}
			// grip-point compensator knob resolution, BEFORE the filter
			// lock (openVRIDHand and the aligner grip override live under
			// poseLogLock — never nested under deriveFilterLock). unknown
			// hand = no compensation for that device, honestly.
			bool gripEnable = driverConfig.streamFrame.kalmanGripEnable;
			double gripBlend = driverConfig.streamFrame.kalmanGripBlend;
			if(gripBlend < 0.0){ gripBlend = 0.0; }
			if(gripBlend > 2.0){ gripBlend = 2.0; }
			double gripLocal[3] = {0, 0, 0};
			bool gripHave = false;
			{
				int gripHand = -1;
				{
					std::lock_guard<std::mutex> handGuard(poseLogLock);
					auto handIt = openVRIDHand.find(openVRID);
					if(handIt != openVRIDHand.end()){ gripHand = handIt->second; }
					if(gripHand == 0 || gripHand == 1){
						if(alignerGripActive.load(std::memory_order_relaxed)){
							for(int i = 0; i < 3; i++){
								gripLocal[i] = alignerGripCm[gripHand][i] / 100.0;
							}
						}else{
							const double* cm = gripHand == 0
								? driverConfig.streamFrame.kalmanGripLeftCm
								: driverConfig.streamFrame.kalmanGripRightCm;
							for(int i = 0; i < 3; i++){ gripLocal[i] = cm[i] / 100.0; }
						}
					}
				}
				double rMag = sqrt(gripLocal[0] * gripLocal[0]
					+ gripLocal[1] * gripLocal[1] + gripLocal[2] * gripLocal[2]);
				// 1mm floor: below it the compensation is numerically
				// meaningless and the shadow instrumentation just repeats
				// the main channel. 30cm cap: a wildly wrong r is worse
				// than none (w=20 rad/s at 0.3m fabricates 6 m/s).
				gripHave = rMag > 0.001 && rMag < 0.3;
			}
			double qa = driverConfig.streamFrame.kalmanProcessAccel;
			if(qa < 1.0){ qa = 1.0; }
			if(qa > 2000.0){ qa = 2000.0; }
			double rp = driverConfig.streamFrame.kalmanPosNoiseMm / 1000.0;
			if(rp < 0.0002){ rp = 0.0002; }
			double R = rp * rp;
			double qaA = driverConfig.streamFrame.kalmanProcessAngAccel;
			if(qaA < 10.0){ qaA = 10.0; }
			if(qaA > 20000.0){ qaA = 20000.0; }
			double ro = driverConfig.streamFrame.kalmanOriNoiseDeg * 3.14159265358979323846 / 180.0;
			if(ro < 0.0005){ ro = 0.0005; }
			double Ra = ro * ro;
			double lead = driverConfig.streamFrame.kalmanLeadMs / 1000.0;
			if(lead < 0){ lead = 0; }
			if(lead > 0.05){ lead = 0.05; }
			// CA knob resolution. CA-full reads its OWN noise knobs so
			// tuning it never disturbs the CV mode's field values.
			if(caFull){
				rp = driverConfig.streamFrame.kalmanCaPosNoiseMm / 1000.0;
				if(rp < 0.0002){ rp = 0.0002; }
				R = rp * rp;
				ro = driverConfig.streamFrame.kalmanCaOriNoiseDeg * 3.14159265358979323846 / 180.0;
				if(ro < 0.0005){ ro = 0.0005; }
				Ra = ro * ro;
			}
			double caJ = driverConfig.streamFrame.kalmanCaJerk;
			// floor lowered 10 -> 1 after the first field session pinned
			// J at the old clamp: at low jerk the CA filter degrades
			// gracefully into "very smooth CV + slow accel tracker",
			// which is a legitimate corner of the tuning space
			if(caJ < 1.0){ caJ = 1.0; }
			if(caJ > 50000.0){ caJ = 50000.0; }
			double caJA = driverConfig.streamFrame.kalmanCaAngJerk;
			if(caJA < 50.0){ caJA = 50.0; }
			if(caJA > 500000.0){ caJA = 500000.0; }
			double caTau = driverConfig.streamFrame.kalmanCaAccelTauMs / 1000.0;
			if(caTau < 0.02){ caTau = 0.02; }
			if(caTau > 10.0){ caTau = 10.0; }
			double caMagJ = driverConfig.streamFrame.kalmanCaMagJerk;
			if(caMagJ < 1.0){ caMagJ = 1.0; }
			if(caMagJ > 50000.0){ caMagJ = 50000.0; }
			double caMagTau = driverConfig.streamFrame.kalmanCaMagAccelTauMs / 1000.0;
			if(caMagTau < 0.02){ caMagTau = 0.02; }
			if(caMagTau > 10.0){ caMagTau = 10.0; }
			// covariance transition consistency A/B (see CaCovPredict)
			bool caExactCov = driverConfig.streamFrame.kalmanCaExactCov;
			bool adaptR = driverConfig.streamFrame.kalmanAdaptiveR;
			double adaptMax = driverConfig.streamFrame.kalmanAdaptiveRMaxDiv;
			if(adaptMax < 1.0){ adaptMax = 1.0; }
			if(adaptMax > 100.0){ adaptMax = 100.0; }
			bool announceKalman = false;
			bool announceGaze = false;
			bool logKalDiag = false;
			// STUCKDIAG edge flags (log outside the lock)
			bool stuckEnterLog = false;
			bool stuckExitLog = false;
			double stuckLogDiv = 0, stuckLogMs = 0, stuckLogMax = 0, stuckLogV0 = 0;
			// corrupt-payload gate log flags (log outside the lock)
			bool logGarbage = false;
			double gbPos[3] = {0, 0, 0};
			double gbQn2 = 0;
			double diagNis = 0;
			double diagStepMax = 0;
			int diagFrozen = 0;
			int diagDup = 0;
			int diagP3d = 0;
			int diagBends = 0;
			int diagTurnSteps = 0;
			double diagBendMean = 0;
			double diagBendMax = 0;
			int diagDtBack = 0;
			double diagDtMean = 0;
			double diagDtMax = 0;
			double diagCoastMax = 0;
			double diagANis = 0;
			double diagFdtMean = 0;
			double diagFdtMax = 0;
			double diagAccMax = 0;
			double diagWAccMax = 0;
			int diagAccNZ = 0;
			int diagLossRuns = 0;
			double diagLossMs = 0;
			int diagTeleports = 0;
			int diagGarbage = 0;
			int diagVClamp = 0;
			int diagRtsFrames = 0, diagRtsRep = 0;
			double diagRtsDepth = 0;
			double diagRDiv = 1.0;
			double diagRADiv = 1.0;
			double diagCaAcc = 0;
			double diagCaWAcc = 0;
			// Epoch/state observability.  These are copied while the Kalman
			// lock is held and emitted with KALDIAG outside the lock.
			double diagRawPos[3] = {0, 0, 0};
			double diagStateP[3] = {0, 0, 0};
			double diagStateV[3] = {0, 0, 0};
			double diagStateA[3] = {0, 0, 0};
			double diagSubmitP[3] = {0, 0, 0};
			double diagTMeas = 0;
			double diagOffsetIn = 0;
			double diagOffsetOut = 0;
			int diagFresh = 0;
			double diagSignedDirDeg = 0;
			bool diagSignedDirValid = false;
			double diagPosSigma = 0;
			double diagVelSigma = 0;
			double diagAccSigma = 0;
			// Observe-only angular-space / Td probe.  No value below is fed
			// back into the estimator or report path.
			bool logSpace = false;
			bool diagQRateValid = false;
			double diagQDtMs = 0;
			double diagSrcW[3] = {
				pose.vecAngularVelocity[0],
				pose.vecAngularVelocity[1],
				pose.vecAngularVelocity[2]
			};
			// Raw upstream linear velocity probe.  This is captured before
			// the Kalman report path overwrites DriverPose_t::vecVelocity.
			// It is OBSERVE-ONLY in this patch; the next field run decides
			// whether it contains enough information to become a very noisy
			// velocity measurement during position-blind motion.
			double diagSrcV[3] = {
				pose.vecVelocity[0], pose.vecVelocity[1], pose.vecVelocity[2]
			};
			double diagFdV[3] = {0, 0, 0};
			double diagKalV[3] = {0, 0, 0};
			int diagP3dNow = 0;
			int diagReacqNow = 0;
			double diagQWorldW[3] = {0, 0, 0};
			double diagQBodyW[3] = {0, 0, 0};
			double diagKalW[3] = {0, 0, 0};
			double diagPreTdV[3] = {0, 0, 0};
			double diagPostTdV[3] = {0, 0, 0};
			double diagLocalX[3] = {0, 0, 0};
			double diagLocalY[3] = {0, 0, 0};
			double diagLocalZ[3] = {0, 0, 0};
			double diagCurveW[3] = {0, 0, 0};
			double diagSrcWorldDeg = -1;
			double diagSrcBodyDeg = -1;
			double diagKalWorldDeg = -1;
			double diagKalBodyDeg = -1;
			double diagCurveVsKalDeg = -1;
			double diagTdCmdDeg = 0;
			double diagTdBendDeg = 0;
			double diagSrcFdDeg = -1;
			double diagSrcKalDeg = -1;
			double diagSrcFdMagRatio = -1;
			bool diagFdTrusted = false;
			// WorldFromDriver velocity-space probe. Observe-only: score the raw
			// upstream linear velocity in all three plausible spaces against the
			// corrected fresh-position secant. The existing spaceVelocityFixMode
			// runs after the Kalman output; this shadow probe never changes pose.
			vr::HmdQuaternion_t diagQwd = pose.qWorldFromDriverRotation;
			double diagQwdAngleDeg = 0;
			double diagSrcVQwd[3] = {0, 0, 0};
			double diagSrcVQwdInv[3] = {0, 0, 0};
			double diagSrcQwdFdDeg = -1;
			double diagSrcQwdInvFdDeg = -1;
			// device-time measurement stamp: the device says WHEN this
			// pose was true (poseTimeOffset); the filter previously
			// treated every sample as "now" — timing is the proven
			// pathology of this platform. sanity: an offset beyond
			// 100ms is not believed (falls back to receipt time).
			bool devTime = driverConfig.streamFrame.kalmanDeviceTime;
			double tOff = pose.poseTimeOffset;
			if(tOff < -0.1 || tOff > 0.1){ tOff = 0; }
			double tMeas = devTime ? now + tOff : now;
			{
			std::lock_guard<std::mutex> kalGuard(deriveFilterLock);
			KalState &ks = kalStates[openVRID];
			// Raw quaternion finite-difference in BOTH conventions:
			//   q1*q0^-1 -> world/driver-space angular velocity
			//   q0^-1*q1 -> body/controller-local angular velocity
			// The source vecAngularVelocity is logged against both.  This
			// directly tests the report that the upstream runtime may be
			// supplying angular velocity in the wrong coordinate space.
			//
			// Use an orientation-only freshness clock.  Position duplicate
			// detection is intentionally irrelevant here because GxR can
			// keep q live while p is frozen.
			{
				vr::HmdQuaternion_t qNow = pose.qRotation;
				double qNowN = sqrt(qNow.w * qNow.w + qNow.x * qNow.x
					+ qNow.y * qNow.y + qNow.z * qNow.z);
				if(qNowN > 1e-9){
					qNow.w /= qNowN; qNow.x /= qNowN;
					qNow.y /= qNowN; qNow.z /= qNowN;
				}
				if(!ks.diagRawQHave){
					ks.diagRawQHave = true;
					ks.diagRawQ[0] = qNow.w; ks.diagRawQ[1] = qNow.x;
					ks.diagRawQ[2] = qNow.y; ks.diagRawQ[3] = qNow.z;
					ks.diagRawQT = tMeas;
				}else{
					double dotQ = ks.diagRawQ[0] * qNow.w
						+ ks.diagRawQ[1] * qNow.x
						+ ks.diagRawQ[2] * qNow.y
						+ ks.diagRawQ[3] * qNow.z;
					double absDotQ = fabs(dotQ);
					if(absDotQ > 1.0){ absDotQ = 1.0; }
					// Ignore exact/repeated orientation payloads.  Align the
					// quaternion hemisphere before differencing so a q/-q
					// representation flip cannot masquerade as 360 degrees.
					if(1.0 - absDotQ > 1e-10){
						if(dotQ < 0){
							qNow.w = -qNow.w; qNow.x = -qNow.x;
							qNow.y = -qNow.y; qNow.z = -qNow.z;
						}
						double qdt = tMeas - ks.diagRawQT;
						if(qdt > 0.001 && qdt < 0.100){
							vr::HmdQuaternion_t qPrev = {
								ks.diagRawQ[0], ks.diagRawQ[1],
								ks.diagRawQ[2], ks.diagRawQ[3]
							};
							vr::HmdQuaternion_t qPrevInv = {
								qPrev.w, -qPrev.x, -qPrev.y, -qPrev.z
							};
							vr::HmdQuaternion_t dqWorld = QuatMultiply(qNow, qPrevInv);
							vr::HmdQuaternion_t dqBody = QuatMultiply(qPrevInv, qNow);
							auto deltaToRate = [&](vr::HmdQuaternion_t dq, double outW[3]){
								if(dq.w < 0){
									dq.w = -dq.w; dq.x = -dq.x;
									dq.y = -dq.y; dq.z = -dq.z;
								}
								double sv = sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
								if(sv > 1e-12){
									double ang = 2.0 * atan2(sv, dq.w);
									double sc = ang / (sv * qdt);
									outW[0] = dq.x * sc;
									outW[1] = dq.y * sc;
									outW[2] = dq.z * sc;
								}else{
									outW[0] = 0; outW[1] = 0; outW[2] = 0;
								}
							};
							deltaToRate(dqWorld, diagQWorldW);
							deltaToRate(dqBody, diagQBodyW);
							diagQDtMs = qdt * 1000.0;
							diagQRateValid = true;
						}
						ks.diagRawQ[0] = qNow.w; ks.diagRawQ[1] = qNow.x;
						ks.diagRawQ[2] = qNow.y; ks.diagRawQ[3] = qNow.z;
						ks.diagRawQT = tMeas;
					}
				}
			}
			auto vecAngleDeg = [](const double a[3], const double b[3]){
				double am = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
				double bm = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
				if(am < 1e-6 || bm < 1e-6){ return -1.0; }
				double d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (am * bm);
				if(d < -1.0){ d = -1.0; }
				if(d > 1.0){ d = 1.0; }
				return acos(d) * 180.0 / 3.14159265358979323846;
			};
			// switching between CV and CA modes mid-session invalidates
			// the carried covariances (different layouts) — clean reinit
			if(ks.lastMode != velocityFixMode){
				ks.lastMode = velocityFixMode;
				ks.have = false;
				ks.haveFast = false;
				ks.reacqCheck = false;
				ks.reacqActive = false;
				ks.reacqCount = 0;
			}
			// adaptive measurement trust: divide R/Ra by the fast scheduler
			// statistic, bounded. baseline (schedNis << 1) pins the divisor
			// at 1 = bit-identical to off; the BASE values are kept for the
			// scheduler's own normalization below. dup-soft inflation
			// stacks multiplicatively on top, unchanged.
			double RbaseSched = R;
			double RaBaseSched = Ra;
			// hoisted: dup detection below may need to UNDO the division
			// for repeat samples (a repeat must never be trusted more
			// than soft-dedup says, see the dupHit soft branch)
			double rDivApplied = 1.0;
			double raDivApplied = 1.0;
			if(adaptR){
				double divL = ks.schedNis;
				if(divL < 1.0){ divL = 1.0; }
				if(divL > adaptMax){ divL = adaptMax; }
				double divA = ks.schedANis;
				if(divA < 1.0){ divA = 1.0; }
				if(divA > adaptMax){ divA = adaptMax; }
				R /= divL;
				Ra /= divA;
				rDivApplied = divL;
				raDivApplied = divA;
			}
			// acceleration field probe: raw stream, before any
			// accept/drop decision (we are probing what vrlink SENDS,
			// not what the filter uses)
			{
				double am = sqrt(pose.vecAcceleration[0] * pose.vecAcceleration[0]
					+ pose.vecAcceleration[1] * pose.vecAcceleration[1]
					+ pose.vecAcceleration[2] * pose.vecAcceleration[2]);
				double wm = sqrt(pose.vecAngularAcceleration[0] * pose.vecAngularAcceleration[0]
					+ pose.vecAngularAcceleration[1] * pose.vecAngularAcceleration[1]
					+ pose.vecAngularAcceleration[2] * pose.vecAngularAcceleration[2]);
				if(am > ks.accMax){ ks.accMax = am; }
				if(wm > ks.wAccMax){ ks.wAccMax = wm; }
				if(am > 1e-9 || wm > 1e-9){ ks.accNZ++; }
			}
			double dt = devTime ? tMeas - ks.tMeas : now - ks.time;
			bool dropSample = false;
			// corrupt-payload sanity gate (field 2026-08-14): vrlink
			// delivers occasional position garbage (~4e18m, sub-frame-dt
			// bursts) FLAGGED Running_OK — the loss gate never sees it.
			// a non-finite or out-of-playspace measurement is not a
			// measurement: reject it BEFORE dt/dup/teleport/state ever
			// touch it. rejected samples take the existing drop path
			// (state, clocks, lastMeas untouched; reported pose holds
			// the last filtered state), so the guard can never reinit
			// at garbage and the accept path can never innovate across
			// it. bursts longer than 200ms reinit naturally at the next
			// good sample via the dt > 0.2 path. 50m bound: generous
			// for any real playspace, far below the garbage class.
			{
				bool garbage = false;
				for(int gI = 0; gI < 3; gI++){
					double pc = pose.vecPosition[gI];
					if(!std::isfinite(pc) || pc > 50.0 || pc < -50.0){ garbage = true; }
				}
				double qn2g = pose.qRotation.w * pose.qRotation.w
					+ pose.qRotation.x * pose.qRotation.x
					+ pose.qRotation.y * pose.qRotation.y
					+ pose.qRotation.z * pose.qRotation.z;
				if(!std::isfinite(qn2g) || qn2g < 0.25 || qn2g > 4.0){ garbage = true; }
				if(garbage){
					dropSample = true;
					ks.garbageN++;
					if(!ks.garbageRun){
						ks.garbageRun = true;
						if(driverConfig.streamFrame.poseLogging){
							logGarbage = true;
							gbPos[0] = pose.vecPosition[0];
							gbPos[1] = pose.vecPosition[1];
							gbPos[2] = pose.vecPosition[2];
							gbQn2 = qn2g;
						}
					}
				}else{
					ks.garbageRun = false;
				}
			}
			if(!dropSample && devTime && ks.have && dt <= 0 && dt > -0.2){
				// out-of-order on the device clock: this sample is OLDER
				// than the state. it carries no new information — drop
				// it. never reinit here: zeroing velocity mid-throw on a
				// late packet is exactly the failure the old dt<=0
				// reinit would produce once device time is in play.
				dropSample = true;
				ks.dtBack++;
			}
			// duplicate detection, BEFORE any clock or state commit: in
			// drop mode a detected repeat is treated as never having
			// arrived, so ks.tMeas must stay at the last ACCEPTED
			// measurement — the next real sample then predicts across
			// the full accumulated device-time gap in one honest step.
			// (committing the clock here would under-advance that
			// prediction and re-introduce the stale-stillness drag.)
			int dupMode = driverConfig.streamFrame.kalmanDupMode;
			bool dupHit = false;
			// dupRepeat = this sample IS a gate-qualifying repeat (frozen
			// payload while the state moves), independent of whether the
			// run cap still down-weights it. the run clock must be keyed
			// on THIS, not on dupHit (bug fix 2026-08-14: past the cap a
			// repeat has dupHit=false, and the old !dupHit reset restarted
			// the run — so a sustained post-throw freeze alternated one
			// full-weight sample per ~cap of re-inflated ones, stretching
			// the velocity kill ~10x. field symptom: hand parked ~1m out
			// for 0.5-2s after hard throws. the cap's own rationale says a
			// repeat sustained past it IS stillness — so every repeat past
			// the cap must stay full weight until a FRESH sample arrives.)
			bool dupRepeat = false;
			bool posOnlyFreeze = false;
			if(!dropSample && ks.have && dt > 0 && dt <= 0.2 && dupMode != 0 && ks.haveMeas){
				double ddx = pose.vecPosition[0] - ks.lastMeas[0];
				double ddy = pose.vecPosition[1] - ks.lastMeas[1];
				double ddz = pose.vecPosition[2] - ks.lastMeas[2];
				double stepD = sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
				double stSpd = sqrt(ks.v[0] * ks.v[0] + ks.v[1] * ks.v[1] + ks.v[2] * ks.v[2]);
				// 3dof-fallback classifier (field 2026-08-16: hand parked
				// ~1m out for ~0.5s while still ROTATING with the wrist;
				// 66 windows with fresh gaps >400ms against a 90ms cap,
				// reacquire teleports 0.8-2.4m). a frozen position with a
				// moving quaternion is the tracker's position-only loss,
				// not a still hand: the quaternion in the SAME payload is
				// the proof of motion. 0.2deg/sample ~ 18deg/s at stream
				// cadence, an order of magnitude above orientation noise.
				if(stepD < 0.0003 && driverConfig.streamFrame.kalmanPosFreeze3dof){
					double qd = pose.qRotation.w * ks.lastMeasQ[0]
						+ pose.qRotation.x * ks.lastMeasQ[1]
						+ pose.qRotation.y * ks.lastMeasQ[2]
						+ pose.qRotation.z * ks.lastMeasQ[3];
					if(qd < 0){ qd = -qd; }
					if(qd > 1.0){ qd = 1.0; }
					if(2.0 * acos(qd) > 0.0035){
						// no state-speed gate: a slow (aiming) hand with a
						// frozen position and live rotation is the same
						// tracker event and deserves the same protection
						posOnlyFreeze = true;
						ks.posFreeze3dof++;
						// velocity decay during position-blindness (field
						// 2026-08-16 out-of-FOV windups): coasting on the
						// occlusion-entry velocity sails the hand up to 2m
						// out (measured maxDiv 1.99m) and reacquires with a
						// wrong-DIRECTION velocity state — worse for a
						// following throw than the old park-at-stale, which
						// at least restarted from v=0. real windups
						// decelerate; decay v toward zero so short freezes
						// coast nearly untouched (mid-throw, <100ms) and
						// long occlusions glide to a stop near the loss
						// point. 0 disables (pure coast).
						double vdMs = driverConfig.streamFrame.kalmanPosFreezeVelDecayMs;
						if(vdMs > 0){
							if(vdMs < 20.0){ vdMs = 20.0; }
							if(vdMs > 2000.0){ vdMs = 2000.0; }
							double vDecay = exp(-dt / (vdMs / 1000.0));
							for(int vi = 0; vi < 3; vi++){
								ks.v[vi] *= vDecay;
								ks.vF[vi] *= vDecay;
							}
						}
					}
				}
				// age mode: a frozen payload is a repeat at ANY state speed
				// (its distrust scales with |v|*age, so a still hand's
				// repeats are simply believed) — the 0.5 m/s gate exists
				// only for the fixed-scale modes
				bool ageMode = dupMode == 4;
				if(stepD < 0.0003 && (stSpd > 0.5 || ageMode)){
					dupRepeat = true;
					// run cap (bug fix, caught live: NIS 570 for 8+s).
					// skipping repeats blocks the very measurements that
					// update the speed this gate tests, so an abrupt
					// stop could skip forever on stale velocity. a
					// repeat sustained past the cap IS stillness:
					// process it normally. applies to coast AND drop.
					double coastMax = driverConfig.streamFrame.kalmanDupCoastMaxMs / 1000.0;
					if(coastMax < 0.01){ coastMax = 0.01; }
					if(coastMax > 0.5){ coastMax = 0.5; }
					if(ks.coastStart < 0){ ks.coastStart = now; }
					double coastLen = now - ks.coastStart + dt;
					if(coastLen <= coastMax || posOnlyFreeze){
						dupHit = true;
						ks.dupSkipped++;
						double cMs = coastLen * 1000.0;
						if(cMs > ks.coastMaxMs){ ks.coastMaxMs = cMs; }
					}else if((caM || caFull) && !posOnlyFreeze){
						// a repeat sustained past the cap IS stillness by
						// this gate's own definition — so the CA accel
						// states go to zero with it. a live acceleration
						// here is exactly the phantom that keeps fighting
						// the stale position through the post-throw repeat
						// runs (field: hand parked off-position for a
						// beat after hard throws).
						for(int zi = 0; zi < 3; zi++){
							ks.ca[zi] = 0;
							ks.caF[zi] = 0;
							ks.caW[zi] = 0;
						}
					}
				}
			}
			bool dupDrop = dupHit && dupMode == 2 && !posOnlyFreeze;

			// ==== discontinuity reacquisition ====
			// A position jump is checked on the POSITION cadence, not the
			// callback/state cadence.  Coast advances ks.tMeas on repeated
			// callbacks (~360Hz) while distinct positions arrive ~90Hz; using
			// dt here made an ordinary fresh-to-fresh hand step look ~4x too
			// fast and could falsely open reacquisition during hard throws.
			double posDt = ks.tFresh > 0 ? tMeas - ks.tFresh : dt;
			if(posDt <= 0){ posDt = dt; }
			// For continuity only, clamp the comparison horizon to one plausible
			// position epoch.  The lower bound prevents callback-rate false
			// teleports; the upper bound makes a large return after a long
			// position-blind interval prove itself through the candidate path
			// rather than being assimilated as one giant innovation.
			double continuityDt = posDt;
			if(continuityDt < 0.008){ continuityDt = 0.008; }
			if(continuityDt > 0.020){ continuityDt = 0.020; }

			// Once a physically impossible position jump is observed, NEVER
			// let the generic dt>200ms initializer decide when the raw stream
			// has become trustworthy again.  Candidate callbacks are kept out of
			// the live estimator until a coherent short trajectory is proven.
			auto resetReacqAtCurrent = [&](){
				ks.reacqCount = 1;
				ks.reacqFirstT = tMeas;
				ks.reacqLastT = tMeas;
				ks.reacqT[0] = tMeas;
				ks.reacqHavePrevStepV = false;
				for(int rI = 0; rI < 3; rI++){
					ks.reacqFirstP[rI] = pose.vecPosition[rI];
					ks.reacqLastP[rI] = pose.vecPosition[rI];
					ks.reacqP[0][rI] = pose.vecPosition[rI];
					ks.reacqPrevStepV[rI] = 0;
				}
			};
			auto startReacq = [&](double triggerStep, const char* reason){
				ks.reacqActive = true;
				ks.reacqCheck = false;
				resetReacqAtCurrent();
				dropSample = true;
				if(driverConfig.streamFrame.poseLogging){
					DriverLog("PoseLog: KALREACQ id=%u START reason=%s step=%.2fm dt=%.1fms posDt=%.1fms",
						openVRID, reason, triggerStep, dt * 1000.0, posDt * 1000.0);
				}
			};

			// First Running_OK sample after a SHORT flagged loss: accept it
			// immediately when it is physically continuous; otherwise route it
			// into the exact same candidate mechanism as an unflagged teleport.
			if(!dropSample && ks.reacqCheck){
				ks.reacqCheck = false;
				if(ks.have && ks.haveMeas && continuityDt > 0){
					double rdx = pose.vecPosition[0] - ks.lastMeas[0];
					double rdy = pose.vecPosition[1] - ks.lastMeas[1];
					double rdz = pose.vecPosition[2] - ks.lastMeas[2];
					double rStep = sqrt(rdx * rdx + rdy * rdy + rdz * rdz);
					double telM = driverConfig.streamFrame.kalmanTeleportM;
					if(telM > 0 && rStep > telM && rStep > 25.0 * continuityDt){
						startReacq(rStep, "short-loss");
					}
				}
			}

			if(!dropSample && ks.reacqActive){
				// Candidate callbacks are not measurements for the live estimator.
				// Require seven DISTINCT samples (~67ms at the measured 90Hz
				// position cadence), coherent adjacent velocities, and a low-
				// residual least-squares trajectory before promotion.
				dropSample = true;
				double cdt = tMeas - ks.reacqLastT;
				double cdx = pose.vecPosition[0] - ks.reacqLastP[0];
				double cdy = pose.vecPosition[1] - ks.reacqLastP[1];
				double cdz = pose.vecPosition[2] - ks.reacqLastP[2];
				double cStep = sqrt(cdx * cdx + cdy * cdy + cdz * cdz);
				if(cdt > 0 && cStep >= 0.0003){
					// Individual step envelope: ~10m/s plus 2cm measurement/cadence
					// slack.  A candidate outside it restarts from the newest point.
					double maxCStep = 0.02 + 10.0 * cdt;
					if(cdt > 0.2 || cStep > maxCStep){
						resetReacqAtCurrent();
						if(driverConfig.streamFrame.poseLogging){
							DriverLog("PoseLog: KALREACQ id=%u RESET reason=step step=%.2fm cdt=%.1fms",
								openVRID, cStep, cdt * 1000.0);
						}
					}else{
						double stepV[3] = { cdx / cdt, cdy / cdt, cdz / cdt };
						double stepDv2 = 0;
						if(ks.reacqHavePrevStepV){
							for(int rI = 0; rI < 3; rI++){
								double dv = stepV[rI] - ks.reacqPrevStepV[rI];
								stepDv2 += dv * dv;
							}
						}
						double stepDv = sqrt(stepDv2);
						// Generous physical coherence envelope.  The field failures
						// reversed candidate velocity by many m/s in one ~11ms epoch;
						// real hard throws remain comfortably inside this bound.
						double maxStepDv = 1.5 + 120.0 * cdt;
						if(ks.reacqHavePrevStepV && stepDv > maxStepDv){
							resetReacqAtCurrent();
							if(driverConfig.streamFrame.poseLogging){
								DriverLog("PoseLog: KALREACQ id=%u RESET reason=step-accel dV=%.2fm/s max=%.2fm/s cdt=%.1fms",
									openVRID, stepDv, maxStepDv, cdt * 1000.0);
							}
						}else{
							for(int rI = 0; rI < 3; rI++){
								ks.reacqPrevStepV[rI] = stepV[rI];
								ks.reacqLastP[rI] = pose.vecPosition[rI];
							}
							ks.reacqHavePrevStepV = true;
							ks.reacqLastT = tMeas;
							if(ks.reacqCount < KalState::reacqFitN){
								int ci = ks.reacqCount;
								ks.reacqT[ci] = tMeas;
								for(int rI = 0; rI < 3; rI++){
									ks.reacqP[ci][rI] = pose.vecPosition[rI];
								}
								ks.reacqCount++;
							}

							double span = ks.reacqLastT - ks.reacqFirstT;
							if(ks.reacqCount >= KalState::reacqFitN && span >= 0.040){
								// Least-squares constant-velocity fit over the complete
								// candidate window.  This is much harder for one lucky
								// first/last pair in a bad coordinate solution to spoof.
								double meanT = 0;
								double meanP[3] = {0, 0, 0};
								for(int ci = 0; ci < KalState::reacqFitN; ci++){
									meanT += ks.reacqT[ci];
									for(int rI = 0; rI < 3; rI++){
										meanP[rI] += ks.reacqP[ci][rI];
									}
								}
								meanT /= KalState::reacqFitN;
								for(int rI = 0; rI < 3; rI++){ meanP[rI] /= KalState::reacqFitN; }

								double varT = 0;
								double covTP[3] = {0, 0, 0};
								for(int ci = 0; ci < KalState::reacqFitN; ci++){
									double tc = ks.reacqT[ci] - meanT;
									varT += tc * tc;
									for(int rI = 0; rI < 3; rI++){
										covTP[rI] += tc * (ks.reacqP[ci][rI] - meanP[rI]);
									}
								}
								double seedV[3] = {0, 0, 0};
								if(varT > 1e-9){
									for(int rI = 0; rI < 3; rI++){ seedV[rI] = covTP[rI] / varT; }
								}
								double seedV2 = seedV[0] * seedV[0] + seedV[1] * seedV[1] + seedV[2] * seedV[2];
								double seedSpeed = sqrt(seedV2);

								double fitErr2 = 0;
								for(int ci = 0; ci < KalState::reacqFitN; ci++){
									double tc = ks.reacqT[ci] - meanT;
									double e2 = 0;
									for(int rI = 0; rI < 3; rI++){
										double e = ks.reacqP[ci][rI] - (meanP[rI] + seedV[rI] * tc);
										e2 += e * e;
									}
									fitErr2 += e2;
								}
								double fitRms = sqrt(fitErr2 / KalState::reacqFitN);

								// A short reacquisition should also be dynamically reachable
								// from the carried derivative state.  Let the allowance grow
								// with time since the last trusted distinct position so a real
								// long occlusion is not forced to preserve stale direction.
								double sinceFresh = ks.tFresh > 0 ? tMeas - ks.tFresh : span;
								if(sinceFresh < 0){ sinceFresh = 0; }
								if(sinceFresh > 0.5){ sinceFresh = 0.5; }
								double dvCarry2 = 0;
								for(int rI = 0; rI < 3; rI++){
									double dv = seedV[rI] - ks.v[rI];
									dvCarry2 += dv * dv;
								}
								double dvCarry = sqrt(dvCarry2);
								double maxDvCarry = 2.0 + 100.0 * sinceFresh;

								const char* rejectReason = nullptr;
								if(seedSpeed > 10.0){ rejectReason = "seed-speed"; }
								else if(fitRms > 0.040){ rejectReason = "fit-rms"; }
								else if(dvCarry > maxDvCarry){ rejectReason = "carry-dv"; }

								if(rejectReason){
									resetReacqAtCurrent();
									if(driverConfig.streamFrame.poseLogging){
										DriverLog("PoseLog: KALREACQ id=%u RESET reason=%s seedV=%.2fm/s fit=%.1fmm dCarry=%.2f/%.2fm/s span=%.1fms",
											openVRID, rejectReason, seedSpeed, fitRms * 1000.0,
											dvCarry, maxDvCarry, span * 1000.0);
									}
								}else{
									ks.have = true;
									ks.time = now;
									ks.tMeas = tMeas;
									ks.coastStart = -1.0;
									ks.haveSlow = false;
									ks.nisEma = 1.0;
									ks.schedNis = 1.0;
									for(int rI = 0; rI < 3; rI++){
										ks.p[rI] = pose.vecPosition[rI];
										ks.v[rI] = seedV[rI];
										ks.ca[rI] = 0;
										ks.P[rI][0] = 0.01; ks.P[rI][1] = 0; ks.P[rI][2] = 4.0;
										CaInit(ks.P6[rI], 0.01, 4.0, 2500.0);
										if(!caFull){
											ks.pF[rI] = pose.vecPosition[rI];
											ks.vF[rI] = seedV[rI];
											ks.caF[rI] = 0;
											ks.PF[rI][0] = 0.01; ks.PF[rI][1] = 0; ks.PF[rI][2] = 4.0;
											CaInit(ks.PF6[rI], 0.01, 4.0, 2500.0);
										}
										ks.lastMeas[rI] = pose.vecPosition[rI];
									}
									if(!caFull){ ks.haveFast = true; }
									ks.q = pose.qRotation;
									ks.haveMeas = true;
									ks.lastMeasQ[0] = pose.qRotation.w;
									ks.lastMeasQ[1] = pose.qRotation.x;
									ks.lastMeasQ[2] = pose.qRotation.y;
									ks.lastMeasQ[3] = pose.qRotation.z;
									ks.tFresh = tMeas;
									ks.rawCount = 0;
									ks.rawSecHave = false;
									ks.rawPkActive = false;
									ks.rawPkWActive = false;
									ks.rtsCount = 0;
									ks.reacqActive = false;
									ks.reacqCount = 0;
									ks.reacqHavePrevStepV = false;
									if(driverConfig.streamFrame.poseLogging){
										DriverLog("PoseLog: KALREACQ id=%u PROMOTE n=%d span=%.1fms seedV=%.2fm/s seed=(%.2f,%.2f,%.2f) fit=%.1fmm dCarry=%.2fm/s",
											openVRID, KalState::reacqFitN, span * 1000.0, seedSpeed,
											seedV[0], seedV[1], seedV[2], fitRms * 1000.0, dvCarry);
									}
								}
							}
						}
					}
				}
			}
			// Fresh position secant for signed direction diagnostics.  This is
			// telemetry only and deliberately uses the previous accepted raw
			// position with the DISTINCT-position clock (posDt), before lastMeas
			// is changed by the accept path.
			// Positive/negative is the yaw error around +Y:
			// atan2(dot(+Y, v_fd x v_kal), dotXZ(v_fd,v_kal)).
			double rawFd[3] = {0, 0, 0};
			bool rawFdValid = false;
			if(!dropSample && !dupRepeat && ks.haveMeas && posDt > 0.001 && posDt <= 0.2){
				for(int fdI = 0; fdI < 3; fdI++){
					rawFd[fdI] = (pose.vecPosition[fdI] - ks.lastMeas[fdI]) / posDt;
				}
				double fdXZ2 = rawFd[0] * rawFd[0] + rawFd[2] * rawFd[2];
				rawFdValid = fdXZ2 > 0.25; // >0.5 m/s horizontal: direction is meaningful
				if(rawFdValid){
					for(int fdI = 0; fdI < 3; fdI++){ diagFdV[fdI] = rawFd[fdI]; }
				}
			}
			// teleport guard (2026-08-12): an UNFLAGGED reacquire slams
			// a huge step into the filter as one innovation and the
			// reported velocity rockets for several frames (field: 12m
			// and 21m single steps, NIS spikes to 620). physics decides:
			// a real hand cannot exceed ~25 m/s, so a step that both
			// clears the floor (kalmanTeleportM, ignores freeze
			// catch-ups) and implies >25 m/s is a reacquire — reinit
			// cleanly at the new position instead of innovating across
			// it.  The speed test uses continuityDt derived from the distinct-
			// position clock, not callback dt; flagged losses never reach here
			// (estimator gate).
			if(!dropSample && !dupDrop && ks.have && ks.haveMeas && dt > 0 && dt <= 0.2){
				double telM = driverConfig.streamFrame.kalmanTeleportM;
				if(telM > 0){
					double tdx = pose.vecPosition[0] - ks.lastMeas[0];
					double tdy = pose.vecPosition[1] - ks.lastMeas[1];
					double tdz = pose.vecPosition[2] - ks.lastMeas[2];
					double stepT = sqrt(tdx * tdx + tdy * tdy + tdz * tdz);
					if(stepT > telM && stepT > 25.0 * continuityDt){
						ks.teleports++;
						// Start one candidate run instead of spamming rejects
						// until dt crosses 200ms.  While reacqActive is true
						// the generic reinit branch is unreachable.
						startReacq(stepT, "teleport");
					}
				}
			}
			// age of THIS sample's payload: time since the last fresh
			// (distinct-position) sample on the device clock, incl. the
			// current step. used by dupMode 4 (age).
			double payloadAge = ks.tFresh > 0 ? (tMeas - ks.tFresh) : dt;
			if(payloadAge < dt){ payloadAge = dt; }
			if(payloadAge > 0.5){ payloadAge = 0.5; }
			if(dupHit && dupMode == 4 && !posOnlyFreeze){
				// AGE: R_rep = R + (|v|*age)^2, Ra_rep = Ra + (|w|*age)^2.
				// no scale, no floor, no threshold; adaptive-R division
				// undone exactly as in soft (a repeat is never trusted
				// more than its age allows)
				R *= rDivApplied;
				Ra *= raDivApplied;
				double vSp = sqrt(ks.v[0] * ks.v[0] + ks.v[1] * ks.v[1] + ks.v[2] * ks.v[2]);
				double wSp = sqrt(ks.w[0] * ks.w[0] + ks.w[1] * ks.w[1] + ks.w[2] * ks.w[2]);
				double dv = vSp * payloadAge;
				double dw = wSp * payloadAge;
				R += dv * dv;
				Ra += dw * dw;
			}else if(dupHit && (dupMode == 3 || posOnlyFreeze)){
				// SOFT: this repeat WILL be processed as a measurement,
				// but with honest noise for a sample of unknown age —
				// inflate R (and the angular Ra: the payload freezes as
				// a whole) for this callback only. gain on the repeat
				// shrinks ~k^2; covariance keeps accumulating through
				// the run, so the fresh sample's catch-up gain
				// self-schedules. k=1 is bit-identical to off.
				// (2026-08-16 field session) two composition fixes:
				// 1. adaptive measurement trust must NOT apply to
				//    repeats: with adaptR the division (up to maxDiv)
				//    stacked against this inflation, making frozen
				//    repeats MORE trusted than fresh samples (rDiv 4-8
				//    during repeat runs in the field logs = throws
				//    dying in the hand). undo the division here; the
				//    scheduler also no longer feeds on repeats (below).
				R *= rDivApplied;
				Ra *= raDivApplied;
				// 2. the distrust must be ABSOLUTE, not proportional to
				//    the sensor noise: a repeat's true uncertainty is
				//    "how far the hand moves per frozen frame", which
				//    does not shrink when caP is tuned down. floor the
				//    base sigma at 5mm / 2deg (≈0.5m/s and 200deg/s
				//    over one 10ms frame) so honest small caP/caO no
				//    longer silently weakens dedup: at the old field
				//    values (caP 5.7mm) this is behavior-identical.
				double sK = driverConfig.streamFrame.kalmanDupRScale;
				if(sK < 1.0){ sK = 1.0; }
				if(sK > 100.0){ sK = 100.0; }
				double rFloor = 0.005 * 0.005;      // (5mm)^2
				double raFloor = 0.035 * 0.035;     // (2deg)^2 in rad
				double Rrep = R > rFloor ? R : rFloor;
				double RaRep = Ra > raFloor ? Ra : raFloor;
				if(posOnlyFreeze){
					// proven position-only tracker loss: the moving
					// quaternion proves the hand is NOT still, so the
					// frozen position is certainly stale — distrust it
					// harder than an ambiguous repeat, and leave Ra
					// HONEST so the live orientation keeps tracking
					// through the event (matching what the real hand is
					// visibly doing while the old code parked it).
					R = Rrep * sK * sK * 9.0;
				}else{
					R = Rrep * sK * sK;
					Ra = RaRep * sK * sK;
				}
			}else if(adaptR){
				// fresh sample: the division stands; record the peaks
				// here (not at division time) so telemetry reflects
				// trust actually applied to real measurements
				if(rDivApplied > ks.rDivPk){ ks.rDivPk = rDivApplied; }
				if(raDivApplied > ks.rADivPk){ ks.rADivPk = raDivApplied; }
			}
			if(dropSample || dupDrop){
				// state, clocks, and dt statistics untouched. the
				// reported pose repeats the last filtered state; the
				// runtime's forward prediction keeps the rendered hand
				// animating from the still-live velocity.
			}else if(!ks.have || dt <= 0 || dt > 0.2){
				ks.have = true;
				ks.time = now;
				ks.tMeas = tMeas;
				ks.coastStart = -1.0;
				for(int a2 = 0; a2 < 3; a2++){
					ks.p[a2] = pose.vecPosition[a2];
					ks.v[a2] = 0;
					ks.P[a2][0] = 0.01; ks.P[a2][1] = 0; ks.P[a2][2] = 1.0;
					ks.w[a2] = 0;
					ks.Pa[a2][0] = 0.05; ks.Pa[a2][1] = 0; ks.Pa[a2][2] = 10.0;
					// CA state seeding: a = 0 with generous variance
					// ((50 m/s^2)^2 linear) so the first throw after a
					// reinit is not sluggish
					ks.ca[a2] = 0;
					CaInit(ks.P6[a2], 0.01, 1.0, 2500.0);
					ks.caW[a2] = 0;
					CaInit(ks.Pa6[a2], 0.05, 10.0, 40000.0);
				}
				ks.q = pose.qRotation;
				// reinit invalidates the dup/teleport baseline: without
				// this, the teleport guard compares every subsequent
				// sample against the PRE-teleport lastMeas and
				// retriggers once per callback until the position
				// wanders back (field 2026-08-13: one genuine 6.30m
				// garbage pose -> 79 retriggers in 220ms). haveMeas =
				// false makes both guards wait one sample for the
				// accept path to re-seed lastMeas.
				ks.haveMeas = false;
				// raw reference ring restarts across a reinit (a secant
				// spanning a teleport is not a reference)
				ks.rawCount = 0;
				ks.rawSecHave = false;
				ks.rawPkActive = false;
				ks.rawPkWActive = false;
				ks.rtsCount = 0;
			}else{
				ks.time = now;
				ks.tMeas = tMeas;
				ks.dtSumMs += dt * 1000.0;
				ks.dtN++;
				if(dt * 1000.0 > ks.dtMaxMs){ ks.dtMaxMs = dt * 1000.0; }
				double dt2 = dt * dt;
				// dup decision was made above (coast and soft reach
				// here; drop never does — it exits via the drop path)
				bool dupCoast = dupHit && dupMode == 1 && !posOnlyFreeze;
				bool linearMeasurementMissing = posOnlyFreeze;
				if(!dupRepeat){
					// only a genuinely FRESH sample ends the dup run.
					// keyed on dupRepeat, not dupHit: soft-mode repeats
					// are processed but must still accumulate toward the
					// cap, AND repeats past the cap must keep the run
					// alive so they stay at full weight (see the
					// dupRepeat bug-fix note above) instead of
					// restarting the soft-inflation cycle.
					ks.coastStart = -1.0;
				}
				if(dupCoast){
					// coast: advance both estimators along their state,
					// inflate covariance, NO measurement update (a repeat
					// is missing data, not evidence of stillness). CA
					// channels coast on their decaying acceleration; the
					// Singer decay is what bounds phantom integration
					// across a coast run.
					double caBetaC = exp(-dt / caTau);
					double caMagBetaC = exp(-dt / caMagTau);
					for(int a2 = 0; a2 < 3; a2++){
						if(caFull){
							CaStatePredict(dt, caTau, ks.p[a2], ks.v[a2], ks.ca[a2]);
							CaCovPredict(dt, caJ, caTau, caBetaC, caExactCov, ks.P6[a2]);
							rtsStep = true;
							rtsPredX[a2][0] = ks.p[a2]; rtsPredX[a2][1] = ks.v[a2]; rtsPredX[a2][2] = ks.ca[a2];
							for(int c6 = 0; c6 < 6; c6++){ rtsPredP[a2][c6] = ks.P6[a2][c6]; }
						}else{
							ks.p[a2] += ks.v[a2] * dt;
							ks.P[a2][0] += 2.0 * ks.P[a2][1] * dt + ks.P[a2][2] * dt2 + qa * qa * dt2 * dt2 / 4.0;
							ks.P[a2][1] += ks.P[a2][2] * dt + qa * qa * dt2 * dt / 2.0;
							ks.P[a2][2] += qa * qa * dt2;
						}
						if(ks.haveFast && caM){
							CaStatePredict(dt, caMagTau, ks.pF[a2], ks.vF[a2], ks.caF[a2]);
							CaCovPredict(dt, caMagJ, caMagTau, caMagBetaC, caExactCov, ks.PF6[a2]);
						}else if(ks.haveFast){
							ks.pF[a2] += ks.vF[a2] * dt;
							double qaF = driverConfig.streamFrame.kalmanMagAccel;
							if(qaF < 1.0){ qaF = 1.0; }
							if(qaF > 2000.0){ qaF = 2000.0; }
							ks.PF[a2][0] += 2.0 * ks.PF[a2][1] * dt + ks.PF[a2][2] * dt2 + qaF * qaF * dt2 * dt2 / 4.0;
							ks.PF[a2][1] += ks.PF[a2][2] * dt + qaF * qaF * dt2 * dt / 2.0;
							ks.PF[a2][2] += qaF * qaF * dt2;
						}
					}
					// orientation coasts by the current angular velocity
					// (plus the decaying angular acceleration in CA-full)
					double halfDtC = 0.5 * dt;
					vr::HmdQuaternion_t dqc = {1.0, ks.w[0] * halfDtC, ks.w[1] * halfDtC, ks.w[2] * halfDtC};
					if(caFull){
						double caBetaCq = exp(-dt / caTau);
						double aAvgFacC = (caTau / dt) * (1.0 - caBetaCq);
						dqc.x = (ks.w[0] + 0.5 * ks.caW[0] * aAvgFacC * dt) * halfDtC;
						dqc.y = (ks.w[1] + 0.5 * ks.caW[1] * aAvgFacC * dt) * halfDtC;
						dqc.z = (ks.w[2] + 0.5 * ks.caW[2] * aAvgFacC * dt) * halfDtC;
						for(int a2 = 0; a2 < 3; a2++){
							ks.w[a2] += ks.caW[a2] * aAvgFacC * dt;
							ks.caW[a2] *= caBetaCq;
							CaCovPredict(dt, caJA, caTau, caBetaCq, caExactCov, ks.Pa6[a2]);
						}
					}
					ks.q = QuatMultiply(dqc, ks.q);
					double qnc = sqrt(ks.q.w * ks.q.w + ks.q.x * ks.q.x + ks.q.y * ks.q.y + ks.q.z * ks.q.z);
					if(qnc > 1e-9){ ks.q.w /= qnc; ks.q.x /= qnc; ks.q.y /= qnc; ks.q.z /= qnc; }
				}else{
				// linear channel: per-axis constant-velocity Kalman, or
				// the constant-acceleration (Singer) estimator in the
				// CA-full experiment mode. same R, same dup/teleport
				// machinery — only the motion model changes.
				// coordinated-turn coast (kalmanFreezeCoastTurn): while the
				// tracker feeds a frozen position with a live quaternion,
				// rotate the linear v/a by the live angular velocity before
				// predicting, so the coasted hand follows the swing's arc
				// instead of its tangent. the per-axis covariances are left
				// alone (they are already inflating through the run).
				{
					double turnK = driverConfig.streamFrame.kalmanFreezeCoastTurn;
					if(linearMeasurementMissing && turnK > 0.0 && dt > 0.0){
						if(turnK > 1.0){ turnK = 1.0; }
						double ht = 0.5 * dt * turnK;
						vr::HmdQuaternion_t dqt = {1.0, ks.w[0] * ht, ks.w[1] * ht, ks.w[2] * ht};
						double nq = sqrt(dqt.w * dqt.w + dqt.x * dqt.x + dqt.y * dqt.y + dqt.z * dqt.z);
						if(nq > 1e-9){
							dqt.w /= nq; dqt.x /= nq; dqt.y /= nq; dqt.z /= nq;
							double vIn[3] = {ks.v[0], ks.v[1], ks.v[2]};
							double vRot[3];
							QuatRotateVector(dqt, vIn, vRot);
							ks.v[0] = vRot[0]; ks.v[1] = vRot[1]; ks.v[2] = vRot[2];
							if(caFull){
								double aIn[3] = {ks.ca[0], ks.ca[1], ks.ca[2]};
								double aRot[3];
								QuatRotateVector(dqt, aIn, aRot);
								ks.ca[0] = aRot[0]; ks.ca[1] = aRot[1]; ks.ca[2] = aRot[2];
							}
							ks.turnCoastSteps++;
						}
					}
				}
				double caBeta = exp(-dt / caTau);
				double nisAccum = 0;
				double nisBaseAccum = 0;
				if(caFull){
					for(int a2 = 0; a2 < 3; a2++){
						CaStatePredict(dt, caTau, ks.p[a2], ks.v[a2], ks.ca[a2]);
						CaCovPredict(dt, caJ, caTau, caBeta, caExactCov, ks.P6[a2]);
						rtsStep = true;
						rtsPredX[a2][0] = ks.p[a2]; rtsPredX[a2][1] = ks.v[a2]; rtsPredX[a2][2] = ks.ca[a2];
						for(int c6 = 0; c6 < 6; c6++){ rtsPredP[a2][c6] = ks.P6[a2][c6]; }
						if(!linearMeasurementMissing){
							double yv = pose.vecPosition[a2] - ks.p[a2];
							nisBaseAccum += yv * yv / (ks.P6[a2][0] + RbaseSched);
							nisAccum += CaUpdate(yv, R,
								ks.p[a2], ks.v[a2], ks.ca[a2], ks.P6[a2]);
						}
					}
					double caMagNow = sqrt(ks.ca[0] * ks.ca[0] + ks.ca[1] * ks.ca[1] + ks.ca[2] * ks.ca[2]);
					if(caMagNow > ks.caAccPk){ ks.caAccPk = caMagNow; }
				}else
				for(int a2 = 0; a2 < 3; a2++){
					ks.p[a2] += ks.v[a2] * dt;
					double Ppp = ks.P[a2][0] + 2.0 * ks.P[a2][1] * dt + ks.P[a2][2] * dt2 + qa * qa * dt2 * dt2 / 4.0;
					double Ppv = ks.P[a2][1] + ks.P[a2][2] * dt + qa * qa * dt2 * dt / 2.0;
					double Pvv = ks.P[a2][2] + qa * qa * dt2;
					if(linearMeasurementMissing){
						ks.P[a2][0] = Ppp;
						ks.P[a2][1] = Ppv;
						ks.P[a2][2] = Pvv;
					}else{
						double y = pose.vecPosition[a2] - ks.p[a2];
						double S = Ppp + R;
						nisAccum += y * y / S;
						nisBaseAccum += y * y / (Ppp + RbaseSched);
						double Kp = Ppp / S;
						double Kv = Ppv / S;
						ks.p[a2] += Kp * y;
						ks.v[a2] += Kv * y;
						ks.P[a2][0] = (1.0 - Kp) * Ppp;
						ks.P[a2][1] = (1.0 - Kp) * Ppv;
						ks.P[a2][2] = Pvv - Kv * Ppv;
					}
				}
				if(!linearMeasurementMissing){
					ks.nisEma += 0.1 * (nisAccum / 3.0 - ks.nisEma);
					// The linear scheduler must learn only from a real
					// positional measurement.  A p3d callback has live
					// orientation but no new translational observation.
					if(!dupHit){
						ks.schedNis += 0.3 * (nisBaseAccum / 3.0 - ks.schedNis);
					}
				}
				// raw-step telemetry (EMA-free, so single-frame freezes or
				// teleports cannot hide): settles the FOV question
				{
					if(ks.haveMeas){
						double dx = pose.vecPosition[0] - ks.lastMeas[0];
						double dy = pose.vecPosition[1] - ks.lastMeas[1];
						double dz = pose.vecPosition[2] - ks.lastMeas[2];
						double step = sqrt(dx * dx + dy * dy + dz * dz);
						if(step > ks.stepMax){ ks.stepMax = step; }
						double stSpeed = sqrt(ks.v[0] * ks.v[0] + ks.v[1] * ks.v[1] + ks.v[2] * ks.v[2]);
						if(step < 0.0003 && stSpeed > 0.7){ ks.stepFrozen++; }
						// fresh-to-fresh clock: time between DISTINCT
						// raw samples on the measurement clock — the
						// tracker's true cadence (~8.3ms expected),
						// as opposed to dtMean's callback cadence.
						// mode-independent by design; also the
						// instrument for any transport-side fix.
						if(step >= 0.0003){
							if(ks.tFresh > 0){
								double fdt = (tMeas - ks.tFresh) * 1000.0;
								if(fdt > 0){
									ks.fdtSumMs += fdt;
									ks.fdtN++;
									if(fdt > ks.fdtMaxMs){ ks.fdtMaxMs = fdt; }
								}
							}
							ks.tFresh = tMeas;
							// raw reference ring: fresh samples only (see
							// KalState::rawRingN). secant over the full ring
							// attributed to the window CENTER, so its peak
							// time is an unbiased estimate of the true peak
							// instant (a windowed average lags by half its
							// span; centering removes that).
							ks.rawT[ks.rawHead] = tMeas;
							ks.rawTr[ks.rawHead] = now;
							ks.rawP[ks.rawHead][0] = pose.vecPosition[0];
							ks.rawP[ks.rawHead][1] = pose.vecPosition[1];
							ks.rawP[ks.rawHead][2] = pose.vecPosition[2];
							ks.rawQ[ks.rawHead] = pose.qRotation;
							ks.rawHead = (ks.rawHead + 1) % KalState::rawRingN;
							if(ks.rawCount < KalState::rawRingN){ ks.rawCount++; }
							if(ks.rawCount == KalState::rawRingN){
								int rNew = (ks.rawHead - 1 + KalState::rawRingN) % KalState::rawRingN;
								int rOld = ks.rawHead;
								double rSpan = ks.rawT[rNew] - ks.rawT[rOld];
								if(rSpan > 0.005 && rSpan < 0.25){
									ks.rawSecHave = true;
									// receipt-clock center: comparable to the
									// release edge and to pkOutT (both receipt)
									ks.rawSecT = 0.5 * (ks.rawTr[rNew] + ks.rawTr[rOld]);
									for(int a2 = 0; a2 < 3; a2++){
										ks.rawSecV[a2] = (ks.rawP[rNew][a2] - ks.rawP[rOld][a2]) / rSpan;
									}
									vr::HmdQuaternion_t qoc = {ks.rawQ[rOld].w, -ks.rawQ[rOld].x, -ks.rawQ[rOld].y, -ks.rawQ[rOld].z};
									vr::HmdQuaternion_t rdq = QuatMultiply(ks.rawQ[rNew], qoc);
									double rsg = rdq.w < 0 ? -1.0 : 1.0;
									double rvn = sqrt(rdq.x * rdq.x + rdq.y * rdq.y + rdq.z * rdq.z);
									double rang = 2.0 * atan2(rvn, fabs(rdq.w));
									double rsc = rvn > 1e-9 ? rsg * rang / (rvn * rSpan) : 0.0;
									ks.rawSecW[0] = rdq.x * rsc;
									ks.rawSecW[1] = rdq.y * rsc;
									ks.rawSecW[2] = rdq.z * rsc;
									double rsp = sqrt(ks.rawSecV[0] * ks.rawSecV[0] + ks.rawSecV[1] * ks.rawSecV[1] + ks.rawSecV[2] * ks.rawSecV[2]);
									double rwsp = sqrt(ks.rawSecW[0] * ks.rawSecW[0] + ks.rawSecW[1] * ks.rawSecW[1] + ks.rawSecW[2] * ks.rawSecW[2]);
									// raw peak segmentation on the RAW speed
									if(!ks.rawPkActive && rsp > 1.0){
										ks.rawPkActive = true;
										ks.rawPkSp = 0;
									}
									if(ks.rawPkActive){
										if(rsp > ks.rawPkSp){
											ks.rawPkSp = rsp;
											ks.rawPkT = ks.rawSecT;
											for(int a2 = 0; a2 < 3; a2++){ ks.rawPkV[a2] = ks.rawSecV[a2]; }
										}
										if(rsp < 0.8){ ks.rawPkActive = false; }
									}
									if(!ks.rawPkWActive && rwsp > 4.0){
										ks.rawPkWActive = true;
										ks.rawPkWSp = 0;
									}
									if(ks.rawPkWActive){
										if(rwsp > ks.rawPkWSp){
											ks.rawPkWSp = rwsp;
											ks.rawPkWT = ks.rawSecT;
											for(int a2 = 0; a2 < 3; a2++){ ks.rawPkW[a2] = ks.rawSecW[a2]; }
										}
										if(rwsp < 3.0){ ks.rawPkWActive = false; }
									}
								}
							}
						}
					}else{
						// Seed the distinct-position clock with the first accepted
						// baseline so the next fresh step is timed against position,
						// not whichever callback happened immediately before it.
						ks.tFresh = tMeas;
					}
					ks.haveMeas = true;
					ks.lastMeasQ[0] = pose.qRotation.w;
					ks.lastMeasQ[1] = pose.qRotation.x;
					ks.lastMeasQ[2] = pose.qRotation.y;
					ks.lastMeasQ[3] = pose.qRotation.z;
					ks.lastMeas[0] = pose.vecPosition[0];
					ks.lastMeas[1] = pose.vecPosition[1];
					ks.lastMeas[2] = pose.vecPosition[2];
				}
				// parallel FAST estimator for the magnitude channel.
				// mode 4: CV with its own high accel (legacy). mode 5
				// (CA-M): a constant-acceleration channel — jerk and
				// decay attack the ramp-lag deficit directly, at the
				// sensor-floor R, instead of buying magnitude with a
				// loosened CV. mode 6 (CA-full): no fast channel; the
				// main CA state carries honest magnitude itself.
				if(caM){
					double caMagBeta = exp(-dt / caMagTau);
					if(!ks.haveFast){
						ks.haveFast = true;
						for(int a2 = 0; a2 < 3; a2++){
							ks.pF[a2] = pose.vecPosition[a2];
							ks.vF[a2] = 0;
							ks.caF[a2] = 0;
							CaInit(ks.PF6[a2], 0.01, 1.0, 2500.0);
						}
					}else{
						for(int a2 = 0; a2 < 3; a2++){
							CaStatePredict(dt, caMagTau, ks.pF[a2], ks.vF[a2], ks.caF[a2]);
							CaCovPredict(dt, caMagJ, caMagTau, caMagBeta, caExactCov, ks.PF6[a2]);
							if(!linearMeasurementMissing){
								CaUpdate(pose.vecPosition[a2] - ks.pF[a2], R,
									ks.pF[a2], ks.vF[a2], ks.caF[a2], ks.PF6[a2]);
							}
						}
						double caFMag = sqrt(ks.caF[0] * ks.caF[0] + ks.caF[1] * ks.caF[1] + ks.caF[2] * ks.caF[2]);
						if(caFMag > ks.caAccPk){ ks.caAccPk = caFMag; }
					}
				}else if(!caFull){
					double qaF = driverConfig.streamFrame.kalmanMagAccel;
					if(qaF < 1.0){ qaF = 1.0; }
					if(qaF > 2000.0){ qaF = 2000.0; }
					if(!ks.haveFast){
						ks.haveFast = true;
						for(int a2 = 0; a2 < 3; a2++){
							ks.pF[a2] = pose.vecPosition[a2];
							ks.vF[a2] = 0;
							ks.PF[a2][0] = 0.01; ks.PF[a2][1] = 0; ks.PF[a2][2] = 1.0;
						}
					}else{
						for(int a2 = 0; a2 < 3; a2++){
							ks.pF[a2] += ks.vF[a2] * dt;
							double Ppp = ks.PF[a2][0] + 2.0 * ks.PF[a2][1] * dt + ks.PF[a2][2] * dt2 + qaF * qaF * dt2 * dt2 / 4.0;
							double Ppv = ks.PF[a2][1] + ks.PF[a2][2] * dt + qaF * qaF * dt2 * dt / 2.0;
							double Pvv = ks.PF[a2][2] + qaF * qaF * dt2;
							if(linearMeasurementMissing){
								ks.PF[a2][0] = Ppp;
								ks.PF[a2][1] = Ppv;
								ks.PF[a2][2] = Pvv;
							}else{
								double y = pose.vecPosition[a2] - ks.pF[a2];
								double S = Ppp + R;
								double Kp = Ppp / S;
								double Kv = Ppv / S;
								ks.pF[a2] += Kp * y;
								ks.vF[a2] += Kv * y;
								ks.PF[a2][0] = (1.0 - Kp) * Ppp;
								ks.PF[a2][1] = (1.0 - Kp) * Ppv;
								ks.PF[a2][2] = Pvv - Kv * Ppv;
							}
						}
					}
				}
				// angular channel: predict q by w (in CA-full: by the
				// midpoint angular velocity including the angular
				// acceleration state), correct by the residual
				double halfDt = 0.5 * dt;
				vr::HmdQuaternion_t dq = {1.0, ks.w[0] * halfDt, ks.w[1] * halfDt, ks.w[2] * halfDt};
				if(caFull){
					// exact Singer average for the angular accel too:
					// bounds the same long-gap overshoot on orientation
					double aAvgFacA = (caTau / dt) * (1.0 - caBeta);
					dq.x = (ks.w[0] + 0.5 * ks.caW[0] * aAvgFacA * dt) * halfDt;
					dq.y = (ks.w[1] + 0.5 * ks.caW[1] * aAvgFacA * dt) * halfDt;
					dq.z = (ks.w[2] + 0.5 * ks.caW[2] * aAvgFacA * dt) * halfDt;
					for(int a2 = 0; a2 < 3; a2++){
						ks.w[a2] += ks.caW[a2] * aAvgFacA * dt;
						ks.caW[a2] *= caBeta;
						CaCovPredict(dt, caJA, caTau, caBeta, caExactCov, ks.Pa6[a2]);
					}
					double caWMag = sqrt(ks.caW[0] * ks.caW[0] + ks.caW[1] * ks.caW[1] + ks.caW[2] * ks.caW[2]);
					if(caWMag > ks.caWAccPk){ ks.caWAccPk = caWMag; }
				}
				vr::HmdQuaternion_t qPred = QuatMultiply(dq, ks.q);
				double qn = sqrt(qPred.w * qPred.w + qPred.x * qPred.x + qPred.y * qPred.y + qPred.z * qPred.z);
				if(qn > 1e-9){ qPred.w /= qn; qPred.x /= qn; qPred.y /= qn; qPred.z /= qn; }
				vr::HmdQuaternion_t qc = {qPred.w, -qPred.x, -qPred.y, -qPred.z};
				vr::HmdQuaternion_t qe = QuatMultiply(pose.qRotation, qc);
				double sgn = qe.w < 0 ? -1.0 : 1.0;
				double res[3] = { 2.0 * sgn * qe.x, 2.0 * sgn * qe.y, 2.0 * sgn * qe.z };
				double corr[3];
				double aNisAccum = 0;
				double aNisBaseAccum = 0;
				if(caFull){
					// covariance already predicted above; the zero-seeded
					// error scratch comes back as K0 * residual = the
					// orientation correction, w gets K1, caW gets K2
					for(int a2 = 0; a2 < 3; a2++){
						double errS = 0;
						aNisBaseAccum += res[a2] * res[a2] / (ks.Pa6[a2][0] + RaBaseSched);
						aNisAccum += CaUpdate(res[a2], Ra, errS, ks.w[a2], ks.caW[a2], ks.Pa6[a2]);
						corr[a2] = errS;
					}
				}else
				for(int a2 = 0; a2 < 3; a2++){
					double Ppp = ks.Pa[a2][0] + 2.0 * ks.Pa[a2][1] * dt + ks.Pa[a2][2] * dt2 + qaA * qaA * dt2 * dt2 / 4.0;
					double Ppv = ks.Pa[a2][1] + ks.Pa[a2][2] * dt + qaA * qaA * dt2 * dt / 2.0;
					double Pvv = ks.Pa[a2][2] + qaA * qaA * dt2;
					double S = Ppp + Ra;
					aNisAccum += res[a2] * res[a2] / S;
					aNisBaseAccum += res[a2] * res[a2] / (Ppp + RaBaseSched);
					double Kp = Ppp / S;
					double Kv = Ppv / S;
					corr[a2] = Kp * res[a2];
					ks.w[a2] += Kv * res[a2];
					ks.Pa[a2][0] = (1.0 - Kp) * Ppp;
					ks.Pa[a2][1] = (1.0 - Kp) * Ppv;
					ks.Pa[a2][2] = Pvv - Kv * Ppv;
				}
				ks.nisAEma += 0.1 * (aNisAccum / 3.0 - ks.nisAEma);
				// same repeat exclusion as the linear scheduler
				if(!dupHit){
					ks.schedANis += 0.3 * (aNisBaseAccum / 3.0 - ks.schedANis);
				}
				vr::HmdQuaternion_t qCorr = {1.0, corr[0] * 0.5, corr[1] * 0.5, corr[2] * 0.5};
				ks.q = QuatMultiply(qCorr, qPred);
				double qn2 = sqrt(ks.q.w * ks.q.w + ks.q.x * ks.q.x + ks.q.y * ks.q.y + ks.q.z * ks.q.z);
				if(qn2 > 1e-9){ ks.q.w /= qn2; ks.q.x /= qn2; ks.q.y /= qn2; ks.q.z /= qn2; }
				}
			}
			// STUCKDIAG watchdog: state position vs THIS measurement (pose
			// is still the raw measurement here — the report block below
			// overwrites it). a divergence run opening means the filter's
			// carried momentum is gliding past what the tracker reports —
			// the filter-side stuck-hand mechanism, as opposed to the
			// KALLOSS raw-passthrough freeze. pure telemetry: no state is
			// touched, no behavior changes.
			if(driverConfig.streamFrame.poseLogging && !dropSample && !dupDrop && !posOnlyFreeze && ks.have){
				double sdx = ks.p[0] - pose.vecPosition[0];
				double sdy = ks.p[1] - pose.vecPosition[1];
				double sdz = ks.p[2] - pose.vecPosition[2];
				double sdiv = sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
				if(!ks.stuckRun){
					if(sdiv > 0.25){
						ks.stuckRun = true;
						ks.stuckStartT = now;
						ks.stuckMax = sdiv;
						ks.stuckV0 = sqrt(ks.v[0] * ks.v[0] + ks.v[1] * ks.v[1] + ks.v[2] * ks.v[2]);
						stuckEnterLog = true;
						stuckLogDiv = sdiv;
						stuckLogV0 = ks.stuckV0;
					}
				}else{
					if(sdiv > ks.stuckMax){ ks.stuckMax = sdiv; }
					if(sdiv < 0.10){
						ks.stuckRun = false;
						stuckExitLog = true;
						stuckLogMs = (now - ks.stuckStartT) * 1000.0;
						stuckLogMax = ks.stuckMax;
						stuckLogV0 = ks.stuckV0;
					}
				}
			}
			// grip-point velocity transport: the state's v is the TRACKED
			// ORIGIN's velocity; the palm's is v + w x r with r the
			// origin->grip vector rotated into world by the filtered q.
			// rigid-body identity, so the same w serves every point — the
			// angular channel is untouched. shadow-computed whenever r is
			// set (PEAKDIAG scores it against the same secant, and the
			// flick unit test is |vGrip| collapsing to ~0 on pure wrist
			// snaps); REPORTED only when the enable knob is on. ks.v/ks.w
			// are never written: the estimator keeps tracking the origin
			// the measurements actually describe.
			double vOutState[3] = { ks.v[0], ks.v[1], ks.v[2] };
			ks.diagGripHave = gripHave;
			ks.diagGripWr = 0;
			if(gripHave){
				double rWorld[3];
				QuatRotateVector(ks.q, gripLocal, rWorld);
				double wxr[3] = {
					ks.w[1] * rWorld[2] - ks.w[2] * rWorld[1],
					ks.w[2] * rWorld[0] - ks.w[0] * rWorld[2],
					ks.w[0] * rWorld[1] - ks.w[1] * rWorld[0],
				};
				ks.diagGripWr = gripBlend * sqrt(wxr[0] * wxr[0] + wxr[1] * wxr[1] + wxr[2] * wxr[2]);
				for(int a2 = 0; a2 < 3; a2++){
					ks.diagGripV[a2] = ks.v[a2] + gripBlend * wxr[a2];
				}
				if(gripEnable){
					// compensated velocity feeds the history ring too, so
					// the rewind blend, the fixed-lag smoother and RELDIAG
					// all see the channel the game sees
					for(int a2 = 0; a2 < 3; a2++){ vOutState[a2] = ks.diagGripV[a2]; }
				}
			}else{
				for(int a2 = 0; a2 < 3; a2++){ ks.diagGripV[a2] = ks.v[a2]; }
			}
			// report THE STATE, whole and self consistent (optional fixed
			// forward lead, as native drivers use against transport lag)
			// history push (cheap, always on: keeps the rewind warm so
			// enabling the experiment mid-session works immediately)
			ks.histT[ks.histHead] = now;
			for(int a2 = 0; a2 < 3; a2++){
				ks.histV[ks.histHead][a2] = vOutState[a2];
				ks.histW[ks.histHead][a2] = ks.w[a2];
				ks.histP[ks.histHead][a2] = ks.p[a2];
			}
			ks.histQ[ks.histHead] = ks.q;
			ks.histHead = (ks.histHead + 1) % KalState::histSize;
			if(ks.histCount < KalState::histSize){ ks.histCount++; }
			if(caFull && rtsStep){
				int rh = ks.rtsHead;
				ks.rtsT[rh] = tMeas;
				ks.rtsDt[rh] = dt;
				for(int a2 = 0; a2 < 3; a2++){
					for(int c3 = 0; c3 < 3; c3++){ ks.rtsXp[rh][a2][c3] = rtsPredX[a2][c3]; }
					for(int c6 = 0; c6 < 6; c6++){ ks.rtsPp[rh][a2][c6] = rtsPredP[a2][c6]; ks.rtsPf[rh][a2][c6] = ks.P6[a2][c6]; }
					ks.rtsXf[rh][a2][0] = ks.p[a2]; ks.rtsXf[rh][a2][1] = ks.v[a2]; ks.rtsXf[rh][a2][2] = ks.ca[a2];
					ks.rtsW[rh][a2] = ks.w[a2];
					ks.rtsWa[rh][a2] = ks.caW[a2];
				}
				ks.rtsQ[rh] = ks.q;
				ks.rtsHead = (rh + 1) % KalState::rtsN;
				if(ks.rtsCount < KalState::rtsN){ ks.rtsCount++; }
			}
			// PEAKDIAG scratch: this frame's per-channel speeds, read by
			// the post-DeriveMotion gesture scorer under the same lock
			ks.diagCalmSp = sqrt(ks.v[0] * ks.v[0] + ks.v[1] * ks.v[1] + ks.v[2] * ks.v[2]);
			ks.diagMagSp = ks.haveFast
				? sqrt(ks.vF[0] * ks.vF[0] + ks.vF[1] * ks.vF[1] + ks.vF[2] * ks.vF[2])
				: ks.diagCalmSp;
			double vRep[3] = { vOutState[0], vOutState[1], vOutState[2] };
			double wRep[3] = { ks.w[0], ks.w[1], ks.w[2] };
			double ksOutP[3] = {0, 0, 0};
			vr::HmdQuaternion_t ksOutQ = {1, 0, 0, 0};
			bool useSmoothOut = false;
			if(driverConfig.streamFrame.kalmanReleaseRewindMs > 0.5 && now < ks.rewindUntil && ks.histCount > 2){
				// find the history sample closest to the rewind target and
				// report it, blending back to live over the hold window
				int best = -1;
				double bestD = 1e9;
				for(int i = 0; i < ks.histCount; i++){
					double d = fabs(ks.histT[i] - ks.rewindTarget);
					if(d < bestD){ bestD = d; best = i; }
				}
				if(best >= 0 && bestD < 0.1){
					double holdS = driverConfig.streamFrame.kalmanRewindHoldMs / 1000.0;
					if(holdS < 0.02){ holdS = 0.02; }
					double frac = (ks.rewindUntil - now) / holdS; // 1 -> 0
					if(frac > 1){ frac = 1; }
					if(frac < 0){ frac = 0; }
					for(int a2 = 0; a2 < 3; a2++){
						vRep[a2] = ks.histV[best][a2] * frac + vRep[a2] * (1.0 - frac);
						wRep[a2] = ks.histW[best][a2] * frac + wRep[a2] * (1.0 - frac);
					}
				}
			}
			// direction/magnitude split: direction from the slow EMA,
			// magnitude live. slow copies always maintained (cheap) so the
			// knob engages instantly; gated at low speed where direction
			// is meaningless.
			{
				double dMs = driverConfig.streamFrame.kalmanDirSmoothMs;
				double aMs = driverConfig.streamFrame.kalmanAngDirSmoothMs;
				double sdt = dt;
				if(sdt <= 0 || sdt > 0.05){ sdt = 0.011; }
				if(!ks.haveSlow){
					ks.haveSlow = true;
					for(int a2 = 0; a2 < 3; a2++){ ks.vSlow[a2] = vRep[a2]; ks.wSlow[a2] = wRep[a2]; }
				}else{
					double aV = dMs > 0.5 ? 1.0 - exp(-sdt / (dMs / 1000.0)) : 1.0;
					double aW = aMs > 0.5 ? 1.0 - exp(-sdt / (aMs / 1000.0)) : 1.0;
					for(int a2 = 0; a2 < 3; a2++){
						ks.vSlow[a2] += aV * (vRep[a2] - ks.vSlow[a2]);
						ks.wSlow[a2] += aW * (wRep[a2] - ks.wSlow[a2]);
					}
				}
				if(dMs > 0.5){
					double mLive = sqrt(vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2]);
					double mSlow = sqrt(ks.vSlow[0] * ks.vSlow[0] + ks.vSlow[1] * ks.vSlow[1] + ks.vSlow[2] * ks.vSlow[2]);
					if(mLive > 0.3 && mSlow > 0.15){
						for(int a2 = 0; a2 < 3; a2++){ vRep[a2] = ks.vSlow[a2] / mSlow * mLive; }
					}
				}
				if(aMs > 0.5){
					double mLiveW = sqrt(wRep[0] * wRep[0] + wRep[1] * wRep[1] + wRep[2] * wRep[2]);
					double mSlowW = sqrt(ks.wSlow[0] * ks.wSlow[0] + ks.wSlow[1] * ks.wSlow[1] + ks.wSlow[2] * ks.wSlow[2]);
					if(mLiveW > 1.0 && mSlowW > 0.5){
						for(int a2 = 0; a2 < 3; a2++){ wRep[a2] = ks.wSlow[a2] / mSlowW * mLiveW; }
					}
				}
			}
			// magnitude channel: |v| from the fast estimator on the calm
			// state's direction, then the always-on trim multipliers
			{
				// CA-M forces the (now CA) fast channel as the magnitude
				// source — that substitution IS the mode
				if((driverConfig.streamFrame.kalmanMagSource == 1 || caM) && ks.haveFast){
					// STRICT channel separation (field 2026-08-10: every 180
					// flip traced to the fast estimator's DIRECTION leaking
					// into the output — via the raw-vector fallback in the
					// first version, via disagreement handoffs in the blend
					// version. the fast channel's direction is noise; it is
					// NEVER reported. direction comes from the calm state
					// only; fast contributes MAGNITUDE, and only once the
					// calm direction is established. otherwise the output
					// is pure calm: occasionally weak, never flipped.)
					double mDir = sqrt(vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2]);
					double mFast = sqrt(ks.vF[0] * ks.vF[0] + ks.vF[1] * ks.vF[1] + ks.vF[2] * ks.vF[2]);
					if(mDir > 0.3 && mFast > mDir){
						for(int a2 = 0; a2 < 3; a2++){ vRep[a2] = vRep[a2] / mDir * mFast; }
					}
					// otherwise: pure calm output stands
				}
				double mS = driverConfig.streamFrame.kalmanMagScale;
				if(mS < 0.25){ mS = 0.25; }
				if(mS > 4.0){ mS = 4.0; }
				double mSA = driverConfig.streamFrame.kalmanAngMagScale;
				if(mSA < 0.25){ mSA = 0.25; }
				if(mSA > 4.0){ mSA = 4.0; }
				for(int a2 = 0; a2 < 3; a2++){
					vRep[a2] *= mS;
					wRep[a2] *= mSA;
				}
			}
			// Direction lead (Rodrigues wrist-axis derotation, ALL modes).
			//
			// 2026-08-16: the CA-full state-prediction variant (v + a*tau*
			// (1-e^(-L/tau))) is retired. at the ratified J/tau the linear
			// acceleration state is effectively dead (offline: peaks ~4
			// m/s^2 against a ~65 m/s^2 throw; the Td=5 nudge is ~17 mm/s
			// on a 5 m/s throw = ~0.2deg of bend), so that form was a
			// no-op, and the relDirOff series that "ratified" it was
			// scored against a secant of the FILTERED pose (see the raw
			// ring note in KalState) — an instrument artifact. the field
			// evidence that stands is the felt one: without derotation,
			// combined arm+wrist throws skew left. so CA-full goes back
			// to the proven form: rotate the reported direction forward
			// about the filter's own w-hat by |w|*Td. identity when w~0,
			// nothing to bend when |v|~0, magnitude/spin/pose untouched.
			// the acceleration-state prediction can be revisited only
			// with a live accel state (tau >= ~100ms), which the same
			// offline pass shows overshoots and reverses after release.
			{
				for(int a2 = 0; a2 < 3; a2++){
					diagPreTdV[a2] = vRep[a2];
				}
				double wm = sqrt(wRep[0] * wRep[0] + wRep[1] * wRep[1] + wRep[2] * wRep[2]);
				double dLead;
				if(driverConfig.streamFrame.kalmanDirLeadAdaptive){
					// adaptive: Td_eff = base + slope * |w| — the tail
					// autopsy's throw-dependent lag, as a smooth law.
					// OVERRIDES the manual knob while enabled.
					double baseMs = driverConfig.streamFrame.kalmanDirLeadBaseMs;
					if(baseMs < 0){ baseMs = 0; }
					double slope = driverConfig.streamFrame.kalmanDirLeadWMs;
					if(slope < 0){ slope = 0; }
					dLead = (baseMs + slope * wm) / 1000.0;
				}else{
					dLead = driverConfig.streamFrame.kalmanDirLeadMs / 1000.0;
				}
				if(dLead > 0.0){
					if(dLead > 0.05){ dLead = 0.05; }
					double mV = sqrt(vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2]);
					if(wm > 1e-3 && mV > 0.05){
						diagTdCmdDeg = wm * dLead * 180.0 / 3.14159265358979323846;
						double ax = wRep[0] / wm, ay = wRep[1] / wm, az = wRep[2] / wm;
						double ang = wm * dLead;
						double c = cos(ang), s = sin(ang);
						double adotv = ax * vRep[0] + ay * vRep[1] + az * vRep[2];
						double cr[3] = {
							ay * vRep[2] - az * vRep[1],
							az * vRep[0] - ax * vRep[2],
							ax * vRep[1] - ay * vRep[0],
						};
						for(int a2 = 0; a2 < 3; a2++){
							double axc[3] = { ax, ay, az };
							vRep[a2] = vRep[a2] * c + cr[a2] * s + axc[a2] * adotv * (1.0 - c);
						}
					}
				}
				for(int a2 = 0; a2 < 3; a2++){
					diagPostTdV[a2] = vRep[a2];
					diagKalW[a2] = ks.w[a2];
				}
				double bendNow = vecAngleDeg(diagPreTdV, diagPostTdV);
				if(bendNow >= 0){ diagTdBendDeg = bendNow; }

				// Controller local basis expressed in driver/world space.
				// We log all three axes rather than assume which physical
				// controller axis is the palm normal.
				const double lx[3] = {1, 0, 0};
				const double ly[3] = {0, 1, 0};
				const double lz[3] = {0, 0, 1};
				QuatRotateVector(ks.q, lx, diagLocalX);
				QuatRotateVector(ks.q, ly, diagLocalY);
				QuatRotateVector(ks.q, lz, diagLocalZ);

				// Translational curvature angular velocity, observe-only:
				// omega_curve = (v x a) / |v|^2.  If this aligns with throw
				// correction better than controller w, it supports replacing
				// Td's wrist-axis rotation with a trajectory-derived axis.
				double kv2 = ks.v[0] * ks.v[0] + ks.v[1] * ks.v[1] + ks.v[2] * ks.v[2];
				if(caFull && kv2 > 0.25){
					diagCurveW[0] = (ks.v[1] * ks.ca[2] - ks.v[2] * ks.ca[1]) / kv2;
					diagCurveW[1] = (ks.v[2] * ks.ca[0] - ks.v[0] * ks.ca[2]) / kv2;
					diagCurveW[2] = (ks.v[0] * ks.ca[1] - ks.v[1] * ks.ca[0]) / kv2;
					diagCurveVsKalDeg = vecAngleDeg(diagCurveW, diagKalW);
				}
				// Source linear velocity diagnostics.  fdV is available only
				// on a genuinely fresh positional measurement.  During p3d /
				// reacquisition it intentionally becomes NA while srcV and
				// kalV keep logging, letting the field trace show whether the
				// upstream velocity still carries useful unseen-motion data.
				diagSrcKalDeg = vecAngleDeg(diagSrcV, ks.v);
				diagP3dNow = posOnlyFreeze ? 1 : 0;
				diagReacqNow = ks.reacqActive ? 1 : 0;
				for(int a2 = 0; a2 < 3; a2++){
					diagKalV[a2] = ks.v[a2];
				}
				diagFdTrusted = rawFdValid && !ks.reacqActive && !posOnlyFreeze;
				// Normalize WorldFromDriver before using it as a rotation. Test
				// both directions because the OpenVR pose transform convention and
				// vrlink's velocity convention are exactly what this probe is
				// intended to adjudicate in the field.
				double qwdN = sqrt(diagQwd.w * diagQwd.w + diagQwd.x * diagQwd.x
					+ diagQwd.y * diagQwd.y + diagQwd.z * diagQwd.z);
				if(qwdN > 1e-9){
					diagQwd.w /= qwdN; diagQwd.x /= qwdN; diagQwd.y /= qwdN; diagQwd.z /= qwdN;
					double qwAbs = fabs(diagQwd.w);
					if(qwAbs > 1.0){ qwAbs = 1.0; }
					diagQwdAngleDeg = 2.0 * acos(qwAbs) * 180.0 / 3.14159265358979323846;
					QuatRotateVector(diagQwd, diagSrcV, diagSrcVQwd);
					vr::HmdQuaternion_t qwdInv = {diagQwd.w, -diagQwd.x, -diagQwd.y, -diagQwd.z};
					QuatRotateVector(qwdInv, diagSrcV, diagSrcVQwdInv);
				}
				if(diagFdTrusted){
					diagSrcFdDeg = vecAngleDeg(diagSrcV, diagFdV);
					diagSrcQwdFdDeg = vecAngleDeg(diagSrcVQwd, diagFdV);
					diagSrcQwdInvFdDeg = vecAngleDeg(diagSrcVQwdInv, diagFdV);
					double srcM = sqrt(diagSrcV[0]*diagSrcV[0] + diagSrcV[1]*diagSrcV[1] + diagSrcV[2]*diagSrcV[2]);
					double fdM = sqrt(diagFdV[0]*diagFdV[0] + diagFdV[1]*diagFdV[1] + diagFdV[2]*diagFdV[2]);
					if(fdM > 0.05){ diagSrcFdMagRatio = srcM / fdM; }
				}
				if(diagQRateValid){
					diagSrcWorldDeg = vecAngleDeg(diagSrcW, diagQWorldW);
					diagSrcBodyDeg = vecAngleDeg(diagSrcW, diagQBodyW);
					diagKalWorldDeg = vecAngleDeg(diagKalW, diagQWorldW);
					diagKalBodyDeg = vecAngleDeg(diagKalW, diagQBodyW);
					double qwm = sqrt(diagQWorldW[0] * diagQWorldW[0] + diagQWorldW[1] * diagQWorldW[1] + diagQWorldW[2] * diagQWorldW[2]);
					double vm = sqrt(diagPostTdV[0] * diagPostTdV[0] + diagPostTdV[1] * diagPostTdV[1] + diagPostTdV[2] * diagPostTdV[2]);
					if(driverConfig.streamFrame.poseLogging && now - ks.diagSpaceLastLog >= 0.05
							&& (qwm > 1.0 || vm > 0.5)){
						ks.diagSpaceLastLog = now;
						logSpace = true;
					}
				}
			}
			// gaze aim assist: bend the reported direction toward where
			// the eyes already are. direction only; magnitude preserved.
			{
				double assist = driverConfig.streamFrame.kalmanGazeAssist;
				if(assist > 0.001 && gazeFresh && haveHmdQuat){
					// values > 1 are the TEST regime: they shrink the
					// disagreement angle needed for full gaze takeover
					// (full lock at >= 45/G degrees). G=10 with maxDeg=180
					// locks any throw more than ~4.5 deg off gaze straight
					// onto it — for verifying the pipeline end to end.
					if(assist > 20.0){ assist = 20.0; }
					double mV = sqrt(vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2]);
					if(mV > driverConfig.streamFrame.kalmanGazeMinSpeed){
						// rotate head-space gaze into driver space: g' = q g q*
						vr::HmdQuaternion_t q = hmdQuatForGaze;
						vr::HmdQuaternion_t gq = {0, gazeHead[0], gazeHead[1], gazeHead[2]};
						vr::HmdQuaternion_t qc = {q.w, -q.x, -q.y, -q.z};
						vr::HmdQuaternion_t t1 = QuatMultiply(q, gq);
						vr::HmdQuaternion_t gw = QuatMultiply(t1, qc);
						double gW[3] = { gw.x, gw.y, gw.z };
						double dirV[3] = { vRep[0] / mV, vRep[1] / mV, vRep[2] / mV };
						double d = dirV[0] * gW[0] + dirV[1] * gW[1] + dirV[2] * gW[2];
						if(d > -0.999 && d < 0.999){
							double angBetween = acos(d);
							// v2 weighting (field 2026-08-10: a fixed cap
							// neuters the assist on reversals — a 170-deg
							// wrong throw bent 30 deg is still wrong. small
							// disagreement = the hand is basically right,
							// refine gently; large disagreement at throw
							// speed = the hand data is invalid and gaze,
							// which fixated the target early, takes over.
							// weight ramps with disagreement: t = assist *
							// angle/45deg, clamped to 1 — continuous, no
							// thresholds. maxDeg remains as a pure safety
							// clamp on the final bend.
							double t = assist * (angBetween / (45.0 * 3.14159265358979323846 / 180.0));
							if(t > 1.0){ t = 1.0; }
							double bend = t * angBetween;
							double maxR = driverConfig.streamFrame.kalmanGazeMaxDeg * 3.14159265358979323846 / 180.0;
							if(bend > maxR){ bend = maxR; }
							if(bend > 1e-4){
								t = bend / angBetween;
								double nd[3];
								double nn = 0;
								for(int a2 = 0; a2 < 3; a2++){
									nd[a2] = dirV[a2] * (1.0 - t) + gW[a2] * t;
									nn += nd[a2] * nd[a2];
								}
								nn = sqrt(nn);
								if(nn > 1e-6){
									for(int a2 = 0; a2 < 3; a2++){ vRep[a2] = nd[a2] / nn * mV; }
								}
								ks.gazeBends++;
								double bendDeg = bend * 180.0 / 3.14159265358979323846;
								ks.gazeBendSum += bendDeg;
								if(bendDeg > ks.gazeBendMax){ ks.gazeBendMax = bendDeg; }
								if(!gazeAssistAnnounced){
									gazeAssistAnnounced = true;
									announceGaze = true;
								}
							}
						}
					}
				}
			}
			// fixed-lag smoothed reporting: fuse the stored forward state
			// at t-L with the current state backcast to t-L, and report the
			// WHOLE state (pose + velocities) from that instant, coherent.
			double smoothLag = driverConfig.streamFrame.kalmanSmoothLagMs / 1000.0;
			if(smoothLag > 0.2){ smoothLag = 0.2; }
			double rtsEpochOff = 0; // reported epoch relative to tMeas (<= 0)
			if(caFull && smoothLag > 0.001){
				// ---- fixed-lag RTS (see KalState::rtsN) ----
				ks.rtsRepFrames++;
				int newest = (ks.rtsHead - 1 + KalState::rtsN) % KalState::rtsN;
				double tTargetD = tMeas - smoothLag;
				// find m: newest entry with rtsT[m] <= tTargetD
				int m = -1, depth = 0;
				for(int i = 0; i < ks.rtsCount; i++){
					int idx = (newest - i + KalState::rtsN) % KalState::rtsN;
					if(ks.rtsT[idx] <= tTargetD){ m = idx; depth = i; break; }
				}
				// need the ring to actually cover L, and the entry must be
				// close to the target (a gap in the ring means dropped
				// samples / a reinit; do not extrapolate across it)
				if(m >= 0 && depth >= 1 && tTargetD - ks.rtsT[m] < 0.03){
					double xs[3][3];
					bool ok = true;
					for(int a2 = 0; a2 < 3 && ok; a2++){
						// x_s(newest) = x_f(newest)
						double xsv[3] = { ks.rtsXf[newest][a2][0], ks.rtsXf[newest][a2][1], ks.rtsXf[newest][a2][2] };
						for(int i = 1; i <= depth; i++){
							int k = (newest - i + KalState::rtsN) % KalState::rtsN;   // entry k
							int k1 = (k + 1) % KalState::rtsN;                        // entry k+1
							// transition k -> k+1 (Singer, same form as CaStatePredict)
							double dts = ks.rtsDt[k1];
							double bt = exp(-dts / caTau);
							double gv = caTau * (1.0 - bt);
							double gp = 0.5 * dts * gv;
							// A = Pf(k) * F^T   (Pf sym: [00 01 02 11 12 22])
							const double *Pf = ks.rtsPf[k][a2];
							double P00 = Pf[0], P01 = Pf[1], P02 = Pf[2], P11 = Pf[3], P12 = Pf[4], P22 = Pf[5];
							// F = [[1,dt,gp],[0,1,gv],[0,0,bt]]; (F^T)[j][i] = F[i][j]
							// A[r][c] = sum_j Pf[r][j] * F[c][j]
							double A[3][3] = {
								{ P00 + P01 * dts + P02 * gp,  P01 + P02 * gv,  P02 * bt },
								{ P01 + P11 * dts + P12 * gp,  P11 + P12 * gv,  P12 * bt },
								{ P02 + P12 * dts + P22 * gp,  P12 + P22 * gv,  P22 * bt } };
							// inverse of Pp(k+1) (sym 3x3)
							const double *Pp = ks.rtsPp[k1][a2];
							double a00 = Pp[0], a01 = Pp[1], a02 = Pp[2], a11 = Pp[3], a12 = Pp[4], a22 = Pp[5];
							double c00 = a11 * a22 - a12 * a12;
							double c01 = a02 * a12 - a01 * a22;
							double c02 = a01 * a12 - a02 * a11;
							double det = a00 * c00 + a01 * c01 + a02 * c02;
							if(!(det > 1e-30) || !std::isfinite(det)){ ok = false; break; }
							double c11 = a00 * a22 - a02 * a02;
							double c12 = a01 * a02 - a00 * a12;
							double c22 = a00 * a11 - a01 * a01;
							double id = 1.0 / det;
							double Pi[3][3] = {
								{ c00 * id, c01 * id, c02 * id },
								{ c01 * id, c11 * id, c12 * id },
								{ c02 * id, c12 * id, c22 * id } };
							// C = A * Pi ; d = x_s(k+1) - x_p(k+1)
							double d[3] = { xsv[0] - ks.rtsXp[k1][a2][0], xsv[1] - ks.rtsXp[k1][a2][1], xsv[2] - ks.rtsXp[k1][a2][2] };
							double Pid[3] = {
								Pi[0][0] * d[0] + Pi[0][1] * d[1] + Pi[0][2] * d[2],
								Pi[1][0] * d[0] + Pi[1][1] * d[1] + Pi[1][2] * d[2],
								Pi[2][0] * d[0] + Pi[2][1] * d[1] + Pi[2][2] * d[2] };
							for(int r = 0; r < 3; r++){
								xsv[r] = ks.rtsXf[k][a2][r] + A[r][0] * Pid[0] + A[r][1] * Pid[1] + A[r][2] * Pid[2];
							}
						}
						// entry m sits at rtsT[m] <= target: micro-predict the
						// smoothed state forward to the exact epoch
						double dtf = tTargetD - ks.rtsT[m];
						if(dtf > 1e-6){ CaStatePredict(dtf, caTau, xsv[0], xsv[1], xsv[2]); }
						if(!std::isfinite(xsv[0]) || !std::isfinite(xsv[1])){ ok = false; break; }
						xs[a2][0] = xsv[0]; xs[a2][1] = xsv[1]; xs[a2][2] = xsv[2];
					}
					if(ok){
						ks.rtsFrames++;
						ks.rtsDepthSum += depth;
						double dtf = tTargetD - ks.rtsT[m];
						double wS[3];
						for(int a2 = 0; a2 < 3; a2++){
							ksOutP[a2] = xs[a2][0];
							vRep[a2] = xs[a2][1];
							// angular: filtered state at t-L (own lag ~7ms)
							wS[a2] = ks.rtsW[m][a2];
							wRep[a2] = wS[a2];
						}
						// direction lead on the smoothed vector, about the
						// smoothed w (identity at Td=0; with L ~ skew the
						// smoothed tangent is already the release tangent
						// and Td should be ~0 — kept for A/B parity)
						{
							double dLeadS = driverConfig.streamFrame.kalmanDirLeadMs / 1000.0;
							if(dLeadS < 0){ dLeadS = 0; }
							if(dLeadS > 0.03){ dLeadS = 0.03; }
							double wm = sqrt(wS[0] * wS[0] + wS[1] * wS[1] + wS[2] * wS[2]);
							double mV = sqrt(vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2]);
							if(dLeadS > 0 && wm > 1e-3 && mV > 0.05){
								double ang = wm * dLeadS;
								double kx = wS[0] / wm, ky = wS[1] / wm, kz = wS[2] / wm;
								double c = cos(ang), sn = sin(ang);
								double kv = kx * vRep[0] + ky * vRep[1] + kz * vRep[2];
								double cr[3] = { ky * vRep[2] - kz * vRep[1], kz * vRep[0] - kx * vRep[2], kx * vRep[1] - ky * vRep[0] };
								double vr[3];
								for(int a2 = 0; a2 < 3; a2++){
									double kk = (a2 == 0 ? kx : (a2 == 1 ? ky : kz));
									vr[a2] = vRep[a2] * c + cr[a2] * sn + kk * kv * (1.0 - c);
								}
								for(int a2 = 0; a2 < 3; a2++){ vRep[a2] = vr[a2]; }
							}
						}
						// orientation: stored q at m rotated forward by w*dtf
						vr::HmdQuaternion_t qm = ks.rtsQ[m];
						if(dtf > 1e-6){
							double hf = 0.5 * dtf;
							vr::HmdQuaternion_t dqf = {1.0, wS[0] * hf, wS[1] * hf, wS[2] * hf};
							qm = QuatMultiply(dqf, qm);
							double qn = sqrt(qm.w * qm.w + qm.x * qm.x + qm.y * qm.y + qm.z * qm.z);
							if(qn > 1e-9){ qm.w /= qn; qm.x /= qn; qm.y /= qn; qm.z /= qn; }
						}
						ksOutQ = qm;
						useSmoothOut = true;
						// epoch stamp: latent (default) presents the t-L state
						// as current; honest reports the true epoch (see the
						// kalmanSmoothLagEpoch note in Config.h)
						rtsEpochOff = driverConfig.streamFrame.kalmanSmoothLagEpoch == 1 ? -smoothLag : 0.0;
					}
				}
			}else if(!caFull && smoothLag > 0.001 && ks.histCount > 3){
				// legacy two-estimate fusion for the CV modes (unchanged)
				double tTarget = now - smoothLag;
				int best = -1;
				double bestD = 1e9;
				for(int i = 0; i < ks.histCount; i++){
					double dTi = fabs(ks.histT[i] - tTarget);
					if(dTi < bestD){ bestD = dTi; best = i; }
				}
				if(best >= 0 && bestD < 0.08){
					for(int a2 = 0; a2 < 3; a2++){
						double pBack = ks.p[a2] - ks.v[a2] * smoothLag;
						double pFwd = ks.histP[best][a2];
						double vFwd = ks.histV[best][a2];
						double wFwd = ks.histW[best][a2];
						ksOutP[a2] = 0.5 * (pFwd + pBack);
						vRep[a2] = 0.5 * (vFwd + vRep[a2]);
						wRep[a2] = 0.5 * (wFwd + wRep[a2]);
					}
					// orientation: backcast current q by -w*L, nlerp with
					// the stored q (hemisphere safe)
					double hb = -0.5 * smoothLag;
					vr::HmdQuaternion_t dqb = {1.0, ks.w[0] * hb, ks.w[1] * hb, ks.w[2] * hb};
					vr::HmdQuaternion_t qBack = QuatMultiply(dqb, ks.q);
					vr::HmdQuaternion_t qF = ks.histQ[best];
					double dotq = qF.w * qBack.w + qF.x * qBack.x + qF.y * qBack.y + qF.z * qBack.z;
					double sg = dotq < 0 ? -1.0 : 1.0;
					vr::HmdQuaternion_t qs = {
						0.5 * (sg * qF.w + qBack.w), 0.5 * (sg * qF.x + qBack.x),
						0.5 * (sg * qF.y + qBack.y), 0.5 * (sg * qF.z + qBack.z)};
					double qsn = sqrt(qs.w * qs.w + qs.x * qs.x + qs.y * qs.y + qs.z * qs.z);
					if(qsn > 1e-9){ qs.w /= qsn; qs.x /= qsn; qs.y /= qsn; qs.z /= qsn; }
					ksOutQ = qs;
					useSmoothOut = true;
				}
			}
			// reported-velocity sanity clamp (belt and suspenders behind
			// the payload gate): no human hand exceeds 25 m/s (the
			// teleport guard's own constant); 50 m/s is double that
			// margin. engages ONLY on insanity — a state velocity this
			// wrong means an unmodeled corruption slipped every gate,
			// and handing it to the runtime's forward prediction
			// teleports the rendered hand. pure output bound, counted
			// for KALDIAG; the state itself is never touched.
			{
				double vm2 = vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2];
				if(!std::isfinite(vm2)){
					vRep[0] = 0; vRep[1] = 0; vRep[2] = 0;
					ks.vClampN++;
				}else if(vm2 > 50.0 * 50.0){
					double sc = 50.0 / sqrt(vm2);
					vRep[0] *= sc; vRep[1] *= sc; vRep[2] *= sc;
					ks.vClampN++;
				}
			}
			// Capture the raw input before replacing DriverPose_t below.
			for(int a2 = 0; a2 < 3; a2++){ diagRawPos[a2] = pose.vecPosition[a2]; }
			diagTMeas = tMeas;
			diagOffsetIn = tOff;
			diagFresh = (!dropSample && !dupRepeat && !posOnlyFreeze) ? 1 : 0;
			// Covariance telemetry for the reported "warm-up" question.
			// A fixed CA model should not learn across throws, but the
			// duplicate policy is state-dependent; a long still period could
			// in principle leave the filter much more confident than a moving
			// period.  Log the actual CA uncertainty so that hypothesis is
			// directly testable rather than judged from feel.
			if(caFull){
				double pVar = 0, vVar = 0, aVar = 0;
				for(int a2 = 0; a2 < 3; a2++){
					double pv = ks.P6[a2][0] > 0 ? ks.P6[a2][0] : 0;
					double vv = ks.P6[a2][3] > 0 ? ks.P6[a2][3] : 0;
					double av = ks.P6[a2][5] > 0 ? ks.P6[a2][5] : 0;
					pVar += pv;
					vVar += vv;
					aVar += av;
				}
				diagPosSigma = sqrt(pVar / 3.0);
				diagVelSigma = sqrt(vVar / 3.0);
				diagAccSigma = sqrt(aVar / 3.0);
			}

			ks.repHave = true;
			for(int a2 = 0; a2 < 3; a2++){
				pose.vecPosition[a2] = (useSmoothOut ? ksOutP[a2] : ks.p[a2]) + ks.v[a2] * lead;
				pose.vecVelocity[a2] = vRep[a2];
				pose.vecAngularVelocity[a2] = wRep[a2];
				ks.repV[a2] = vRep[a2];
				ks.repW[a2] = wRep[a2];
				diagStateP[a2] = ks.p[a2];
				diagStateV[a2] = ks.v[a2];
				diagStateA[a2] = caFull ? ks.ca[a2] : 0.0;
			}
			if(caFull){
				// the lead honestly includes the acceleration state
				if(lead > 0){
					for(int a2 = 0; a2 < 3; a2++){
						pose.vecPosition[a2] += 0.5 * ks.ca[a2] * lead * lead;
					}
				}
				// optional: hand the runtime's forward prediction the
				// acceleration states (own toggle — overshoot risk)
				if(driverConfig.streamFrame.kalmanCaReportAccel){
					for(int a2 = 0; a2 < 3; a2++){
						pose.vecAcceleration[a2] = ks.ca[a2];
						pose.vecAngularAcceleration[a2] = ks.caW[a2];
					}
				}
			}
			if(useSmoothOut){
				pose.qRotation = ksOutQ;
			}
			if(!useSmoothOut){
				if(lead > 0){
					double hl = 0.5 * lead;
					vr::HmdQuaternion_t dql = {1.0, ks.w[0] * hl, ks.w[1] * hl, ks.w[2] * hl};
					vr::HmdQuaternion_t ql = QuatMultiply(dql, ks.q);
					double n3 = sqrt(ql.w * ql.w + ql.x * ql.x + ql.y * ql.y + ql.z * ql.z);
					if(n3 > 1e-9){ ql.w /= n3; ql.x /= n3; ql.y /= n3; ql.z /= n3; }
					pose.qRotation = ql;
				}else{
					pose.qRotation = ks.q;
				}
			}
			// reported angular velocity frame (kalmanAngularOutFrame):
			// 0 world (state as-is), 1 body (q^-1 w q using the pose
			// actually reported), 2 zero. ks.repW stays world-frame so
			// the KALSPACE/KALVSRC diagnostics keep their meaning.
			{
				int wFrame = driverConfig.streamFrame.kalmanAngularOutFrame;
				if(wFrame == 2){
					pose.vecAngularVelocity[0] = 0; pose.vecAngularVelocity[1] = 0; pose.vecAngularVelocity[2] = 0;
					pose.vecAngularAcceleration[0] = 0; pose.vecAngularAcceleration[1] = 0; pose.vecAngularAcceleration[2] = 0;
				}else if(wFrame == 1){
					vr::HmdQuaternion_t qInv = pose.qRotation;
					qInv.x = -qInv.x; qInv.y = -qInv.y; qInv.z = -qInv.z;
					double wIn[3] = {pose.vecAngularVelocity[0], pose.vecAngularVelocity[1], pose.vecAngularVelocity[2]};
					double wB[3];
					QuatRotateVector(qInv, wIn, wB);
					pose.vecAngularVelocity[0] = wB[0]; pose.vecAngularVelocity[1] = wB[1]; pose.vecAngularVelocity[2] = wB[2];
					// angular acceleration (only nonzero with kalmanCaReportAccel)
					// lives in the same frame as angular velocity
					double aIn[3] = {pose.vecAngularAcceleration[0], pose.vecAngularAcceleration[1], pose.vecAngularAcceleration[2]};
					if(aIn[0] != 0 || aIn[1] != 0 || aIn[2] != 0){
						double aB[3];
						QuatRotateVector(qInv, aIn, aB);
						pose.vecAngularAcceleration[0] = aB[0]; pose.vecAngularAcceleration[1] = aB[1]; pose.vecAngularAcceleration[2] = aB[2];
					}
				}
			}

			// Epoch contract:
			//   ks.p/q are estimates at the accepted measurement epoch tMeas.
			//   lead moves the reported state to tMeas + lead.
			//   fixed-lag output instead represents receipt-now - smoothLag.
			// DriverPose_t::poseTimeOffset must describe that same epoch or the
			// runtime predicts an already/time-shifted state again.
			if(useSmoothOut && caFull){
				// RTS epoch: tMeas - L on the device clock
				pose.poseTimeOffset = (devTime ? tOff : 0.0) + rtsEpochOff + lead;
			}else if(useSmoothOut){
				pose.poseTimeOffset = -smoothLag + lead;
			}else if(devTime){
				pose.poseTimeOffset = tOff + lead;
			}else{
				pose.poseTimeOffset = lead;
			}
			diagOffsetOut = pose.poseTimeOffset;
			for(int a2 = 0; a2 < 3; a2++){ diagSubmitP[a2] = pose.vecPosition[a2]; }
			// submitted-position ring (receipt clock): what a pose-history
			// throw estimator sees from the driver side (before the
			// runtime's own photon prediction). read by RELDIAG.
			{
				int sh = ks.subHead;
				ks.subT[sh] = now;
				for(int a2 = 0; a2 < 3; a2++){ ks.subP[sh][a2] = pose.vecPosition[a2]; }
				ks.subHead = (sh + 1) % KalState::subN;
				if(ks.subCount < KalState::subN){ ks.subCount++; }
			}

			if(rawFdValid){
				double outXZ2 = vRep[0] * vRep[0] + vRep[2] * vRep[2];
				if(outXZ2 > 0.25){
					double dotXZ = rawFd[0] * vRep[0] + rawFd[2] * vRep[2];
					double crossY = rawFd[2] * vRep[0] - rawFd[0] * vRep[2];
					diagSignedDirDeg = atan2(crossY, dotXZ) * 180.0 / 3.14159265358979323846;
					diagSignedDirValid = true;
				}
			}
			{
				// announce on ANY kalman knob change (2026-08-11: the
				// P-sweep sessions were invisible in the log because
				// only qa re-announced — never again)
				double sig = driverConfig.streamFrame.kalmanProcessAccel
					+ driverConfig.streamFrame.kalmanPosNoiseMm * 1e3
					+ driverConfig.streamFrame.kalmanProcessAngAccel * 1e5
					+ driverConfig.streamFrame.kalmanOriNoiseDeg * 1e8
					+ driverConfig.streamFrame.kalmanLeadMs * 1e10
					+ driverConfig.streamFrame.kalmanDupMode * 1e12
					+ driverConfig.streamFrame.kalmanDupRScale * 1e13
					+ (driverConfig.streamFrame.kalmanDeviceTime ? 1e15 : 0)
					+ driverConfig.streamFrame.kalmanDupCoastMaxMs * 1e16;
				// CA knobs get their OWN signature: appending them to the
				// legacy sum would drop below double epsilon next to the
				// 1e16-scale terms and never re-announce (the exact
				// failure the 2026-08-11 note warns about)
				double sigCa = velocityFixMode
					+ driverConfig.streamFrame.kalmanCaJerk * 1e2
					+ driverConfig.streamFrame.kalmanCaAngJerk * 1e5
					+ driverConfig.streamFrame.kalmanCaPosNoiseMm * 1e8
					+ driverConfig.streamFrame.kalmanCaOriNoiseDeg * 1e10
					+ driverConfig.streamFrame.kalmanCaAccelTauMs * 1e11
					+ driverConfig.streamFrame.kalmanCaMagJerk * 1e-3
					+ driverConfig.streamFrame.kalmanCaMagAccelTauMs * 1e-7
					+ (driverConfig.streamFrame.kalmanCaReportAccel ? 0.1 : 0)
					// dirLead rides the CA sig (applies to every kalman
					// mode, but this sum's epsilon floor ~1.5e-3 leaves
					// ms-scale terms fully representable, unlike the
					// 1e16-scale legacy sig)
					+ driverConfig.streamFrame.kalmanDirLeadMs * 17.0
					+ driverConfig.streamFrame.kalmanLossCoastMs * 0.31
					+ (driverConfig.streamFrame.kalmanDirLeadAdaptive ? 0.031 : 0)
					+ driverConfig.streamFrame.kalmanDirLeadBaseMs * 0.71
					+ driverConfig.streamFrame.kalmanDirLeadWMs * 11.0
					+ (driverConfig.streamFrame.kalmanAdaptiveR ? 0.0071 : 0)
					+ driverConfig.streamFrame.kalmanAdaptiveRMaxDiv * 0.013
					+ (caExactCov ? 0.0017 : 0)
					+ (driverConfig.streamFrame.kalmanPosFreeze3dof ? 0.00073 : 0)
					+ driverConfig.streamFrame.kalmanPosFreezeVelDecayMs * 0.000031
					+ driverConfig.streamFrame.kalmanAngularOutFrame * 0.00091
					+ driverConfig.streamFrame.kalmanFreezeCoastTurn * 0.00013;
				// grip sig: cm-scale terms would vanish below the CA sig's
				// epsilon floor (~1e-2 next to its 1e13-scale terms)
				double sigGrip = (gripEnable ? 1000.0 : 0.0) + gripBlend * 100.0
					+ gripLocal[0] * 1.0 + gripLocal[1] * 7.0 + gripLocal[2] * 13.0
					+ (gripHave ? 0.001 : 0.0);
				if(!ks.announced || ks.lastQa != sig || ks.lastCaSig != sigCa
						|| ks.lastGripSig != sigGrip){
					ks.announced = true;
					ks.lastQa = sig;
					ks.lastCaSig = sigCa;
					ks.lastGripSig = sigGrip;
					announceKalman = true;
				}
			}
			if(driverConfig.streamFrame.poseLogging && now - ks.lastDiagLog >= 2.0){
				ks.lastDiagLog = now;
				diagNis = ks.nisEma;
				diagStepMax = ks.stepMax;
				diagFrozen = ks.stepFrozen;
				diagDup = ks.dupSkipped;
				diagP3d = ks.posFreeze3dof;
				diagBends = ks.gazeBends;
				diagBendMean = ks.gazeBends > 0 ? ks.gazeBendSum / ks.gazeBends : 0.0;
				diagBendMax = ks.gazeBendMax;
				diagDtBack = ks.dtBack;
				diagDtMean = ks.dtN > 0 ? ks.dtSumMs / ks.dtN : 0.0;
				diagDtMax = ks.dtMaxMs;
				diagCoastMax = ks.coastMaxMs;
				diagANis = ks.nisAEma;
				diagFdtMean = ks.fdtN > 0 ? ks.fdtSumMs / ks.fdtN : 0.0;
				diagFdtMax = ks.fdtMaxMs;
				diagAccMax = ks.accMax;
				diagWAccMax = ks.wAccMax;
				diagAccNZ = ks.accNZ;
				diagLossRuns = ks.lossRuns;
				diagLossMs = ks.lossMsSum;
				diagTeleports = ks.teleports;
				diagCaAcc = ks.caAccPk;
				diagCaWAcc = ks.caWAccPk;
				diagGarbage = ks.garbageN;
				diagVClamp = ks.vClampN;
				diagRtsFrames = ks.rtsFrames;
				diagRtsRep = ks.rtsRepFrames;
				diagRtsDepth = ks.rtsFrames > 0 ? ks.rtsDepthSum / ks.rtsFrames : 0.0;
				ks.rtsFrames = 0; ks.rtsRepFrames = 0; ks.rtsDepthSum = 0;
				ks.garbageN = 0;
				ks.vClampN = 0;
				diagRDiv = ks.rDivPk;
				diagRADiv = ks.rADivPk;
				ks.rDivPk = 1.0;
				ks.rADivPk = 1.0;
				ks.caAccPk = 0;
				ks.caWAccPk = 0;
				ks.stepMax = 0;
				ks.stepFrozen = 0;
				ks.dupSkipped = 0;
				ks.posFreeze3dof = 0;
				diagTurnSteps = ks.turnCoastSteps;
				ks.turnCoastSteps = 0;
				ks.gazeBends = 0;
				ks.gazeBendSum = 0;
				ks.gazeBendMax = 0;
				ks.dtBack = 0;
				ks.dtSumMs = 0;
				ks.dtN = 0;
				ks.dtMaxMs = 0;
				ks.coastMaxMs = 0;
				ks.fdtSumMs = 0;
				ks.fdtN = 0;
				ks.fdtMaxMs = 0;
				ks.accMax = 0;
				ks.wAccMax = 0;
				ks.accNZ = 0;
				ks.lossRuns = 0;
				ks.lossMsSum = 0;
				ks.teleports = 0;
				logKalDiag = true;
			}
			}
			if(logGarbage){
				DriverLog("PoseLog: KALGARBAGE id=%u corrupt payload rejected pos=(%.3g, %.3g, %.3g) |q|2=%.3g",
					openVRID, gbPos[0], gbPos[1], gbPos[2], gbQn2);
			}
			if(stuckEnterLog){
				DriverLog("PoseLog: STUCKDIAG id=%u OPEN div=%.2fm v=%.2fm/s (state gliding past measurement)",
					openVRID, stuckLogDiv, stuckLogV0);
			}
			if(stuckExitLog){
				DriverLog("PoseLog: STUCKDIAG id=%u CLOSE dur=%.0fms maxDiv=%.2fm vEntry=%.2fm/s",
					openVRID, stuckLogMs, stuckLogMax, stuckLogV0);
			}
			if(announceKalman){
			// outside the lock — lock discipline
			DriverLog("VelocityFix: kalman mode active id=%u qa=%.0f rp=%.1fmm qaA=%.0f ro=%.2fdeg lead=%.0fms dirLead=%.0fms dup=%d dupR=%.0f devT=%d cap=%.0f log=%d p3d=%d",
				openVRID, driverConfig.streamFrame.kalmanProcessAccel,
				driverConfig.streamFrame.kalmanPosNoiseMm,
				driverConfig.streamFrame.kalmanProcessAngAccel,
				driverConfig.streamFrame.kalmanOriNoiseDeg,
				driverConfig.streamFrame.kalmanLeadMs,
				driverConfig.streamFrame.kalmanDirLeadMs,
				driverConfig.streamFrame.kalmanDupMode,
				driverConfig.streamFrame.kalmanDupRScale,
				driverConfig.streamFrame.kalmanDeviceTime ? 1 : 0,
				driverConfig.streamFrame.kalmanDupCoastMaxMs,
				driverConfig.streamFrame.poseLogging ? 1 : 0, (int)driverConfig.streamFrame.kalmanPosFreeze3dof);
			DriverLog("VelocityFix: SWORDARC knobs id=%u angularOutFrame=%s freezeCoastTurn=%.2f",
				openVRID,
				driverConfig.streamFrame.kalmanAngularOutFrame == 1 ? "body" : (driverConfig.streamFrame.kalmanAngularOutFrame == 2 ? "zero" : "world"),
				driverConfig.streamFrame.kalmanFreezeCoastTurn);
			if(driverConfig.streamFrame.kalmanDirLeadAdaptive || driverConfig.streamFrame.kalmanAdaptiveR){
				DriverLog("VelocityFix: ADAPT dirLeadAdaptive=%d base=%.1fms slope=%.2fms/rads adaptR=%d maxDiv=%.0f",
					driverConfig.streamFrame.kalmanDirLeadAdaptive ? 1 : 0,
					driverConfig.streamFrame.kalmanDirLeadBaseMs,
					driverConfig.streamFrame.kalmanDirLeadWMs,
					driverConfig.streamFrame.kalmanAdaptiveR ? 1 : 0,
					driverConfig.streamFrame.kalmanAdaptiveRMaxDiv);
			}
			if(velocityFixMode >= 5){
				DriverLog("VelocityFix: CA %s active id=%u J=%.2f Ja=%.0f caP=%.2fmm caO=%.2fdeg tau=%.0fms magJ=%.0f magTau=%.0fms reportAccel=%d excov=%d",
					velocityFixMode == 6 ? "FULL" : "MAGNITUDE",
					openVRID,
					driverConfig.streamFrame.kalmanCaJerk,
					driverConfig.streamFrame.kalmanCaAngJerk,
					driverConfig.streamFrame.kalmanCaPosNoiseMm,
					driverConfig.streamFrame.kalmanCaOriNoiseDeg,
					driverConfig.streamFrame.kalmanCaAccelTauMs,
					driverConfig.streamFrame.kalmanCaMagJerk,
					driverConfig.streamFrame.kalmanCaMagAccelTauMs,
					driverConfig.streamFrame.kalmanCaReportAccel ? 1 : 0,
					driverConfig.streamFrame.kalmanCaExactCov ? 1 : 0);
			}
			if(gripHave || driverConfig.streamFrame.kalmanGripEnable){
				DriverLog("VelocityFix: GRIP compensator id=%u %s blend=%.2f r=(%.2f, %.2f, %.2f)cm |r|=%.1fcm%s",
					openVRID,
					driverConfig.streamFrame.kalmanGripEnable ? "ENABLED" : "shadow-only",
					gripBlend,
					gripLocal[0] * 100.0, gripLocal[1] * 100.0, gripLocal[2] * 100.0,
					sqrt(gripLocal[0] * gripLocal[0] + gripLocal[1] * gripLocal[1]
						+ gripLocal[2] * gripLocal[2]) * 100.0,
					gripHave ? "" : " [INERT: r outside 0.1-30cm or hand unknown]");
			}
			}
			if(announceGaze){
				DriverLog("VelocityFix: gaze aim assist ENGAGED id=%u strength=%.2f maxDeg=%.0f", openVRID,
					driverConfig.streamFrame.kalmanGazeAssist, driverConfig.streamFrame.kalmanGazeMaxDeg);
			}
			if(logSpace){
				DriverLog("PoseLog: KALSPACE id=%u qdt=%.2fms srcW=(%.2f,%.2f,%.2f) qWorldW=(%.2f,%.2f,%.2f) qBodyW=(%.2f,%.2f,%.2f) kalW=(%.2f,%.2f,%.2f) errSrcWorld=%.1fdeg errSrcBody=%.1fdeg errKalWorld=%.1fdeg errKalBody=%.1fdeg preTdV=(%.2f,%.2f,%.2f) postTdV=(%.2f,%.2f,%.2f) tdCmd=%.2fdeg tdBend=%.2fdeg curveW=(%.2f,%.2f,%.2f) curveVsKal=%.1fdeg localX=(%.2f,%.2f,%.2f) localY=(%.2f,%.2f,%.2f) localZ=(%.2f,%.2f,%.2f)",
					openVRID, diagQDtMs,
					diagSrcW[0], diagSrcW[1], diagSrcW[2],
					diagQWorldW[0], diagQWorldW[1], diagQWorldW[2],
					diagQBodyW[0], diagQBodyW[1], diagQBodyW[2],
					diagKalW[0], diagKalW[1], diagKalW[2],
					diagSrcWorldDeg, diagSrcBodyDeg, diagKalWorldDeg, diagKalBodyDeg,
					diagPreTdV[0], diagPreTdV[1], diagPreTdV[2],
					diagPostTdV[0], diagPostTdV[1], diagPostTdV[2],
					diagTdCmdDeg, diagTdBendDeg,
					diagCurveW[0], diagCurveW[1], diagCurveW[2], diagCurveVsKalDeg,
					diagLocalX[0], diagLocalX[1], diagLocalX[2],
					diagLocalY[0], diagLocalY[1], diagLocalY[2],
					diagLocalZ[0], diagLocalZ[1], diagLocalZ[2]);
				DriverLog("PoseLog: KALVSRC id=%u p3d=%d reacq=%d fresh=%d srcV=(%.3f,%.3f,%.3f) fdV=%s(%.3f,%.3f,%.3f) kalV=(%.3f,%.3f,%.3f) preLeadV=(%.3f,%.3f,%.3f) errSrcFd=%s%.1fdeg magSrcFd=%s%.2f errSrcKal=%.1fdeg",
					openVRID, diagP3dNow, diagReacqNow, diagFresh,
					diagSrcV[0], diagSrcV[1], diagSrcV[2],
					diagFdTrusted ? "" : "NA/", diagFdV[0], diagFdV[1], diagFdV[2],
					diagKalV[0], diagKalV[1], diagKalV[2],
					diagPreTdV[0], diagPreTdV[1], diagPreTdV[2],
					diagFdTrusted ? "" : "NA/", diagFdTrusted ? diagSrcFdDeg : 0.0,
					diagSrcFdMagRatio >= 0 ? "" : "NA/", diagSrcFdMagRatio >= 0 ? diagSrcFdMagRatio : 0.0,
					diagSrcKalDeg);
				DriverLog("PoseLog: KALVSPACE id=%u qwd=(%.6f,%.6f,%.6f,%.6f) qwdAng=%.2fdeg raw=(%.3f,%.3f,%.3f) qwdV=(%.3f,%.3f,%.3f) invV=(%.3f,%.3f,%.3f) fd=%s(%.3f,%.3f,%.3f) errRaw=%s%.1fdeg errQwd=%s%.1fdeg errInv=%s%.1fdeg",
					openVRID,
					diagQwd.w, diagQwd.x, diagQwd.y, diagQwd.z, diagQwdAngleDeg,
					diagSrcV[0], diagSrcV[1], diagSrcV[2],
					diagSrcVQwd[0], diagSrcVQwd[1], diagSrcVQwd[2],
					diagSrcVQwdInv[0], diagSrcVQwdInv[1], diagSrcVQwdInv[2],
					diagFdTrusted ? "" : "NA/", diagFdV[0], diagFdV[1], diagFdV[2],
					diagFdTrusted ? "" : "NA/", diagFdTrusted ? diagSrcFdDeg : 0.0,
					diagFdTrusted ? "" : "NA/", diagFdTrusted ? diagSrcQwdFdDeg : 0.0,
					diagFdTrusted ? "" : "NA/", diagFdTrusted ? diagSrcQwdInvFdDeg : 0.0);
			}
			if(logKalDiag){
				// tuning guide: NIS ~ 1 means the noise models match
				// reality; sustained > 3 = too stiff; < 0.3 = too loose
				DriverLog("PoseLog: KALDIAG id=%u nis=%.2f aNis=%.2f stepMax=%.1fmm frozenSteps=%d dupSkipped=%d dtBack=%d dtMean=%.2fms dtMax=%.1fms fdtMean=%.2fms fdtMax=%.1fms coastMax=%.0fms gazeBends=%d bendMean=%.1fdeg bendMax=%.1fdeg accMax=%.2f wAccMax=%.1f accNZ=%d loss=%d lossMs=%.0f tp=%d caAcc=%.1f caWAcc=%.1f garbage=%d vClamp=%d rDiv=%.1f rADiv=%.1f p3d=%d rts=%d/%d rtsDepth=%.1f turn=%d", openVRID, diagNis, diagANis, diagStepMax * 1000.0, diagFrozen, diagDup, diagDtBack, diagDtMean, diagDtMax, diagFdtMean, diagFdtMax, diagCoastMax, diagBends, diagBendMean, diagBendMax, diagAccMax, diagWAccMax, diagAccNZ, diagLossRuns, diagLossMs, diagTeleports, diagCaAcc, diagCaWAcc, diagGarbage, diagVClamp, diagRDiv, diagRADiv, diagP3d, diagRtsFrames, diagRtsRep, diagRtsDepth, diagTurnSteps);
				DriverLog("PoseLog: KALEPOCH id=%u fresh=%d tMeas=%.6f offIn=%.2fms offOut=%.2fms raw=(%.4f,%.4f,%.4f) stateP=(%.4f,%.4f,%.4f) submitP=(%.4f,%.4f,%.4f) submitQ=(%.5f,%.5f,%.5f,%.5f) stateV=(%.3f,%.3f,%.3f) stateA=(%.1f,%.1f,%.1f) sigP=%.2fmm sigV=%.3fm/s sigA=%.1fm/s2 dirYaw=%s%.2fdeg",
					openVRID, diagFresh, diagTMeas, diagOffsetIn * 1000.0, diagOffsetOut * 1000.0,
					diagRawPos[0], diagRawPos[1], diagRawPos[2],
					diagStateP[0], diagStateP[1], diagStateP[2],
					diagSubmitP[0], diagSubmitP[1], diagSubmitP[2],
					pose.qRotation.w, pose.qRotation.x, pose.qRotation.y, pose.qRotation.z,
					diagStateV[0], diagStateV[1], diagStateV[2],
					diagStateA[0], diagStateA[1], diagStateA[2],
					diagPosSigma * 1000.0, diagVelSigma, diagAccSigma,
					diagSignedDirValid ? "" : "NA/", diagSignedDirValid ? diagSignedDirDeg : 0.0);
			}
		}
		// ==== end kalman mode ====
		if(DeriveMotion(openVRID, pose, derivedVel, derivedAng, secantVel, secantAng)){
			double derivedSpeed = sqrt(derivedVel[0] * derivedVel[0] + derivedVel[1] * derivedVel[1] + derivedVel[2] * derivedVel[2]);
			double derivedAngSpeed = sqrt(derivedAng[0] * derivedAng[0] + derivedAng[1] * derivedAng[1] + derivedAng[2] * derivedAng[2]);
			double reportedSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
				+ pose.vecVelocity[1] * pose.vecVelocity[1]
				+ pose.vecVelocity[2] * pose.vecVelocity[2]);
			double reportedAngSpeed = sqrt(pose.vecAngularVelocity[0] * pose.vecAngularVelocity[0]
				+ pose.vecAngularVelocity[1] * pose.vecAngularVelocity[1]
				+ pose.vecAngularVelocity[2] * pose.vecAngularVelocity[2]);
			// one weight for both channels so v and w stay phase consistent
			// (games compute released object velocity as v + w x gripOffset).
			// the 0.15 factor converts rad/s to an effective m/s so wrist
			// flick throws (w dominant, little linear motion) also engage.
			double linS = derivedSpeed > reportedSpeed ? derivedSpeed : reportedSpeed;
			double angS = derivedAngSpeed > reportedAngSpeed ? derivedAngSpeed : reportedAngSpeed;
			double sEff = linS > 0.15 * angS ? linS : 0.15 * angS;
			if(classicMode){
				sEff = linS; // classic: linear speeds only, as in v3
			}
			if(deriveMode){
				// derive: the runtime's velocity is discarded outright and
				// the pose-derived estimate is reported at all speeds — no
				// engage gate, no blend. caps fall back to the report (the
				// estimator flags a teleport, not a real motion, there).
				if(derivedSpeed < 20.0 && derivedAngSpeed < 60.0){
					// speed-adaptive smoothing: the endpoint-derivative
					// estimator is low lag but noisy, and with no engage
					// gate that noise trembles held objects. one alpha for
					// both channels keeps v and w phase consistent.
					double tauSlow = driverConfig.streamFrame.deriveSmoothTauSlowMs / 1000.0;
					double tauFast = driverConfig.streamFrame.deriveSmoothTauFastMs / 1000.0;
					double spLow = driverConfig.streamFrame.deriveSmoothSpeedLow;
					double spHigh = driverConfig.streamFrame.deriveSmoothSpeedHigh;
					if(tauSlow < 0.001){ tauSlow = 0.001; }
					if(tauFast < 0.001){ tauFast = 0.001; }
					if(spHigh <= spLow + 0.01){ spHigh = spLow + 0.01; }
					double sAdapt = derivedSpeed + 0.15 * derivedAngSpeed;
					double m = (sAdapt - spLow) / (spHigh - spLow);
					if(m < 0){ m = 0; }
					if(m > 1){ m = 1; }
					m = m * m * (3.0 - 2.0 * m);
					double tau = tauSlow + (tauFast - tauSlow) * m;
					// split-direction knobs read outside the lock (relaxed
					// consistency is fine: they only shape this frame's math)
					bool splitLin = driverConfig.streamFrame.deriveSplitDirLinear;
					bool splitAng = driverConfig.streamFrame.deriveSplitDirAngular;
					double dirWindow = driverConfig.streamFrame.deriveDirWindowMs / 1000.0;
					if(dirWindow < 0.005){ dirWindow = 0.005; }
					if(dirWindow > 0.2){ dirWindow = 0.2; }
					double dirPow = driverConfig.streamFrame.deriveDirWeightPow;
					if(dirPow < 0.0){ dirPow = 0.0; }
					if(dirPow > 6.0){ dirPow = 6.0; }
					bool logSplit = false;
					int dirSource = driverConfig.streamFrame.deriveDirSource;
					if(dirSource < 0 || dirSource > 2){ dirSource = 1; }
					bool logDirDiag = false;
					double diagOut[3] = {0, 0, 0};
					double diagRaw[3] = {0, 0, 0};
					double diagAngOut[3] = {0, 0, 0};
					{
						std::lock_guard<std::mutex> filterGuard(deriveFilterLock);
						DeriveFilterState &fs = deriveFilterStates[openVRID];
						double fdt = now - fs.time;
						if(!fs.have || fdt <= 0 || fdt > 0.1){
							for(int a = 0; a < 3; a++){
								fs.vel[a] = derivedVel[a];
								fs.ang[a] = derivedAng[a];
							}
							fs.magEma = derivedSpeed;
							fs.angMagEma = derivedAngSpeed;
							// a filter reset means a time gap or teleport:
							// the direction ring's history is equally stale
							fs.dirCount = 0;
							fs.dirHead = 0;
						}else{
							double alpha = 1.0 - exp(-fdt / tau);
							// angular channel: shared alpha (original, phase
							// locked) or its own adaptive alpha from the
							// angular knobs when separate smoothing is on
							double alphaAng = alpha;
							if(driverConfig.streamFrame.deriveSmoothAngSeparate){
								double aTauSlow = driverConfig.streamFrame.deriveSmoothAngTauSlowMs / 1000.0;
								double aTauFast = driverConfig.streamFrame.deriveSmoothAngTauFastMs / 1000.0;
								double aLow = driverConfig.streamFrame.deriveSmoothAngSpeedLow;
								double aHigh = driverConfig.streamFrame.deriveSmoothAngSpeedHigh;
								if(aTauSlow < 0.001){ aTauSlow = 0.001; }
								if(aTauFast < 0.001){ aTauFast = 0.001; }
								if(aHigh <= aLow + 0.01){ aHigh = aLow + 0.01; }
								double mA = (derivedAngSpeed - aLow) / (aHigh - aLow);
								if(mA < 0){ mA = 0; }
								if(mA > 1){ mA = 1; }
								mA = mA * mA * (3.0 - 2.0 * mA);
								double tauAng = aTauSlow + (aTauFast - aTauSlow) * mA;
								alphaAng = 1.0 - exp(-fdt / tauAng);
							}
							for(int a = 0; a < 3; a++){
								fs.vel[a] += alpha * (derivedVel[a] - fs.vel[a]);
								fs.ang[a] += alphaAng * (derivedAng[a] - fs.ang[a]);
							}
							// scalar magnitude channels, matching alphas
							fs.magEma += alpha * (derivedSpeed - fs.magEma);
							fs.angMagEma += alphaAng * (derivedAngSpeed - fs.angMagEma);
						}
						fs.time = now;
						fs.have = true;
						// always push the RAW estimate into the direction
						// ring (cheap), so toggling split live mid-session
						// starts with a warm window
						fs.dirTime[fs.dirHead] = now;
						for(int a = 0; a < 3; a++){
							fs.dirVel[fs.dirHead][a] = derivedVel[a];
							fs.dirAng[fs.dirHead][a] = derivedAng[a];
						}
						fs.dirHead = (fs.dirHead + 1) % DeriveFilterState::dirRingSize;
						if(fs.dirCount < DeriveFilterState::dirRingSize){ fs.dirCount++; }
						double outVel[3] = { fs.vel[0], fs.vel[1], fs.vel[2] };
						double outAng[3] = { fs.ang[0], fs.ang[1], fs.ang[2] };
						if(splitLin || splitAng){
							// direction basis per source. window (0): the
							// speed^pow weighted sum of ring samples —
							// kept for A/B, but the raw estimates' noise
							// is correlated across the window so it helps
							// little. secant (1): raw displacement across
							// the derive ring. runtime (2): vrlink's own
							// reported vector.
							double basisVel[3] = { 0, 0, 0 };
							double basisAng[3] = { 0, 0, 0 };
							if(dirSource == 1){
								for(int a = 0; a < 3; a++){
									basisVel[a] = secantVel[a];
									basisAng[a] = secantAng[a];
								}
							}else if(dirSource == 2){
								for(int a = 0; a < 3; a++){
									basisVel[a] = runtimeVel[a];
									basisAng[a] = runtimeAng[a];
								}
							}else{
								for(int i = 0; i < fs.dirCount; i++){
									int idx = (fs.dirHead + DeriveFilterState::dirRingSize - 1 - i) % DeriveFilterState::dirRingSize;
									if(now - fs.dirTime[idx] > dirWindow){
										break;
									}
									double sv = sqrt(fs.dirVel[idx][0] * fs.dirVel[idx][0]
										+ fs.dirVel[idx][1] * fs.dirVel[idx][1]
										+ fs.dirVel[idx][2] * fs.dirVel[idx][2]);
									double sa = sqrt(fs.dirAng[idx][0] * fs.dirAng[idx][0]
										+ fs.dirAng[idx][1] * fs.dirAng[idx][1]
										+ fs.dirAng[idx][2] * fs.dirAng[idx][2]);
									double wv = pow(sv, dirPow);
									double wa = pow(sa, dirPow);
									for(int a = 0; a < 3; a++){
										basisVel[a] += fs.dirVel[idx][a] * wv;
										basisAng[a] += fs.dirAng[idx][a] * wa;
									}
								}
							}
							if(splitLin){
								double mag = driverConfig.streamFrame.deriveMagSource == 1
									? fs.magEma
									: sqrt(fs.vel[0] * fs.vel[0] + fs.vel[1] * fs.vel[1] + fs.vel[2] * fs.vel[2]);
								double dn = sqrt(basisVel[0] * basisVel[0] + basisVel[1] * basisVel[1] + basisVel[2] * basisVel[2]);
								if(dn > 1e-9 && mag > 1e-9){
									for(int a = 0; a < 3; a++){
										outVel[a] = basisVel[a] / dn * mag;
									}
								}
							}
							if(splitAng){
								double magA = driverConfig.streamFrame.deriveMagSource == 1
									? fs.angMagEma
									: sqrt(fs.ang[0] * fs.ang[0] + fs.ang[1] * fs.ang[1] + fs.ang[2] * fs.ang[2]);
								double dnA = sqrt(basisAng[0] * basisAng[0] + basisAng[1] * basisAng[1] + basisAng[2] * basisAng[2]);
								if(dnA > 1e-9 && magA > 1e-9){
									for(int a = 0; a < 3; a++){
										outAng[a] = basisAng[a] / dnA * magA;
									}
								}
							}
							if(!fs.splitLogged || fs.lastSource != dirSource){
								fs.splitLogged = true;
								fs.lastSource = dirSource;
								logSplit = true;
							}
						}else{
							// re-log if it gets re-enabled after being off
							fs.splitLogged = false;
						}
						// release latch: keep the rolling window peak of the
						// OUTPUT, and if an input release armed the latch,
						// replay the peak (full strength for the first half
						// of the hold, linear decay after)
						{
							// per-channel peaks: each channel keyed on ITS
							// OWN speed, so arm-throw windup (angular spike,
							// backward v) can never poison the linear replay
							// peaks keyed on the SECANT magnitudes, not on
							// |out|: the scalar-EMA magnitude crests AFTER
							// physical release (lag), by which time the
							// direction has reversed — |out|-keyed peaks
							// captured the snap-back (field 2026-08-10:
							// release dir 121 deg median off the true peak,
							// magnitude 1.4-3x). the secant COLLAPSES at
							// reversal (forward and back cancel), so a
							// secant-keyed peak structurally cannot land in
							// the snap-back.
							double secSp = sqrt(secantVel[0] * secantVel[0] + secantVel[1] * secantVel[1] + secantVel[2] * secantVel[2]);
							double secAngSp = sqrt(secantAng[0] * secantAng[0] + secantAng[1] * secantAng[1] + secantAng[2] * secantAng[2]);
							double latchWindow = driverConfig.streamFrame.deriveLatchWindowMs / 1000.0;
							if(latchWindow < 0.02){ latchWindow = 0.02; }
							if(secSp >= fs.linPeakMag || now - fs.linPeakTime > latchWindow){
								fs.linPeakMag = secSp;
								fs.linPeakTime = now;
								for(int a = 0; a < 3; a++){
									fs.linPeakVel[a] = outVel[a];
								}
							}
							if(secAngSp >= fs.angPeakMag || now - fs.angPeakTime > latchWindow){
								fs.angPeakMag = secAngSp;
								fs.angPeakTime = now;
								for(int a = 0; a < 3; a++){
									fs.angPeakVel[a] = outAng[a];
								}
							}
							if(driverConfig.streamFrame.deriveReleaseLatch && now < fs.latchUntil){
								double holdS = driverConfig.streamFrame.deriveLatchHoldMs / 1000.0;
								if(holdS < 0.02){ holdS = 0.02; }
								double frac = (fs.latchUntil - now) / holdS; // 1 -> 0
								double w = frac * 2.0;
								if(w > 1.0){ w = 1.0; }
								if(w < 0.0){ w = 0.0; }
								// each channel replays only if ITS peak passes
								// ITS gate — casual regrabs no longer twitch
								if(fs.linPeakMag > driverConfig.streamFrame.deriveLatchMinSpeed){
									for(int a = 0; a < 3; a++){
										outVel[a] = fs.linPeakVel[a] * w + outVel[a] * (1.0 - w);
									}
								}
								if(fs.angPeakMag > driverConfig.streamFrame.deriveLatchAngMinSpeed){
									for(int a = 0; a < 3; a++){
										outAng[a] = fs.angPeakVel[a] * w + outAng[a] * (1.0 - w);
									}
								}
								// pose-assist: continue the throw arc in the
								// REPORTED position for engines that derive
								// throws from pose deltas rather than
								// vecVelocity. offset = latched v * elapsed
								// hold time, faded with the same decay.
								if(driverConfig.streamFrame.deriveLatchPoseAssist
										&& fs.linPeakMag > driverConfig.streamFrame.deriveLatchMinSpeed){
									double holdSFull = driverConfig.streamFrame.deriveLatchHoldMs / 1000.0;
									if(holdSFull < 0.02){ holdSFull = 0.02; }
									double elapsed = holdSFull - (fs.latchUntil - now);
									if(elapsed < 0){ elapsed = 0; }
									for(int a = 0; a < 3; a++){
										pose.vecPosition[a] += fs.linPeakVel[a] * elapsed * w;
									}
								}
							}
							// consumer discriminator: zero the reported
							// velocity so a 2-minute field test proves
							// whether this game reads vecVelocity at all
							if(driverConfig.streamFrame.deriveDiagVelocity == 1){
								for(int a = 0; a < 3; a++){
									outVel[a] = 0;
									outAng[a] = 0;
								}
							}
						}
						pose.vecVelocity[0] = outVel[0];
						pose.vecVelocity[1] = outVel[1];
						pose.vecVelocity[2] = outVel[2];
						pose.vecAngularVelocity[0] = outAng[0];
						pose.vecAngularVelocity[1] = outAng[1];
						pose.vecAngularVelocity[2] = outAng[2];
						// direction-source diagnostic: while poseLogging is
						// on and the hand moves at throw speed, capture the
						// output plus every candidate direction vector so a
						// field log can rank the sources against reality
						// (100Hz throttle per device; values copied out and
						// the line written outside the lock)
						if(driverConfig.streamFrame.poseLogging){
							double outSp = sqrt(outVel[0] * outVel[0] + outVel[1] * outVel[1] + outVel[2] * outVel[2]);
							double outAngSp = sqrt(outAng[0] * outAng[0] + outAng[1] * outAng[1] + outAng[2] * outAng[2]);
							// effective speed so wrist flicks are captured
							if(outSp + 0.15 * outAngSp > 2.0 && now - fs.lastDirLogTime >= 0.01){
								fs.lastDirLogTime = now;
								logDirDiag = true;
								for(int a = 0; a < 3; a++){
									diagOut[a] = outVel[a];
									diagRaw[a] = derivedVel[a];
									diagAngOut[a] = outAng[a];
								}
							}
						}
					}
					if(logSplit){
						// outside the lock — lock discipline
						DriverLog("VelocityFix: split-dir active id=%u linear=%d angular=%d source=%s window=%.0fms pow=%.1f",
							openVRID, splitLin ? 1 : 0, splitAng ? 1 : 0,
							dirSource == 1 ? "secant" : (dirSource == 2 ? "runtime" : "window"),
							dirWindow * 1000.0, dirPow);
					}
					if(logDirDiag){
						// secantVel/runtimeVel are locals of this frame —
						// no lock needed; raw and out copied under the lock
						DriverLog("PoseLog: BURSTDIR id=%u out=(%.3f, %.3f, %.3f) raw=(%.3f, %.3f, %.3f) sec=(%.3f, %.3f, %.3f) run=(%.3f, %.3f, %.3f)",
							openVRID,
							diagOut[0], diagOut[1], diagOut[2],
							diagRaw[0], diagRaw[1], diagRaw[2],
							secantVel[0], secantVel[1], secantVel[2],
							runtimeVel[0], runtimeVel[1], runtimeVel[2]);
						DriverLog("PoseLog: BURSTANG id=%u out=(%.3f, %.3f, %.3f) raw=(%.3f, %.3f, %.3f) sec=(%.3f, %.3f, %.3f) run=(%.3f, %.3f, %.3f)",
							openVRID,
							diagAngOut[0], diagAngOut[1], diagAngOut[2],
							derivedAng[0], derivedAng[1], derivedAng[2],
							secantAng[0], secantAng[1], secantAng[2],
							runtimeAng[0], runtimeAng[1], runtimeAng[2]);
					}
				}
			}else if(velocityFixMode < 5 && derivedSpeed < 20.0 && derivedAngSpeed < 60.0 && sEff > 1.0){
				// NOTE: mode 4 currently reaches this blend and the
				// peak-hold below (the gates predate it) — preserved
				// as-is because it IS the shipped field behavior. the CA
				// modes (5/6) are gated out by design: the estimator
				// reports the state whole, no heuristics stacked on top.
				double w = (sEff - 1.0) / 1.5;
				if(w > 1.0){ w = 1.0; }
				w = w * w * (3.0 - 2.0 * w); // smoothstep
				pose.vecVelocity[0] = pose.vecVelocity[0] * (1.0 - w) + derivedVel[0] * w;
				pose.vecVelocity[1] = pose.vecVelocity[1] * (1.0 - w) + derivedVel[1] * w;
				pose.vecVelocity[2] = pose.vecVelocity[2] * (1.0 - w) + derivedVel[2] * w;
				if(!classicMode){
					pose.vecAngularVelocity[0] = pose.vecAngularVelocity[0] * (1.0 - w) + derivedAng[0] * w;
					pose.vecAngularVelocity[1] = pose.vecAngularVelocity[1] * (1.0 - w) + derivedAng[1] * w;
					pose.vecAngularVelocity[2] = pose.vecAngularVelocity[2] * (1.0 - w) + derivedAng[2] * w;
				}
			}
			// record the motion snapshot for the release tap regardless of
			// what the peak hold decides below
			// (filled in after the hold logic so it reflects the final output)
			// full mode only from here: classic (v3) is estimator + blend
			// and nothing else; derive is pure replacement (no peak hold —
			// the estimate IS the signal, latching would re-introduce a
			// hybrid)
			if(!classicMode && !deriveMode && velocityFixMode < 5)
			// joint peak hold: right after release the hand snaps back and
			// a low lag estimate faithfully reports that reversal, so games
			// sampling a frame or two late read a backward/down vector.
			// hold the (v, w) pair from the most recent linear speed peak,
			// decaying over 90ms, so late sampling still reads the throw.
			{
				const double holdSeconds = 0.07;
				std::lock_guard<std::mutex> guard(poseLogLock);
				VelFixState &state = velFixStates[openVRID];
				double outSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
					+ pose.vecVelocity[1] * pose.vecVelocity[1]
					+ pose.vecVelocity[2] * pose.vecVelocity[2]);
				double elapsed = now - state.peakTime;
				double decay = 1.0 - elapsed / holdSeconds;
				if(decay < 0){ decay = 0; }
				// plausibility: a step to more than 1.8x + 1 of the previous
				// output is a suspected spike; let it pass through this
				// frame but never latch it as a peak
				bool plausible = outSpeed <= state.lastOutSpeed * 1.8 + 1.0;
				// hold only engages on VIOLENT deceleration (>60 m/s^2 drop
				// from the previous output). post release snap back is
				// 100+ m/s^2, deliberate stops are 10-30, so normal play no
				// longer floats on held velocity (session 8 feedback).
				double sampleDt = now - state.lastOutTime;
				bool violentDecel = state.lastOutTime > 0 && sampleDt > 0.001 && sampleDt < 0.1
					&& (state.lastOutSpeed - outSpeed) / sampleDt > 60.0;
				// direction aware latch: a fresh peak may only be replaced
				// by motion in the same hemisphere. session 11 forensics: a
				// gentle lob's hand RETRACTION (faster than the lob itself)
				// latched as "the peak" and the direction hold then
				// enforced the downward retraction. opposite-direction
				// motion must wait out the freshness window (a new gesture)
				// before it can own the peak.
				bool sameHemisphere = true;
				if(now - state.peakTime < 0.25 && state.peakSpeed > 0.3 && outSpeed > 0.3){
					double dot = pose.vecVelocity[0] * state.peakVel[0]
						+ pose.vecVelocity[1] * state.peakVel[1]
						+ pose.vecVelocity[2] * state.peakVel[2];
					sameHemisphere = dot > 0;
				}
				if(plausible && sameHemisphere && outSpeed >= state.peakSpeed * decay){
					state.holdActive = false;
					state.peakSpeed = outSpeed;
					state.peakTime = now;
					state.peakVel[0] = pose.vecVelocity[0];
					state.peakVel[1] = pose.vecVelocity[1];
					state.peakVel[2] = pose.vecVelocity[2];
					state.peakAng[0] = pose.vecAngularVelocity[0];
					state.peakAng[1] = pose.vecAngularVelocity[1];
					state.peakAng[2] = pose.vecAngularVelocity[2];
				}else if(state.peakSpeed > 2.0 && decay > 0 && state.peakSpeed * decay > outSpeed
						&& (violentDecel || state.holdActive)){
					state.holdActive = true;
					pose.vecVelocity[0] = state.peakVel[0] * decay;
					pose.vecVelocity[1] = state.peakVel[1] * decay;
					pose.vecVelocity[2] = state.peakVel[2] * decay;
					pose.vecAngularVelocity[0] = state.peakAng[0] * decay;
					pose.vecAngularVelocity[1] = state.peakAng[1] * decay;
					pose.vecAngularVelocity[2] = state.peakAng[2] * decay;
				}
				// direction hold: within 120ms of a real peak, keep the
				// OUTPUT DIRECTION aligned with the peak's direction while
				// magnitude stays honest. session 10 forensics: throw
				// releases scatter up to ~60ms after the velocity peak, and
				// by then the upward component has flipped sign — the item
				// dives ("textbook failure") even though speed is fine.
				// direction hold fixes that without the magnitude float
				// that the violent-decel gate was added to prevent, because
				// prediction still moves hands at the true (decaying) speed.
				double elapsedDir = now - state.peakTime;
				if(state.peakSpeed > 1.5 && elapsedDir < 0.12
						&& outSpeed > 0.25 * state.peakSpeed && outSpeed > 0.3){
					double hw = 1.0 - elapsedDir / 0.12;
					double curSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
						+ pose.vecVelocity[1] * pose.vecVelocity[1]
						+ pose.vecVelocity[2] * pose.vecVelocity[2]);
					double peakMag = state.peakSpeed;
					if(curSpeed > 0.001 && peakMag > 0.001){
						// nlerp between near-opposite unit vectors passes
						// through ~zero and normalizes into noise (= the
						// "random direction" throws). opposite-direction
						// current output within the window IS the snap back
						// we are protecting against: use the peak direction
						// outright.
						double dirDot = (pose.vecVelocity[0] * state.peakVel[0]
							+ pose.vecVelocity[1] * state.peakVel[1]
							+ pose.vecVelocity[2] * state.peakVel[2]) / (curSpeed * peakMag);
						double useHw = dirDot < 0.1 ? 1.0 : hw;
						double dir[3];
						double blended[3] = {
							pose.vecVelocity[0] / curSpeed * (1.0 - useHw) + state.peakVel[0] / peakMag * useHw,
							pose.vecVelocity[1] / curSpeed * (1.0 - useHw) + state.peakVel[1] / peakMag * useHw,
							pose.vecVelocity[2] / curSpeed * (1.0 - useHw) + state.peakVel[2] / peakMag * useHw,
						};
						double bn = sqrt(blended[0] * blended[0] + blended[1] * blended[1] + blended[2] * blended[2]);
						if(bn > 0.001){
							dir[0] = blended[0] / bn; dir[1] = blended[1] / bn; dir[2] = blended[2] / bn;
							pose.vecVelocity[0] = dir[0] * curSpeed;
							pose.vecVelocity[1] = dir[1] * curSpeed;
							pose.vecVelocity[2] = dir[2] * curSpeed;
						}
					}
					// same treatment for angular velocity so w x r keeps
					// steering with the throw
					double curAng = sqrt(pose.vecAngularVelocity[0] * pose.vecAngularVelocity[0]
						+ pose.vecAngularVelocity[1] * pose.vecAngularVelocity[1]
						+ pose.vecAngularVelocity[2] * pose.vecAngularVelocity[2]);
					double peakAngMag = sqrt(state.peakAng[0] * state.peakAng[0]
						+ state.peakAng[1] * state.peakAng[1] + state.peakAng[2] * state.peakAng[2]);
					if(curAng > 0.05 && peakAngMag > 0.05){
						double angDot = (pose.vecAngularVelocity[0] * state.peakAng[0]
							+ pose.vecAngularVelocity[1] * state.peakAng[1]
							+ pose.vecAngularVelocity[2] * state.peakAng[2]) / (curAng * peakAngMag);
						double useHwAng = angDot < 0.1 ? 1.0 : hw;
						double blended[3] = {
							pose.vecAngularVelocity[0] / curAng * (1.0 - useHwAng) + state.peakAng[0] / peakAngMag * useHwAng,
							pose.vecAngularVelocity[1] / curAng * (1.0 - useHwAng) + state.peakAng[1] / peakAngMag * useHwAng,
							pose.vecAngularVelocity[2] / curAng * (1.0 - useHwAng) + state.peakAng[2] / peakAngMag * useHwAng,
						};
						double bn = sqrt(blended[0] * blended[0] + blended[1] * blended[1] + blended[2] * blended[2]);
						if(bn > 0.001){
							pose.vecAngularVelocity[0] = blended[0] / bn * curAng;
							pose.vecAngularVelocity[1] = blended[1] / bn * curAng;
							pose.vecAngularVelocity[2] = blended[2] / bn * curAng;
						}
					}
				}
				// release gesture anchor takes precedence over the
				// heuristic holds above: from the moment the finger starts
				// opening (scalar falling from plateau) until 150ms later,
				// the output RATCHETS — rising motion updates it, falling
				// motion cannot degrade it. games sample their release
				// anywhere in this window; all of them read the peak.
				if(now - state.anchorTime < 0.15 && state.anchorTime > 0){
					double curOutSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
						+ pose.vecVelocity[1] * pose.vecVelocity[1]
						+ pose.vecVelocity[2] * pose.vecVelocity[2]);
					if(!state.anchorHasValue || curOutSpeed > state.anchorSpeed){
						state.anchorHasValue = true;
						state.anchorSpeed = curOutSpeed;
						state.anchorVel[0] = pose.vecVelocity[0];
						state.anchorVel[1] = pose.vecVelocity[1];
						state.anchorVel[2] = pose.vecVelocity[2];
						state.anchorAng[0] = pose.vecAngularVelocity[0];
						state.anchorAng[1] = pose.vecAngularVelocity[1];
						state.anchorAng[2] = pose.vecAngularVelocity[2];
					}else{
						pose.vecVelocity[0] = state.anchorVel[0];
						pose.vecVelocity[1] = state.anchorVel[1];
						pose.vecVelocity[2] = state.anchorVel[2];
						pose.vecAngularVelocity[0] = state.anchorAng[0];
						pose.vecAngularVelocity[1] = state.anchorAng[1];
						pose.vecAngularVelocity[2] = state.anchorAng[2];
					}
				}
				state.lastOutSpeed = outSpeed;
				state.lastOutTime = now;
				MotionSnapshot &snap = motionSnapshots[openVRID];
				snap.time = now;
				snap.outVel[0] = pose.vecVelocity[0];
				snap.outVel[1] = pose.vecVelocity[1];
				snap.outVel[2] = pose.vecVelocity[2];
				snap.outAng[0] = pose.vecAngularVelocity[0];
				snap.outAng[1] = pose.vecAngularVelocity[1];
				snap.outAng[2] = pose.vecAngularVelocity[2];
				snap.outSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
					+ pose.vecVelocity[1] * pose.vecVelocity[1]
					+ pose.vecVelocity[2] * pose.vecVelocity[2]);
				snap.trackingOk = true;
				snap.result = (int)pose.result;
			}
			// PEAKDIAG (kalman modes, poseLogging): the better/worse
			// instrument for the CA experiment. per gesture it tracks
			// the peak of the reported OUTPUT, the calm state, the
			// magnitude channel, and the raw ring SECANT (displacement
			// over a window — the closest thing to ground truth), then
			// logs one line at gesture end. out/sec -> 1.0 means the
			// throw carries true displacement speed (magnitude honest);
			// dirOff is the angle between the reported direction at its
			// peak and the secant direction at its peak (direction
			// quality). compare lines across modes 4/5/6 on the same
			// gesture set: better = out/sec closer to 1.0 WITHOUT
			// dirOff growing.
			if(velocityFixMode >= 4 && driverConfig.streamFrame.poseLogging){
				double outSp = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
					+ pose.vecVelocity[1] * pose.vecVelocity[1]
					+ pose.vecVelocity[2] * pose.vecVelocity[2]);
				double outAngSp = sqrt(pose.vecAngularVelocity[0] * pose.vecAngularVelocity[0]
					+ pose.vecAngularVelocity[1] * pose.vecAngularVelocity[1]
					+ pose.vecAngularVelocity[2] * pose.vecAngularVelocity[2]);
				double secSp = sqrt(secantVel[0] * secantVel[0]
					+ secantVel[1] * secantVel[1] + secantVel[2] * secantVel[2]);
				double secAngSp = sqrt(secantAng[0] * secantAng[0]
					+ secantAng[1] * secantAng[1] + secantAng[2] * secantAng[2]);
				bool logPk = false;
				double pkOut = 0, pkSec = 0, pkCalm = 0, pkMag = 0, pkDirOff = 0, pkAngOut = 0, pkAngSec = 0;
				double pkGrip = 0, pkWr = 0, pkDirOffG = 0;
				double pkLagLin = 0, pkLagAng = 0, pkDtW = 0;
				// raw-referenced fields (see KalState::rawRingN): out vs
				// the RAW peak, direction vs the raw peak vector, and the
				// output peak's lag behind the raw peak = the filter's
				// actual velocity group delay (the number the sim table
				// predicts from J/P). -1/NA when no raw peak is fresh.
				double pkRawSec = -1, pkRawDirOff = -1, pkRawLagLin = -9999, pkRawLagAng = -9999, pkRawAngSec = -1;
				{
					std::lock_guard<std::mutex> pkGuard(deriveFilterLock);
					KalState &pks = kalStates[openVRID];
					// release-instant snapshot: shaped output + secant,
					// every frame, read by RELDIAG at the release edge
					pks.relSnapHave = true;
					for(int a2 = 0; a2 < 3; a2++){
						pks.relOutV[a2] = pose.vecVelocity[a2];
						pks.relOutW[a2] = pose.vecAngularVelocity[a2];
						pks.relSecV[a2] = secantVel[a2];
						pks.relSecW[a2] = secantAng[a2];
					}
					if(!pks.pkActive && outSp > 2.0){
						pks.pkActive = true;
						pks.pkOut = 0; pks.pkSec = 0; pks.pkCalm = 0; pks.pkMag = 0;
						pks.pkAngOut = 0; pks.pkAngSec = 0;
						pks.pkGrip = 0; pks.pkWr = 0;
						pks.pkOutT = now; pks.pkSecT = now; pks.pkAngOutT = now; pks.pkAngSecT = now;
						for(int a2 = 0; a2 < 3; a2++){ pks.pkOutVec[a2] = 0; pks.pkSecVec[a2] = 0; pks.pkGripVec[a2] = 0; }
					}
					if(pks.pkActive){
						if(outSp > pks.pkOut){
							pks.pkOut = outSp;
							pks.pkOutT = now;
							for(int a2 = 0; a2 < 3; a2++){ pks.pkOutVec[a2] = pose.vecVelocity[a2]; }
						}
						if(secSp > pks.pkSec){
							pks.pkSec = secSp;
							pks.pkSecT = now;
							for(int a2 = 0; a2 < 3; a2++){ pks.pkSecVec[a2] = secantVel[a2]; }
						}
						if(pks.diagCalmSp > pks.pkCalm){ pks.pkCalm = pks.diagCalmSp; }
						if(pks.diagMagSp > pks.pkMag){ pks.pkMag = pks.diagMagSp; }
						if(outAngSp > pks.pkAngOut){ pks.pkAngOut = outAngSp; pks.pkAngOutT = now; }
						if(secAngSp > pks.pkAngSec){ pks.pkAngSec = secAngSp; pks.pkAngSecT = now; }
						// grip shadow channel: peak of the transported
						// velocity (and of the removed w x r itself).
						// with the compensator DISABLED this is the
						// what-if channel; with it ENABLED out and grip
						// coincide and dirOffG==dirOff.
						if(pks.diagGripHave){
							double gSp = sqrt(pks.diagGripV[0] * pks.diagGripV[0]
								+ pks.diagGripV[1] * pks.diagGripV[1]
								+ pks.diagGripV[2] * pks.diagGripV[2]);
							if(gSp > pks.pkGrip){
								pks.pkGrip = gSp;
								for(int a2 = 0; a2 < 3; a2++){ pks.pkGripVec[a2] = pks.diagGripV[a2]; }
							}
							if(pks.diagGripWr > pks.pkWr){ pks.pkWr = pks.diagGripWr; }
						}
						if(outSp < 0.8){
							pks.pkActive = false;
							logPk = pks.pkSec > 0.5;
							pkOut = pks.pkOut; pkSec = pks.pkSec;
							pkCalm = pks.pkCalm; pkMag = pks.pkMag;
							pkAngOut = pks.pkAngOut; pkAngSec = pks.pkAngSec;
							pkGrip = pks.pkGrip; pkWr = pks.pkWr;
							// channel lags vs own secant (+ = output peaked
							// after the secant = filter lag); dtW = angular
							// output peak vs linear output peak (+ = wrist
							// peaked after arm: kinematic sequencing + any
							// remaining lag mismatch)
							pkLagLin = (pks.pkOutT - pks.pkSecT) * 1000.0;
							pkLagAng = (pks.pkAngOutT - pks.pkAngSecT) * 1000.0;
							pkDtW = (pks.pkAngOutT - pks.pkOutT) * 1000.0;
							double d = 0, no = 0, ns = 0, dg = 0, ng = 0;
							for(int a2 = 0; a2 < 3; a2++){
								d += pks.pkOutVec[a2] * pks.pkSecVec[a2];
								no += pks.pkOutVec[a2] * pks.pkOutVec[a2];
								ns += pks.pkSecVec[a2] * pks.pkSecVec[a2];
								dg += pks.pkGripVec[a2] * pks.pkSecVec[a2];
								ng += pks.pkGripVec[a2] * pks.pkGripVec[a2];
							}
							if(no > 1e-9 && ns > 1e-9){
								double c = d / sqrt(no * ns);
								if(c > 1.0){ c = 1.0; }
								if(c < -1.0){ c = -1.0; }
								pkDirOff = acos(c) * 180.0 / 3.14159265358979323846;
							}
							// raw reference: latest raw peak within 600ms of
							// this gesture's output peak
							if(pks.rawPkSp > 0.5 && fabs(pks.pkOutT - pks.rawPkT) < 0.6){
								pkRawSec = pks.rawPkSp;
								pkRawLagLin = (pks.pkOutT - pks.rawPkT) * 1000.0;
								double dr = 0;
								for(int a2 = 0; a2 < 3; a2++){ dr += pks.pkOutVec[a2] * pks.rawPkV[a2]; }
								if(no > 1e-9){
									double cr = dr / (sqrt(no) * pks.rawPkSp);
									if(cr > 1.0){ cr = 1.0; }
									if(cr < -1.0){ cr = -1.0; }
									pkRawDirOff = acos(cr) * 180.0 / 3.14159265358979323846;
								}
							}
							if(pks.rawPkWSp > 2.0 && fabs(pks.pkAngOutT - pks.rawPkWT) < 0.6){
								pkRawAngSec = pks.rawPkWSp;
								pkRawLagAng = (pks.pkAngOutT - pks.rawPkWT) * 1000.0;
							}
							// grip peak direction vs the SAME origin secant:
							// legitimate reference because the w x r spike
							// moves the origin only centimeters over the
							// gesture — displacement stays palm-dominated
							// even when instantaneous velocity does not
							if(ng > 1e-9 && ns > 1e-9){
								double cg = dg / sqrt(ng * ns);
								if(cg > 1.0){ cg = 1.0; }
								if(cg < -1.0){ cg = -1.0; }
								pkDirOffG = acos(cg) * 180.0 / 3.14159265358979323846;
							}
						}
					}
				}
				// log OUTSIDE the lock
				if(logPk){
					// grip fields: gOut = peak transported speed (flick
					// unit test: collapses toward 0 on a pure wrist snap
					// when r is right), gDirOff = its direction vs the
					// same secant, wr = peak removed |w x r| (the
					// contamination magnitude). all zero when r unset.
					DriverLog("PoseLog: PEAKDIAG id=%u mode=%d out=%.2f sec=%.2f out/sec=%.2f calm=%.2f mag=%.2f dirOff=%.1fdeg angOut=%.1f angSec=%.1f gOut=%.2f gDirOff=%.1fdeg wr=%.2f lagLin=%.0fms lagAng=%.0fms dtW=%.0fms rawSec=%.2f out/rawSec=%.2f rawDirOff=%.1fdeg rawLagLin=%s%.0fms rawAngSec=%.1f angOut/rawAngSec=%.2f rawLagAng=%s%.0fms",
						openVRID, velocityFixMode, pkOut, pkSec,
						pkSec > 0.01 ? pkOut / pkSec : 0.0,
						pkCalm, pkMag, pkDirOff, pkAngOut, pkAngSec,
						pkGrip, pkDirOffG, pkWr, pkLagLin, pkLagAng, pkDtW,
						pkRawSec, pkRawSec > 0.01 ? pkOut / pkRawSec : -1.0, pkRawDirOff,
						pkRawLagLin < -9000 ? "NA/" : "", pkRawLagLin < -9000 ? 0.0 : pkRawLagLin,
						pkRawAngSec, pkRawAngSec > 0.01 ? pkAngOut / pkRawAngSec : -1.0,
						pkRawLagAng < -9000 ? "NA/" : "", pkRawLagAng < -9000 ? 0.0 : pkRawLagAng);
				}
			}
		}
	}else if(velocityFixMode == 2 && openVRID != vr::k_unTrackedDeviceIndex_Hmd
			&& IsStreamedController(openVRID)){
		// tracking dropped mid motion (camera based tracking loses the
		// controller exactly at throw windup). bridge VELOCITY only: if a
		// peak is fresh, keep replaying its decay so a release read during
		// a short dropout still carries the throw instead of zero.
		// positions are never synthesized.
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = velFixStates.find(openVRID);
		if(found != velFixStates.end()){
			VelFixState &state = found->second;
			double elapsed = now - state.peakTime;
			double decay = 1.0 - elapsed / 0.07;
			if(state.peakSpeed > 2.0 && decay > 0){
				pose.vecVelocity[0] = state.peakVel[0] * decay;
				pose.vecVelocity[1] = state.peakVel[1] * decay;
				pose.vecVelocity[2] = state.peakVel[2] * decay;
				pose.vecAngularVelocity[0] = state.peakAng[0] * decay;
				pose.vecAngularVelocity[1] = state.peakAng[1] * decay;
				pose.vecAngularVelocity[2] = state.peakAng[2] * decay;
			}
			MotionSnapshot &snap = motionSnapshots[openVRID];
			snap.time = now;
			snap.outVel[0] = pose.vecVelocity[0];
			snap.outVel[1] = pose.vecVelocity[1];
			snap.outVel[2] = pose.vecVelocity[2];
			snap.outSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
				+ pose.vecVelocity[1] * pose.vecVelocity[1]
				+ pose.vecVelocity[2] * pose.vecVelocity[2]);
			snap.trackingOk = false;
			snap.result = (int)pose.result;
		}
	}
	// mixed-space velocity frame fix (playspace-override setups): the
	// openvr header leaves vecVelocity's frame unspecified while positions
	// are driver-space + WorldFromDriver. an overrider aligning lighthouse
	// space into the vrlink space carries a large WorldFromDriver yaw, and
	// with mismatched conventions thrown objects fly at the right speed in
	// the wrong direction. "world" (1) rotates the reported velocity by
	// qWorldFromDriverRotation, "driver" (2) applies the inverse; the
	// field test decides which matches vrserver's real convention. only
	// devices whose WorldFromDriver rotation deviates >2 deg from identity
	// are touched (and logged once either way, so a log alone shows the
	// alignment angle and whether this fix is even relevant).
	if(openVRID != vr::k_unTrackedDeviceIndex_Hmd && openVRID < 64 && pose.poseIsValid){
		const vr::HmdQuaternion_t &qwd = pose.qWorldFromDriverRotation;
		double wClamped = qwd.w > 1.0 ? 1.0 : (qwd.w < -1.0 ? -1.0 : qwd.w);
		double angleDeg = 2.0 * acos(fabs(wClamped)) * 180.0 / 3.14159265358979323846;
		if(angleDeg > 2.0){
			int spaceFixMode = driverConfig.controllers.spaceVelocityFixMode;
			uint64_t bit = 1ull << openVRID;
			if(!(spaceFixLoggedMask.load(std::memory_order_relaxed) & bit)){
				spaceFixLoggedMask.fetch_or(bit, std::memory_order_relaxed);
				DriverLog("SpaceVelFix: id=%u WorldFromDriver angle=%.1f deg, mode=%s",
					openVRID, angleDeg,
					spaceFixMode == 1 ? "world" : (spaceFixMode == 2 ? "driver" : "off (candidate)"));
			}
			if(spaceFixMode > 0){
				vr::HmdQuaternion_t q = qwd;
				if(spaceFixMode == 2){
					q.x = -q.x; q.y = -q.y; q.z = -q.z;
				}
				double vIn[3] = { pose.vecVelocity[0], pose.vecVelocity[1], pose.vecVelocity[2] };
				double wIn[3] = { pose.vecAngularVelocity[0], pose.vecAngularVelocity[1], pose.vecAngularVelocity[2] };
				double vOut[3], wOut[3];
				QuatRotateVector(q, vIn, vOut);
				QuatRotateVector(q, wIn, wOut);
				pose.vecVelocity[0] = vOut[0]; pose.vecVelocity[1] = vOut[1]; pose.vecVelocity[2] = vOut[2];
				pose.vecAngularVelocity[0] = wOut[0]; pose.vecAngularVelocity[1] = wOut[1]; pose.vecAngularVelocity[2] = wOut[2];
			}
		}
	}

	if(driverConfig.streamFrame.poseLogging && openVRID != vr::k_unTrackedDeviceIndex_Hmd){
		LogDevicePose(openVRID, pose);
	}
	return true;
}

bool CustomHeadsetDeviceProvider::IsStreamedController(uint32_t openVRID){
	{
		std::lock_guard<std::mutex> guard(streamedIdentityLock);
		auto found = streamedControllerCache.find(openVRID);
		if(found != streamedControllerCache.end()){
			return found->second != 0;
		}
	}
	// property query with NO lock held (concurrency law: never call out
	// while holding a lock — ResolveContainerId taught us that one)
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(openVRID);
	vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
	char serial[128] = {};
	vr::VRProperties()->GetStringProperty(container, vr::Prop_SerialNumber_String, serial, sizeof(serial), &propError);
	bool streamed = false;
	if(propError == vr::TrackedProp_Success){
		streamed = strncmp(serial, "VRLINK", 6) == 0 || strncmp(serial, "SamsungVST", 10) == 0;
	}else{
		// property not readable yet: do not cache, do not touch
		return false;
	}
	{
		std::lock_guard<std::mutex> guard(streamedIdentityLock);
		streamedControllerCache[openVRID] = streamed ? 1 : 0;
	}
	DriverLog("VelocityFix: id=%u serial=%s streamed=%d%s", openVRID, serial, streamed ? 1 : 0,
		streamed ? "" : " (native velocity, never touched)");
	return streamed;
}

// direction secant over the full derive ring: raw displacement newest-oldest
// over the ring span. at throw speeds the displacement (5-20cm) dwarfs the
// ~1-4mm per-sample position noise, so this direction is clean to a few
// degrees where the endpoint-fit direction is noise dominated (the fit's
// consecutive estimates share 7/8 of their inputs — their noise is common
// mode and does not average away). pure math; called under poseLogLock.
void CustomHeadsetDeviceProvider::ComputeRingSecant(const VelFixState &state, bool useSmoothed, double secantVel[3], double secantAng[3]){
	int newest = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
	int oldest = state.head; // ring is full at every call site
	double span = state.time[newest] - state.time[oldest];
	if(span <= 1e-6){
		for(int a = 0; a < 3; a++){ secantVel[a] = 0; secantAng[a] = 0; }
		return;
	}
	const double (*P)[3] = useSmoothed ? state.smPos : state.pos;
	for(int a = 0; a < 3; a++){
		secantVel[a] = (P[newest][a] - P[oldest][a]) / span;
	}
	// angular secant: world-frame relative rotation oldest -> newest as a
	// rotation vector over the span (same small angle mapping as the fit)
	const vr::HmdQuaternion_t* Q = useSmoothed ? state.smQuat : state.quat;
	vr::HmdQuaternion_t qOldConj = {Q[oldest].w, -Q[oldest].x, -Q[oldest].y, -Q[oldest].z};
	vr::HmdQuaternion_t dq = QuatMultiply(Q[newest], qOldConj);
	double sign = dq.w < 0 ? -1.0 : 1.0;
	double vn = sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
	double angle = 2.0 * atan2(vn, fabs(dq.w));
	double scale = vn > 1e-9 ? sign * angle / (vn * span) : 0.0;
	secantAng[0] = dq.x * scale;
	secantAng[1] = dq.y * scale;
	secantAng[2] = dq.z * scale;
}

bool CustomHeadsetDeviceProvider::DeriveMotion(uint32_t openVRID, const vr::DriverPose_t &pose, double derivedVel[3], double derivedAng[3], double secantVel[3], double secantAng[3]){
	double now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
	std::lock_guard<std::mutex> guard(poseLogLock);
	VelFixState &state = velFixStates[openVRID];
	
	// teleport / recenter rejection: a step implying > 30 m/s from the
	// previous sample invalidates the window
	if(state.count > 0){
		int prev = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
		double dt = now - state.time[prev];
		if(dt <= 0 || dt > 0.1){
			state.count = 0;
			state.haveEma = false;
			state.haveEmaAng = false;
		}else{
			double dx = pose.vecPosition[0] - state.pos[prev][0];
			double dy = pose.vecPosition[1] - state.pos[prev][1];
			double dz = pose.vecPosition[2] - state.pos[prev][2];
			double stepDist = sqrt(dx * dx + dy * dy + dz * dz);
			// 2026-08-17: dt here is the CALLBACK spacing (~2.8ms) while
			// distinct positions arrive every ~14ms, so a fresh step of
			// a 6+ m/s hand (8-10cm) implies "30+ m/s" and reset the
			// ring on every fresh sample of a fast throw. in the kalman
			// modes this ring only feeds instruments (secant, PEAKDIAG
			// snapshot) — the reset froze the release snapshot for the
			// whole fast phase (field: 122/137 releases scored a stale
			// windup vector). a teleport is a big jump, not a fast hand:
			// require the step itself to be large as well. gated to the
			// kalman modes so the legacy modes' field behavior is
			// untouched.
			bool teleStep = stepDist / dt > 30.0;
			if(driverConfig.streamFrame.velocityFixMode >= 5 && stepDist < 0.3){ teleStep = false; }
			if(teleStep){
				state.count = 0;
				state.haveEma = false;
				state.haveEmaAng = false;
			}
		}
	}
	
	// skip duplicated / oversampled updates so the ring spans real time
	if(state.count > 0){
		int prev = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
		if(now - state.time[prev] < 0.003){
			// still allow output from the existing window
			if(state.count < VelFixState::ringSize || !state.haveEma || !state.haveEmaAng){
				return false;
			}
			derivedVel[0] = state.emaVel[0];
			derivedVel[1] = state.emaVel[1];
			derivedVel[2] = state.emaVel[2];
			derivedAng[0] = state.emaAng[0];
			derivedAng[1] = state.emaAng[1];
			derivedAng[2] = state.emaAng[2];
			ComputeRingSecant(state, state.haveSm, secantVel, secantAng);
			return true;
		}
	}
	// input prefilter (opt-in): per-axis median of the last 3 raw
	// positions. one-sample lag; single-sample spikes (network jitter
	// tails) can no longer reach the fit or the secant. state is under
	// poseLogLock like the rest of VelFixState; pure math only.
	double pushPos[3] = { pose.vecPosition[0], pose.vecPosition[1], pose.vecPosition[2] };
	if(driverConfig.streamFrame.derivePreFilter == 1){
		state.rawPos[2][0] = state.rawPos[1][0]; state.rawPos[2][1] = state.rawPos[1][1]; state.rawPos[2][2] = state.rawPos[1][2];
		state.rawPos[1][0] = state.rawPos[0][0]; state.rawPos[1][1] = state.rawPos[0][1]; state.rawPos[1][2] = state.rawPos[0][2];
		state.rawPos[0][0] = pushPos[0]; state.rawPos[0][1] = pushPos[1]; state.rawPos[0][2] = pushPos[2];
		if(state.rawCount < 3){ state.rawCount++; }
		if(state.rawCount == 3){
			for(int a = 0; a < 3; a++){
				double x = state.rawPos[0][a], y = state.rawPos[1][a], z = state.rawPos[2][a];
				double lo = x < y ? (x < z ? x : z) : (y < z ? y : z);
				double hi = x > y ? (x > z ? x : z) : (y > z ? y : z);
				pushPos[a] = x + y + z - lo - hi;
			}
		}
	}else{
		state.rawCount = 0;
	}
	// pre-smoothing EMA (adjustable strength): maintained whenever the
	// knob is nonzero so the smoothed ring is warm; consumed by the
	// secant (always, when on) and by the fit (scope=both)
	double smoothMs = driverConfig.streamFrame.derivePreSmoothMs;
	if(smoothMs > 0.001){
		if(smoothMs > 100.0){ smoothMs = 100.0; }
		double sdt = state.count > 0 ? now - state.time[(state.head + VelFixState::ringSize - 1) % VelFixState::ringSize] : 0.0;
		if(!state.haveSm || sdt <= 0 || sdt > 0.1){
			state.smPosEma[0] = pushPos[0];
			state.smPosEma[1] = pushPos[1];
			state.smPosEma[2] = pushPos[2];
			state.smQuatEma = pose.qRotation;
			state.haveSm = true;
		}else{
			double sAlpha = 1.0 - exp(-sdt / (smoothMs / 1000.0));
			for(int a = 0; a < 3; a++){
				state.smPosEma[a] += sAlpha * (pushPos[a] - state.smPosEma[a]);
			}
			// nlerp EMA toward the incoming orientation (hemisphere safe)
			vr::HmdQuaternion_t q = pose.qRotation;
			double dot = q.w * state.smQuatEma.w + q.x * state.smQuatEma.x + q.y * state.smQuatEma.y + q.z * state.smQuatEma.z;
			double sgn = dot < 0 ? -1.0 : 1.0;
			state.smQuatEma.w += sAlpha * (sgn * q.w - state.smQuatEma.w);
			state.smQuatEma.x += sAlpha * (sgn * q.x - state.smQuatEma.x);
			state.smQuatEma.y += sAlpha * (sgn * q.y - state.smQuatEma.y);
			state.smQuatEma.z += sAlpha * (sgn * q.z - state.smQuatEma.z);
			double qn = sqrt(state.smQuatEma.w * state.smQuatEma.w + state.smQuatEma.x * state.smQuatEma.x
				+ state.smQuatEma.y * state.smQuatEma.y + state.smQuatEma.z * state.smQuatEma.z);
			if(qn > 1e-9){
				state.smQuatEma.w /= qn; state.smQuatEma.x /= qn; state.smQuatEma.y /= qn; state.smQuatEma.z /= qn;
			}
		}
	}else{
		state.haveSm = false;
	}
	state.smPos[state.head][0] = state.haveSm ? state.smPosEma[0] : pushPos[0];
	state.smPos[state.head][1] = state.haveSm ? state.smPosEma[1] : pushPos[1];
	state.smPos[state.head][2] = state.haveSm ? state.smPosEma[2] : pushPos[2];
	state.smQuat[state.head] = state.haveSm ? state.smQuatEma : pose.qRotation;
	bool feedFitSmoothed = state.haveSm && driverConfig.streamFrame.derivePreSmoothScope == 1;
	state.pos[state.head][0] = feedFitSmoothed ? state.smPos[state.head][0] : pushPos[0];
	state.pos[state.head][1] = feedFitSmoothed ? state.smPos[state.head][1] : pushPos[1];
	state.pos[state.head][2] = feedFitSmoothed ? state.smPos[state.head][2] : pushPos[2];
	state.quat[state.head] = pose.qRotation;
	state.time[state.head] = now;
	state.head = (state.head + 1) % VelFixState::ringSize;
	if(state.count < VelFixState::ringSize){
		state.count++;
		state.haveEma = false;
		state.haveEmaAng = false;
	}
	if(state.count < VelFixState::ringSize){
		return false;
	}
	
	// quadratic least squares over the whole ring, derivative evaluated at
	// the NEWEST sample (Savitzky-Golay style endpoint derivative). a linear
	// fit's slope is the velocity at the window CENTROID (~35ms ago), and
	// during a wrist snap that lag is ~20 degrees of arc = throws flying in
	// wrong directions. the quadratic term captures the arc's curvature so
	// the endpoint evaluation has near zero lag while every sample still
	// contributes to noise averaging.
	double tMean = 0;
	for(int i = 0; i < VelFixState::ringSize; i++){
		tMean += state.time[i];
	}
	tMean /= VelFixState::ringSize;
	double s2 = 0, s3 = 0, s4 = 0;
	double sp[3] = {0, 0, 0}, spt[3] = {0, 0, 0}, spt2[3] = {0, 0, 0};
	for(int i = 0; i < VelFixState::ringSize; i++){
		double dt = state.time[i] - tMean;
		double dt2 = dt * dt;
		s2 += dt2; s3 += dt2 * dt; s4 += dt2 * dt2;
		for(int a = 0; a < 3; a++){
			double p = state.pos[i][a];
			sp[a] += p; spt[a] += p * dt; spt2[a] += p * dt2;
		}
	}
	// normal equations for [a, b, c] over basis [1, t, t^2] with centered t
	// (sum of t is 0): | n 0 s2 ; 0 s2 s3 ; s2 s3 s4 |. closed form cramer
	// solutions for b and c (verified against brute force fits):
	//   det = n(s2 s4 - s3^2) - s2^3
	//   b   = (n s4 Spt - n s3 Spt2 + s2 s3 Sp - s2^2 Spt) / det
	//   c   = (n s2 Spt2 - n s3 Spt - s2^2 Sp) / det
	const double n = (double)VelFixState::ringSize;
	double det = n * (s2 * s4 - s3 * s3) - s2 * s2 * s2;
	if(fabs(det) <= 1e-18 || s2 <= 1e-9){
		return false;
	}
	int newestIdx = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
	double tN = state.time[newestIdx] - tMean;
	double slope[3];
	for(int a = 0; a < 3; a++){
		double b = (n * s4 * spt[a] - n * s3 * spt2[a] + s2 * s3 * sp[a] - s2 * s2 * spt[a]) / det;
		double c = (n * s2 * spt2[a] - n * s3 * spt[a] - s2 * s2 * sp[a]) / det;
		// v(t) = b + 2 c t, evaluated at the newest sample
		slope[a] = b + 2.0 * c * tN;
	}
	// angular velocity through the same machinery: express each ring
	// orientation as a rotation vector relative to the middle sample
	// (halves the max angle, keeping the small angle linearization honest:
	// < ~0.4 rad within the window even at 10 rad/s), fit the same
	// endpoint evaluated quadratic to the rotation vector series, then map
	// the derivative back to world axes through the reference orientation.
	int refIdx = (state.head + VelFixState::ringSize / 2) % VelFixState::ringSize;
	vr::HmdQuaternion_t qRef = state.quat[refIdx];
	vr::HmdQuaternion_t qRefConj = {qRef.w, -qRef.x, -qRef.y, -qRef.z};
	double sr[3] = {0, 0, 0}, srt[3] = {0, 0, 0}, srt2[3] = {0, 0, 0};
	for(int i = 0; i < VelFixState::ringSize; i++){
		vr::HmdQuaternion_t dq = QuatMultiply(qRefConj, state.quat[i]);
		double sign = dq.w < 0 ? -1.0 : 1.0;
		double vn = sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
		double angle = 2.0 * atan2(vn, fabs(dq.w));
		double scale = vn > 1e-9 ? sign * angle / vn : sign * 2.0;
		double r[3] = {dq.x * scale, dq.y * scale, dq.z * scale};
		double dt = state.time[i] - tMean;
		for(int a = 0; a < 3; a++){
			sr[a] += r[a]; srt[a] += r[a] * dt; srt2[a] += r[a] * dt * dt;
		}
	}
	double angSlopeRef[3];
	for(int a = 0; a < 3; a++){
		double b = (n * s4 * srt[a] - n * s3 * srt2[a] + s2 * s3 * sr[a] - s2 * s2 * srt[a]) / det;
		double c = (n * s2 * srt2[a] - n * s3 * srt[a] - s2 * s2 * sr[a]) / det;
		angSlopeRef[a] = b + 2.0 * c * tN;
	}
	double angSlope[3];
	QuatRotateVector(qRef, angSlopeRef, angSlope);
	
	// light EMA for smoothness in time (kept small: it adds lag back)
	if(!state.haveEma){
		state.haveEma = true;
		state.emaVel[0] = slope[0];
		state.emaVel[1] = slope[1];
		state.emaVel[2] = slope[2];
	}else{
		state.emaVel[0] = state.emaVel[0] * 0.65 + slope[0] * 0.35;
		state.emaVel[1] = state.emaVel[1] * 0.65 + slope[1] * 0.35;
		state.emaVel[2] = state.emaVel[2] * 0.65 + slope[2] * 0.35;
	}
	if(!state.haveEmaAng){
		state.haveEmaAng = true;
		state.emaAng[0] = angSlope[0];
		state.emaAng[1] = angSlope[1];
		state.emaAng[2] = angSlope[2];
	}else{
		state.emaAng[0] = state.emaAng[0] * 0.65 + angSlope[0] * 0.35;
		state.emaAng[1] = state.emaAng[1] * 0.65 + angSlope[1] * 0.35;
		state.emaAng[2] = state.emaAng[2] * 0.65 + angSlope[2] * 0.35;
	}
	derivedVel[0] = state.emaVel[0];
	derivedVel[1] = state.emaVel[1];
	derivedVel[2] = state.emaVel[2];
	derivedAng[0] = state.emaAng[0];
	derivedAng[1] = state.emaAng[1];
	derivedAng[2] = state.emaAng[2];
	ComputeRingSecant(state, state.haveSm, secantVel, secantAng);
	return true;
}

void CustomHeadsetDeviceProvider::LogDevicePose(uint32_t openVRID, const vr::DriverPose_t &pose){
	double now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
	double speed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
		+ pose.vecVelocity[1] * pose.vecVelocity[1]
		+ pose.vecVelocity[2] * pose.vecVelocity[2]);
	double angularSpeed = sqrt(pose.vecAngularVelocity[0] * pose.vecAngularVelocity[0]
		+ pose.vecAngularVelocity[1] * pose.vecAngularVelocity[1]
		+ pose.vecAngularVelocity[2] * pose.vecAngularVelocity[2]);
	
	// steady line every 2s per device; burst lines (max 100Hz per device)
	// while linear speed exceeds 2 m/s, which is what captures throw arcs
	// and the velocity reported at the moment of release.
	bool steady = false;
	bool burst = false;
	bool announce = false;
	bool trackChange = false;
	double peakForLog = 0;
	double fdSpeed = 0;
	double fdAngSpeed = 0;
	{
		// resolve streamed identity BEFORE taking poseLogLock: on a cache miss
	// IsStreamedController queries VRProperties, which must never happen
	// under this lock (the ResolveContainerId self-deadlock class)
	bool streamedForSnapshot = driverConfig.streamFrame.velocityFixMode == 2
		? IsStreamedController(openVRID) : false;
	std::lock_guard<std::mutex> guard(poseLogLock);
		PoseLogState &state = poseLogStates[openVRID];
		if(!state.announced){
			state.announced = true;
			announce = true;
		}
		// velocity derived from position deltas, lightly smoothed. if the
		// reported |v| saturates near 2 m/s while this keeps climbing during
		// a throw, the clamp lives in the driver's reported velocity and can
		// be replaced from poses.
		// min dt guard: vrlink resubmits re-predicted poses fractions of a
		// millisecond apart; dividing mm differences by sub-ms dt produced
		// absurd fd spikes (field data: 90 m/s at rest) and false bursts.
		// teleport-scale instants are dropped instead of averaged in.
		if(state.havePos && now - state.lastSampleTime >= 0.003 && now - state.lastSampleTime < 0.1){
			double dt = now - state.lastSampleTime;
			double dx = pose.vecPosition[0] - state.lastPos[0];
			double dy = pose.vecPosition[1] - state.lastPos[1];
			double dz = pose.vecPosition[2] - state.lastPos[2];
			double instant = sqrt(dx * dx + dy * dy + dz * dz) / dt;
			if(instant < 30.0){
				state.fdSpeedEma = state.fdSpeedEma * 0.7 + instant * 0.3;
			}
			// quaternion derived angular speed for the same comparison on
			// the rotational side: 2 acos(|<q1,q2>|) / dt
			if(state.haveQuat){
				double dot = state.lastQuat.w * pose.qRotation.w + state.lastQuat.x * pose.qRotation.x
					+ state.lastQuat.y * pose.qRotation.y + state.lastQuat.z * pose.qRotation.z;
				if(dot < 0){ dot = -dot; }
				if(dot > 1.0){ dot = 1.0; }
				double angInstant = 2.0 * acos(dot) / dt;
				if(angInstant < 100.0){
					state.fdAngSpeedEma = state.fdAngSpeedEma * 0.7 + angInstant * 0.3;
				}
			}
			state.lastQuat = pose.qRotation;
			state.haveQuat = true;
		}else if(!state.havePos){
			state.lastQuat = pose.qRotation;
			state.haveQuat = true;
		}
		if(now - state.lastSampleTime >= 0.003 || !state.havePos){
			state.lastPos[0] = pose.vecPosition[0];
			state.lastPos[1] = pose.vecPosition[1];
			state.lastPos[2] = pose.vecPosition[2];
			state.lastSampleTime = now;
			state.havePos = true;
		}
		fdSpeed = state.fdSpeedEma;
		fdAngSpeed = state.fdAngSpeedEma;
		// with velocityFix off the fix path never runs, so record the raw
		// pose as the release snapshot here — A/B sessions then get
		// ReleaseSnap lines in both arms
		// full mode records snapshots in the fix path; off and classic
		// record here (classic's post-blend velocities are what this pose
		// carries by the time logging runs)
		if(driverConfig.streamFrame.velocityFixMode != 2 || !streamedForSnapshot){
			MotionSnapshot &snap = motionSnapshots[openVRID];
			snap.time = now;
			snap.outVel[0] = pose.vecVelocity[0];
			snap.outVel[1] = pose.vecVelocity[1];
			snap.outVel[2] = pose.vecVelocity[2];
			snap.outAng[0] = pose.vecAngularVelocity[0];
			snap.outAng[1] = pose.vecAngularVelocity[1];
			snap.outAng[2] = pose.vecAngularVelocity[2];
			snap.outSpeed = speed;
			snap.trackingOk = pose.poseIsValid && pose.result == vr::TrackingResult_Running_OK;
			snap.result = (int)pose.result;
		}
		if(speed > state.peakSpeed){
			state.peakSpeed = speed;
		}
		// tracking state transitions are logged immediately (dropouts during
		// fast motion zero the speed, so speed-triggered bursts miss them —
		// exactly the "item falls straight down" moments)
		bool nowValid = pose.poseIsValid;
		int nowResult = (int)pose.result;
		if(!state.haveTrackState){
			state.haveTrackState = true;
			state.lastLoggedValid = nowValid;
			state.lastLoggedResult = nowResult;
		}else if((nowValid != state.lastLoggedValid || nowResult != state.lastLoggedResult)
				&& now - state.lastBurstLog >= 0.005){
			state.lastLoggedValid = nowValid;
			state.lastLoggedResult = nowResult;
			state.lastBurstLog = now;
			trackChange = true;
		}
		// keep burst logging alive for 300ms after fast motion so the
		// post release phase (including any dropout / zeroing) is captured.
		// EFFECTIVE speed (|v| + 0.15|w|): pure wrist flicks are w-dominant
		// with little linear motion, and a linear-only trigger made them
		// systematically invisible to the diagnostics (field 2026-08-10:
		// 3 flick samples out of 581)
		double effSpeed = speed + 0.15 * angularSpeed;
		double fdEffSpeed = fdSpeed + 0.15 * fdAngSpeed;
		if(effSpeed > 2.0 || fdEffSpeed > 2.0){
			state.recentFastTime = now;
		}
		bool inPostFastWindow = now - state.recentFastTime < 0.3;
		if(now - state.lastSteadyLog >= 2.0){
			state.lastSteadyLog = now;
			steady = true;
			peakForLog = state.peakSpeed;
			state.peakSpeed = 0;
		}else if((effSpeed > 2.0 || fdEffSpeed > 2.0 || inPostFastWindow)
				&& driverConfig.streamFrame.poseLogBurst
				&& now - state.lastBurstLog >= 0.01){
			state.lastBurstLog = now;
			burst = true;
		}
	}
	if(trackChange){
		DriverLog("PoseLog: TRACKING id=%u valid=%d result=%d |v|=%.3f fd|v|=%.3f pos=(%.3f, %.3f, %.3f)",
			openVRID, (int)pose.poseIsValid, (int)pose.result, speed, fdSpeed,
			pose.vecPosition[0], pose.vecPosition[1], pose.vecPosition[2]);
	}
	if(announce){
		// resolve which physical device this id is, once, so pose lines are
		// attributable without guessing at activation order
		char serial[128] = {};
		vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(openVRID);
		vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
		vr::VRProperties()->GetStringProperty(container, vr::Prop_SerialNumber_String, serial, sizeof(serial), &propError);
		DriverLog("PoseLog: id=%u serial=%s", openVRID,
			propError == vr::TrackedProp_Success ? serial : "(unknown)");
	}
	if(steady){
		DriverLog("PoseLog: id=%u pos=(%.3f, %.3f, %.3f) |v|=%.3f fd|v|=%.3f |w|=%.2f fd|w|=%.2f peak|v|=%.3f valid=%d connected=%d result=%d timeOffset=%.4f",
			openVRID, pose.vecPosition[0], pose.vecPosition[1], pose.vecPosition[2],
			speed, fdSpeed, angularSpeed, fdAngSpeed, peakForLog,
			(int)pose.poseIsValid, (int)pose.deviceIsConnected, (int)pose.result,
			pose.poseTimeOffset);
	}else if(burst){
		DriverLog("PoseLog: BURST id=%u v=(%.3f, %.3f, %.3f) |v|=%.3f fd|v|=%.3f |w|=%.2f fd|w|=%.2f valid=%d result=%d timeOffset=%.4f",
			openVRID, pose.vecVelocity[0], pose.vecVelocity[1], pose.vecVelocity[2],
			speed, fdSpeed, angularSpeed, fdAngSpeed, (int)pose.poseIsValid, (int)pose.result,
			pose.poseTimeOffset);
	}
}

bool CustomHeadsetDeviceProvider::HandleDeviceAdded(const char *&pchDeviceSerialNumber, vr::ETrackedDeviceClass &eDeviceClass, vr::ITrackedDeviceServerDriver *&pDriver){
	#ifdef HAS_PRIVATE
	if(driverConfig.onlyHandlePrivateFunctionality){
		return true;
	}
	#endif
	DriverLog("HandleDeviceAdded %s\n", pchDeviceSerialNumber);
	if(eDeviceClass == vr::TrackedDeviceClass_HMD){
		// keep the (possibly later wrapped) source device for projection
		// queries; GetComponent forwards through shims either way
		hmdDevice = pDriver;
		
		// add more shims here, they can stack and none of the functions are particularly hot
		// later shims can override earlier shims
		// the PosTrackedDeviceActivate function will likely have enough information that you can decide if it is the device you want and can then set shimActive to false to deactivate the shim
		
		// TODO: validate the interface versions of drivers and make the shims conform to versions to prevent potential crashes
		
		if(driverConfig.dreamAir.enable){
			DreamAirShim* dreamAirShim = new DreamAirShim();
			dreamAirShim->deviceProvider = this;
			shims.insert(dreamAirShim);
			pDriver = new ShimTrackedDeviceDriver(dreamAirShim, pDriver);
		}
		
		if(driverConfig.meganeX8K.enable){
			MeganeX8KShim* meganeX8KShim = new MeganeX8KShim();
			meganeX8KShim->deviceProvider = this;
			shims.insert(meganeX8KShim);
			pDriver = new ShimTrackedDeviceDriver(meganeX8KShim, pDriver);
		}
		
		GenericHeadsetShim* genericHeadsetShim = new GenericHeadsetShim();
		genericHeadsetShim->deviceProvider = this;
		shims.insert(genericHeadsetShim);
		pDriver = new ShimTrackedDeviceDriver(genericHeadsetShim, pDriver);
		
		#ifdef VENDOR_GALAXYXR
		if(driverConfig.galaxyXr.nativeIdentity){
			GalaxyXRHmdShim* galaxyXrHmdShim = new GalaxyXRHmdShim();
			galaxyXrHmdShim->deviceProvider = this;
			shims.insert(galaxyXrHmdShim);
			pDriver = new ShimTrackedDeviceDriver(galaxyXrHmdShim, pDriver);
		}
		#endif
	}
	#ifdef VENDOR_GALAXYXR
	// the controller shim carries identity (models, icons), the input
	// profile and the official pose components. it is created for every
	// streamed controller whenever any of those is wanted; no identity
	// checks beyond the serial (2026-08-26: APK identities are unreliable,
	// the user picked this driver for a Galaxy XR, stamp on request).
	if(eDeviceClass == vr::TrackedDeviceClass_Controller && !driverConfig.galaxyXr.controllerBypass
			&& (driverConfig.galaxyXr.nativeIdentity || driverConfig.galaxyXr.nativeInputProfile)){
		std::string serial = pchDeviceSerialNumber ? pchDeviceSerialNumber : "";
		// SamsungVST-Controller-* on the patched APK, VRLINKQ2_Controller_* on
		// the stock one. "Controller" excludes the VRLINKQ_Hand_* hand trackers.
		bool streamedController = (serial.rfind("SamsungVST-Controller", 0) == 0)
			|| (serial.rfind("VRLINK", 0) == 0 && serial.find("Controller") != std::string::npos);
		if(streamedController){
			GalaxyXRControllerShim* controllerShim = new GalaxyXRControllerShim(serial);
			shims.insert(controllerShim);
			pDriver = new ShimTrackedDeviceDriver(controllerShim, pDriver);
		}
	}
	#endif
	// you can change eDeviceClass to change what an existing device shows up as
	
	// if false is returned the device will not be added
	return true;
}

void CustomHeadsetDeviceProvider::GetTunerInput(TunerInputState &out){
	out = TunerInputState();
	std::lock_guard<std::mutex> guard(poseLogLock);
	for(const auto &pair : inputComponents){
		const InputComponentInfo &info = pair.second;
		switch(info.tunerRole){
			case 1:
				// largest-magnitude joystick y across hands, so either stick
				// nudges and an idle stick cannot cancel a deflected one
				if(fabsf(info.tunerScalar) > fabsf(out.stickY)){ out.stickY = info.tunerScalar; }
				break;
			case 2: out.bandOut = out.bandOut || info.tunerBool; break;
			case 3: out.bandIn = out.bandIn || info.tunerBool; break;
			case 4: out.eyeToggle = out.eyeToggle || info.tunerBool; break;
			case 5: out.resetBand = out.resetBand || info.tunerBool; break;
			case 6: if(info.tunerScalar > out.grip){ out.grip = info.tunerScalar; } break;
			case 7:
				if(fabsf(info.tunerScalar) > fabsf(out.stickX)){ out.stickX = info.tunerScalar; }
				break;
			case 8: if(info.tunerScalar > out.trigger){ out.trigger = info.tunerScalar; } break;
			case 9: out.segToggle = out.segToggle || info.tunerBool; break;
		}
	}
}

void CustomHeadsetDeviceProvider::GetAlignController(int hand, AlignControllerState &out){
	std::lock_guard<std::mutex> guard(poseLogLock);
	if(hand == 0 || hand == 1){
		out = alignControllers[hand];
	}else{
		out = AlignControllerState();
	}
}

void CustomHeadsetDeviceProvider::SetAlignerOffsets(bool active, const double rotDeg[3], const double posCm[3]){
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		for(int i = 0; i < 3; i++){
			alignerRotDeg[i] = rotDeg[i];
			alignerPosCm[i] = posCm[i];
		}
		if(!active){
			alignerAppliedLogged = false;
		}
	}
	alignerOverrideActive.store(active, std::memory_order_relaxed);
}

void CustomHeadsetDeviceProvider::SetAlignerGrip(bool active, const double gripCm[2][3]){
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		for(int hand = 0; hand < 2; hand++){
			for(int i = 0; i < 3; i++){
				alignerGripCm[hand][i] = gripCm[hand][i];
			}
		}
	}
	alignerGripActive.store(active, std::memory_order_relaxed);
}
