#include "ConfigLoader.h"
#include <map>
#include <thread>
#include <fstream>
#include <filesystem>
#include <chrono>
#include "nlohmann/json.hpp"
#include "../Driver/DriverLog.h"
#include "../Distortion/DistortionProfileConstructor.h"
#ifdef _WIN32
#include "Windows.h"
#else
#include <unistd.h>
#endif

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;


std::string ConfigLoader::GetConfigFolder(){
	// vendor builds keep their config in a separate folder so they can coexist
	// with the vendor-neutral CustomHeadsetOpenVR driver without sharing state
	#ifdef VENDOR_GALAXYXR
	std::string configFolder = "GalaxyXR/CustomHeadset/";
	#else
	std::string configFolder = "CustomHeadset/";
	#endif
	#ifdef _WIN32
	char* appdataPath = std::getenv("APPDATA");
	std::string configPath = appdataPath == nullptr ? "./" : (std::string(appdataPath) + "/" + configFolder);
	#elif __linux__
	char* appdataPath = std::getenv("HOME");
	std::string configPath = appdataPath == nullptr ? "./" : (std::string(appdataPath) + "/.config/" + configFolder);
	#endif
	return configPath;
}

// legacy path used by this fork before the vendor split (also the neutral driver's path)
static std::string GetLegacyConfigFolder(){
	#ifdef _WIN32
	char* appdataPath = std::getenv("APPDATA");
	return appdataPath == nullptr ? "./" : (std::string(appdataPath) + "/CustomHeadset/");
	#elif __linux__
	char* appdataPath = std::getenv("HOME");
	return appdataPath == nullptr ? "./" : (std::string(appdataPath) + "/.config/CustomHeadset/");
	#endif
}

// one-time migration for the vendor build: users of this fork stored settings
// in the legacy CustomHeadset folder before the driver was renamed. if the
// vendor folder has no settings yet, copy settings.json and the distortion
// profiles over (copy, never move: the legacy folder may also be in use by
// the vendor-neutral driver, whose settings share the same base schema).
// info.json/diagnostic.json are regenerated at runtime and are not migrated.
void ConfigLoader::MigrateLegacyConfig(){
	#ifdef VENDOR_GALAXYXR
	try{
		std::string vendorFolder = GetConfigFolder();
		std::string vendorSettings = vendorFolder + "settings.json";
		if(std::filesystem::exists(vendorSettings)){
			return;
		}
		std::string legacyFolder = GetLegacyConfigFolder();
		std::string legacySettings = legacyFolder + "settings.json";
		if(!std::filesystem::exists(legacySettings)){
			return;
		}
		DriverLog("Migrating settings from %s to %s", legacyFolder.c_str(), vendorFolder.c_str());
		std::filesystem::create_directories(vendorFolder);
		std::filesystem::copy_file(legacySettings, vendorSettings, std::filesystem::copy_options::skip_existing);
		std::string legacyDistortion = legacyFolder + "Distortion";
		if(std::filesystem::exists(legacyDistortion) && std::filesystem::is_directory(legacyDistortion)){
			std::filesystem::copy(legacyDistortion, vendorFolder + "Distortion",
				std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing);
		}
		DriverLog("Settings migration complete");
	}catch(const std::exception& e){
		DriverLog("Settings migration failed: %s", e.what());
	}
	#endif
}

void parseBaseHeadsetConfig(json headsetData, Config::BaseHeadsetConfig& headsetConfig){
	if(headsetData["enable"].is_boolean()){
		headsetConfig.enable = headsetData["enable"].get<bool>();
	}
	if(headsetData["ipd"].is_number()){
		headsetConfig.ipd = headsetData["ipd"].get<double>();
	}
	if(headsetData["ipdOffset"].is_number()){
		headsetConfig.ipdOffset = headsetData["ipdOffset"].get<double>();
	}
	if(headsetData["horizontalIPDOffset"].is_number()){
		headsetConfig.horizontalIPDOffset = headsetData["horizontalIPDOffset"].get<double>();
	}
	if(headsetData["blackLevel"].is_number()){
		headsetConfig.blackLevel = headsetData["blackLevel"].get<double>();
	}
	if(headsetData["colorMultiplier"].is_object()){
		ConfigColor &colorMultiplier = headsetConfig.colorMultiplier;
		if(headsetData["colorMultiplier"]["r"].is_number()){ colorMultiplier.r = headsetData["colorMultiplier"]["r"].get<double>(); }
		if(headsetData["colorMultiplier"]["g"].is_number()){ colorMultiplier.g = headsetData["colorMultiplier"]["g"].get<double>(); }
		if(headsetData["colorMultiplier"]["b"].is_number()){ colorMultiplier.b = headsetData["colorMultiplier"]["b"].get<double>(); }
	}
	if(headsetData["distortionProfile"].is_string()){
		headsetConfig.distortionProfile = headsetData["distortionProfile"].get<std::string>();
	}
	if(headsetData["distortionZoom"].is_number()){
		headsetConfig.distortionZoom = headsetData["distortionZoom"].get<double>();
	}
	if(headsetData["fovZoom"].is_number()){
		headsetConfig.fovZoom = headsetData["fovZoom"].get<double>();
	}
	if(headsetData["flatFovZoom"].is_number()){
		headsetConfig.flatFovZoom = headsetData["flatFovZoom"].get<double>();
	}
	if(headsetData["subpixelShift"].is_number()){
		headsetConfig.subpixelShift = headsetData["subpixelShift"].get<double>();
	}
	if(headsetData["subpixelOffsets"].is_array()){
		headsetConfig.subpixelOffsets = headsetData["subpixelOffsets"].get<std::vector<double>>();
	}
	if(headsetData["resolutionX"].is_number()){
		headsetConfig.resolutionX = headsetData["resolutionX"].get<int>();
	}
	if(headsetData["resolutionY"].is_number()){
		headsetConfig.resolutionY = headsetData["resolutionY"].get<int>();
	}
	if(headsetData["displayRotation"].is_number()){
		headsetConfig.displayRotation = headsetData["displayRotation"].get<int>();
	}
	if(headsetData["maxFovX"].is_number()){
		headsetConfig.maxFovX = headsetData["maxFovX"].get<double>();
	}
	if(headsetData["maxFovY"].is_number()){
		headsetConfig.maxFovY = headsetData["maxFovY"].get<double>();
	}
	if(headsetData["distortionMeshResolution"].is_number()){
		headsetConfig.distortionMeshResolution = headsetData["distortionMeshResolution"].get<int>();
	}
	if(headsetData["fovBurnInPrevention"].is_boolean()){
		headsetConfig.fovBurnInPrevention = headsetData["fovBurnInPrevention"].get<bool>();
	}
	if(headsetData["fovClamping"].is_boolean()){
		headsetConfig.fovClamping = headsetData["fovClamping"].get<bool>();
	}
	if(headsetData["distortionProfileDeviceType"].is_string()){
		headsetConfig.distortionProfileDeviceType = headsetData["distortionProfileDeviceType"].get<std::string>();
	}
	if(headsetData["renderResolutionMultiplierX"].is_number()){
		headsetConfig.renderResolutionMultiplierX = headsetData["renderResolutionMultiplierX"].get<double>();
	}
	if(headsetData["renderResolutionMultiplierY"].is_number()){
		headsetConfig.renderResolutionMultiplierY = headsetData["renderResolutionMultiplierY"].get<double>();
	}
	if(headsetData["superSamplingFilterPercent"].is_number()){
		headsetConfig.superSamplingFilterPercent = headsetData["superSamplingFilterPercent"].get<double>();
	}
	if(headsetData["secondsFromVsyncToPhotons"].is_number()){
		headsetConfig.secondsFromVsyncToPhotons = headsetData["secondsFromVsyncToPhotons"].get<double>();
	}
	if(headsetData["secondsFromPhotonsToVblank"].is_number()){
		headsetConfig.secondsFromPhotonsToVblank = headsetData["secondsFromPhotonsToVblank"].get<double>();
	}
	if(headsetData["eyeRotation"].is_number()){
		headsetConfig.eyeRotation = headsetData["eyeRotation"].get<double>();
	}
	if(headsetData["disableEye"].is_number()){
		headsetConfig.disableEye = headsetData["disableEye"].get<int>();
	}
	if(headsetData["disableEyeDecreaseFov"].is_boolean()){
		headsetConfig.disableEyeDecreaseFov = headsetData["disableEyeDecreaseFov"].get<bool>();
	}
	if(headsetData["useViveBluetooth"].is_boolean()){
		headsetConfig.useViveBluetooth = headsetData["useViveBluetooth"].get<bool>();
	}
	if(headsetData["directMode"].is_boolean()){
		headsetConfig.directMode = headsetData["directMode"].get<bool>();
	}
	if(headsetData["replaceIcons"].is_boolean()){
		headsetConfig.replaceIcons = headsetData["replaceIcons"].get<bool>();
	}
	if(headsetData["edidVendorIdOverride"].is_number()){
		headsetConfig.edidVendorIdOverride = headsetData["edidVendorIdOverride"].get<int>();
	}
	if(headsetData["edidProductIdOverride"].is_number()){
		headsetConfig.edidProductIdOverride = headsetData["edidProductIdOverride"].get<int>();
	}
	if(headsetData["dscVersion"].is_number()){
		headsetConfig.dscVersion = headsetData["dscVersion"].get<int>();
	}
	if(headsetData["dscSliceCount"].is_number()){
		headsetConfig.dscSliceCount = headsetData["dscSliceCount"].get<int>();
	}
	if(headsetData["dscBPPx16"].is_number()){
		headsetConfig.dscBPPx16 = headsetData["dscBPPx16"].get<int>();
	}
	if(headsetData["forceEnable"].is_boolean()){
		headsetConfig.forceEnable = headsetData["forceEnable"].get<bool>();
	}
	if(headsetData["parallelProjection"].is_boolean()){
		headsetConfig.parallelProjection = headsetData["parallelProjection"].get<bool>();
	}
	if(headsetData["enableEyeTracking"].is_boolean()){
		headsetConfig.enableEyeTracking = headsetData["enableEyeTracking"].get<bool>();
	}
	
	if(json& hiddenAreaJson = headsetData["hiddenArea"]; hiddenAreaJson.is_object()){
		auto& newHiddenArea = headsetConfig.hiddenArea;
		if(hiddenAreaJson["enable"].is_boolean()){ newHiddenArea.enable = hiddenAreaJson["enable"].get<bool>(); }
		if(hiddenAreaJson["testMode"].is_boolean()){ newHiddenArea.testMode = hiddenAreaJson["testMode"].get<bool>(); }
		if(hiddenAreaJson["detailLevel"].is_number()){ newHiddenArea.detailLevel = hiddenAreaJson["detailLevel"].get<int>(); }
		if(hiddenAreaJson["radiusTopOuter"].is_number()){ newHiddenArea.radiusTopOuter = hiddenAreaJson["radiusTopOuter"].get<double>(); }
		if(hiddenAreaJson["radiusTopInner"].is_number()){ newHiddenArea.radiusTopInner = hiddenAreaJson["radiusTopInner"].get<double>(); }
		if(hiddenAreaJson["radiusBottomInner"].is_number()){ newHiddenArea.radiusBottomInner = hiddenAreaJson["radiusBottomInner"].get<double>(); }
		if(hiddenAreaJson["radiusBottomOuter"].is_number()){ newHiddenArea.radiusBottomOuter = hiddenAreaJson["radiusBottomOuter"].get<double>(); }
	}
	if(json& stationaryDimmingJson = headsetData["stationaryDimming"]; stationaryDimmingJson.is_object()){
		auto& newStationaryDimming = headsetConfig.stationaryDimming;
		if(stationaryDimmingJson["enable"].is_boolean()){ newStationaryDimming.enable = stationaryDimmingJson["enable"].get<bool>(); }
		if(stationaryDimmingJson["movementThreshold"].is_number()){ newStationaryDimming.movementThreshold = stationaryDimmingJson["movementThreshold"].get<double>(); }
		if(stationaryDimmingJson["movementTime"].is_number()){ newStationaryDimming.movementTime = stationaryDimmingJson["movementTime"].get<double>(); }
		if(stationaryDimmingJson["dimBrightnessPercent"].is_number()){ newStationaryDimming.dimBrightnessPercent = stationaryDimmingJson["dimBrightnessPercent"].get<double>(); }
		if(stationaryDimmingJson["dimSeconds"].is_number()){ newStationaryDimming.dimSeconds = stationaryDimmingJson["dimSeconds"].get<double>(); }
		if(stationaryDimmingJson["brightenSeconds"].is_number()){ newStationaryDimming.brightenSeconds = stationaryDimmingJson["brightenSeconds"].get<double>(); }
	}
}

void ConfigLoader::ParseConfig(){
	// acquire driverConfig.configLock for the duration of this function
	// std::lock_guard<std::mutex> lock(driverConfig.configLock);
	// get file at APPDATA/Roaming/CustomHeadset/settings.json
	std::string configPath = GetConfigFolder() + "settings.json";
	std::ifstream configFile(configPath);
	if(!configFile.is_open()){
		if(!hasLoggedConfigFileNotFound){
			hasLoggedConfigFileNotFound = true;
			DriverLog("Config file not found at %s, using default settings.", configPath.c_str());
		}
		return;
	}
	DriverLog("Loading config file from %s", configPath.c_str());
	try{
		// parse with support for comments
		json data = json::parse(configFile, nullptr, true, true);
		Config newConfig = {};
		if(data["meganeX8K"].is_object()){
			json headsetData = data["meganeX8K"];
			parseBaseHeadsetConfig(headsetData, newConfig.meganeX8K);
		}
		if(data["dreamAir"].is_object()){
			json headsetData = data["dreamAir"];
			parseBaseHeadsetConfig(headsetData, newConfig.dreamAir);
		}
		if(data["generalHeadset"].is_object()){
			json generalHeadsetData = data["generalHeadset"];
			if(generalHeadsetData["useViveBluetooth"].is_boolean()){
				newConfig.generalHeadset.useViveBluetooth = generalHeadsetData["useViveBluetooth"].get<bool>();
			}
		}
		if(data["customShader"].is_object()){
			json customShaderData = data["customShader"];
			if(customShaderData["enable"].is_boolean()){
				newConfig.customShader.enable = customShaderData["enable"].get<bool>();
			}
			if(customShaderData["enableForMeganeX8K"].is_boolean()){
				newConfig.customShader.enableForMeganeX8K = customShaderData["enableForMeganeX8K"].get<bool>();
			}
			if(customShaderData["enableForDreamAir"].is_boolean()){
				newConfig.customShader.enableForDreamAir = customShaderData["enableForDreamAir"].get<bool>();
			}
			if(customShaderData["enableForOther"].is_boolean()){
				newConfig.customShader.enableForOther = customShaderData["enableForOther"].get<bool>();
			}
			if(customShaderData["contrast"].is_number()){
				newConfig.customShader.contrast = customShaderData["contrast"].get<double>();
			}
			if(customShaderData["contrastMidpoint"].is_number()){
				newConfig.customShader.contrastMidpoint = customShaderData["contrastMidpoint"].get<double>();
			}
			if(customShaderData["contrastLinear"].is_boolean()){
				newConfig.customShader.contrastLinear = customShaderData["contrastLinear"].get<bool>();
			}
			if(customShaderData["contrastPerEye"].is_boolean()){
				newConfig.customShader.contrastPerEye = customShaderData["contrastPerEye"].get<bool>();
			}
			if(customShaderData["contrastPerEyeLinear"].is_boolean()){
				newConfig.customShader.contrastPerEyeLinear = customShaderData["contrastPerEyeLinear"].get<bool>();
			}
			if(customShaderData["contrastLeft"].is_number()){
				newConfig.customShader.contrastLeft = customShaderData["contrastLeft"].get<double>();
			}
			if(customShaderData["contrastMidpointLeft"].is_number()){
				newConfig.customShader.contrastMidpointLeft = customShaderData["contrastMidpointLeft"].get<double>();
			}
			if(customShaderData["contrastRight"].is_number()){
				newConfig.customShader.contrastRight = customShaderData["contrastRight"].get<double>();
			}
			if(customShaderData["contrastMidpointRight"].is_number()){
				newConfig.customShader.contrastMidpointRight = customShaderData["contrastMidpointRight"].get<double>();
			}
			if(customShaderData["saturation"].is_number()){
				newConfig.customShader.saturation = customShaderData["saturation"].get<double>();
			}
			if(customShaderData["gamma"].is_number()){
				newConfig.customShader.gamma = customShaderData["gamma"].get<double>();
			}
			if(customShaderData["subpixelShift"].is_boolean()){
				newConfig.customShader.subpixelShift = customShaderData["subpixelShift"].get<bool>();
			}
			if(customShaderData["disableMuraCorrection"].is_boolean()){
				newConfig.customShader.disableMuraCorrection = customShaderData["disableMuraCorrection"].get<bool>();
			}
			if(customShaderData["disableBlackLevels"].is_boolean()){
				newConfig.customShader.disableBlackLevels = customShaderData["disableBlackLevels"].get<bool>();
			}
			if(customShaderData["srgbColorCorrection"].is_boolean()){
				newConfig.customShader.srgbColorCorrection = customShaderData["srgbColorCorrection"].get<bool>();
			}
			if(customShaderData["srgbWhitePointCorrection"].is_boolean()){
				newConfig.customShader.srgbWhitePointCorrection = customShaderData["srgbWhitePointCorrection"].get<bool>();
			}
			if(customShaderData["srgbColorCorrectionMatrix"].is_array()){
				newConfig.customShader.srgbColorCorrectionMatrix = customShaderData["srgbColorCorrectionMatrix"].get<std::vector<double>>();
			}
			if(customShaderData["lensColorCorrection"].is_boolean()){
				newConfig.customShader.lensColorCorrection = customShaderData["lensColorCorrection"].get<bool>();
			}
			if(customShaderData["dither10Bit"].is_boolean()){
				newConfig.customShader.dither10Bit = customShaderData["dither10Bit"].get<bool>();
			}
			if(customShaderData["enableFilterForOverlay"].is_boolean()){
				newConfig.customShader.enableFilterForOverlay = customShaderData["enableFilterForOverlay"].get<bool>();
			}
			if(customShaderData["enableFilterForDashboard"].is_boolean()){
				newConfig.customShader.enableFilterForDashboard = customShaderData["enableFilterForDashboard"].get<bool>();
			}
			if(customShaderData["samplingFilter"].is_string()){
				newConfig.customShader.samplingFilter = customShaderData["samplingFilter"].get<std::string>();
			}
			if(customShaderData["samplingFilterFXAA2SharpenStrength"].is_number()){
				newConfig.customShader.samplingFilterFXAA2SharpenStrength = customShaderData["samplingFilterFXAA2SharpenStrength"].get<double>();
			}
			if(customShaderData["samplingFilterFXAA2SharpenClamp"].is_number()){
				newConfig.customShader.samplingFilterFXAA2SharpenClamp = customShaderData["samplingFilterFXAA2SharpenClamp"].get<double>();
			}
			if(customShaderData["samplingFilterFXAA2CASStrength"].is_number()){
				newConfig.customShader.samplingFilterFXAA2CASStrength = customShaderData["samplingFilterFXAA2CASStrength"].get<double>();
			}
			if(customShaderData["samplingFilterFXAA2CASContrast"].is_number()){
				newConfig.customShader.samplingFilterFXAA2CASContrast = customShaderData["samplingFilterFXAA2CASContrast"].get<double>();
			}
			if(customShaderData["samplingFilterLumaSharpenStrength"].is_number()){
				newConfig.customShader.samplingFilterLumaSharpenStrength = customShaderData["samplingFilterLumaSharpenStrength"].get<double>();
			}
			if(customShaderData["samplingFilterLumaSharpenClamp"].is_number()){
				newConfig.customShader.samplingFilterLumaSharpenClamp = customShaderData["samplingFilterLumaSharpenClamp"].get<double>();
			}
			if(customShaderData["samplingFilterLumaSharpenPattern"].is_number()){
				newConfig.customShader.samplingFilterLumaSharpenPattern = customShaderData["samplingFilterLumaSharpenPattern"].get<int>();
			}
			if(customShaderData["samplingFilterLumaSharpenRadius"].is_number()){
				newConfig.customShader.samplingFilterLumaSharpenRadius = customShaderData["samplingFilterLumaSharpenRadius"].get<double>();
			}
 			if(customShaderData["samplingFilterCASStrength"].is_number()){
 				newConfig.customShader.samplingFilterCASStrength = customShaderData["samplingFilterCASStrength"].get<double>();
 			}
			if(customShaderData["samplingFilterCASContrast"].is_number()){
				newConfig.customShader.samplingFilterCASContrast = customShaderData["samplingFilterCASContrast"].get<double>();
			}
			if(customShaderData["colorMultiplier"].is_object()){
				ConfigColor &colorMultiplier = newConfig.customShader.colorMultiplier;
				if(customShaderData["colorMultiplier"]["r"].is_number()){ colorMultiplier.r = customShaderData["colorMultiplier"]["r"].get<double>(); }
				if(customShaderData["colorMultiplier"]["g"].is_number()){ colorMultiplier.g = customShaderData["colorMultiplier"]["g"].get<double>(); }
				if(customShaderData["colorMultiplier"]["b"].is_number()){ colorMultiplier.b = customShaderData["colorMultiplier"]["b"].get<double>(); }
			}
		}
		if(data["galaxyXr"].is_object()){
			json galaxyXrData = data["galaxyXr"];
			if(galaxyXrData["nativeIdentity"].is_boolean()){
				newConfig.galaxyXr.nativeIdentity = galaxyXrData["nativeIdentity"].get<bool>();
			}
			if(galaxyXrData["renderModelVariant"].is_string()){
				newConfig.galaxyXr.renderModelVariant = galaxyXrData["renderModelVariant"].get<std::string>();
			}
			if(galaxyXrData["nativeInputProfile"].is_boolean()){
				newConfig.galaxyXr.nativeInputProfile = galaxyXrData["nativeInputProfile"].get<bool>();
			}
			if(galaxyXrData["nativeResolution"].is_boolean()){
				newConfig.galaxyXr.nativeResolution = galaxyXrData["nativeResolution"].get<bool>();
			}
			if(galaxyXrData["streamQuality"].is_string()){
				newConfig.galaxyXr.streamQuality = galaxyXrData["streamQuality"].get<std::string>();
			}
			if(galaxyXrData["renderModelScale"].is_number()){
				newConfig.galaxyXr.renderModelScale = galaxyXrData["renderModelScale"].get<double>();
			}
			if(galaxyXrData["gripConvention"].is_boolean()){
				newConfig.galaxyXr.gripConvention = galaxyXrData["gripConvention"].get<bool>();
			}
			if(galaxyXrData["handAnchorXCm"].is_number()){
				newConfig.galaxyXr.handAnchorXCm = galaxyXrData["handAnchorXCm"].get<double>();
			}
			if(galaxyXrData["handAnchorYCm"].is_number()){
				newConfig.galaxyXr.handAnchorYCm = galaxyXrData["handAnchorYCm"].get<double>();
			}
			if(galaxyXrData["handAnchorZCm"].is_number()){
				newConfig.galaxyXr.handAnchorZCm = galaxyXrData["handAnchorZCm"].get<double>();
			}
			if(galaxyXrData["handAnchorPitchDeg"].is_number()){
				newConfig.galaxyXr.handAnchorPitchDeg = galaxyXrData["handAnchorPitchDeg"].get<double>();
			}
			if(galaxyXrData["handAnchorYawDeg"].is_number()){
				newConfig.galaxyXr.handAnchorYawDeg = galaxyXrData["handAnchorYawDeg"].get<double>();
			}
			if(galaxyXrData["handAnchorRollDeg"].is_number()){
				newConfig.galaxyXr.handAnchorRollDeg = galaxyXrData["handAnchorRollDeg"].get<double>();
			}
			if(galaxyXrData["simulateTouch"].is_boolean()){
				newConfig.galaxyXr.simulateTouch = galaxyXrData["simulateTouch"].get<bool>();
			}
			if(galaxyXrData["aimTrimXCm"].is_number()){
				newConfig.galaxyXr.aimTrimXCm = galaxyXrData["aimTrimXCm"].get<double>();
			}
			if(galaxyXrData["aimTrimYCm"].is_number()){
				newConfig.galaxyXr.aimTrimYCm = galaxyXrData["aimTrimYCm"].get<double>();
			}
			if(galaxyXrData["aimTrimZCm"].is_number()){
				newConfig.galaxyXr.aimTrimZCm = galaxyXrData["aimTrimZCm"].get<double>();
			}
			if(galaxyXrData["componentRebaseIncludeTrim"].is_boolean()){
				newConfig.galaxyXr.componentRebaseIncludeTrim = galaxyXrData["componentRebaseIncludeTrim"].get<bool>();
			}
			if(galaxyXrData["officialComponents"].is_boolean()){
				newConfig.galaxyXr.officialComponents = galaxyXrData["officialComponents"].get<bool>();
			}
			if(galaxyXrData["meshOffsetXCm"].is_number()){
				newConfig.galaxyXr.meshOffsetXCm = galaxyXrData["meshOffsetXCm"].get<double>();
			}
			if(galaxyXrData["meshOffsetYCm"].is_number()){
				newConfig.galaxyXr.meshOffsetYCm = galaxyXrData["meshOffsetYCm"].get<double>();
			}
			if(galaxyXrData["meshOffsetZCm"].is_number()){
				newConfig.galaxyXr.meshOffsetZCm = galaxyXrData["meshOffsetZCm"].get<double>();
			}
			if(galaxyXrData["skeletonOffsetXCm"].is_number()){
				newConfig.galaxyXr.skeletonOffsetXCm = galaxyXrData["skeletonOffsetXCm"].get<double>();
			}
			if(galaxyXrData["skeletonOffsetYCm"].is_number()){
				newConfig.galaxyXr.skeletonOffsetYCm = galaxyXrData["skeletonOffsetYCm"].get<double>();
			}
			if(galaxyXrData["skeletonOffsetZCm"].is_number()){
				newConfig.galaxyXr.skeletonOffsetZCm = galaxyXrData["skeletonOffsetZCm"].get<double>();
			}
			if(galaxyXrData["skeletonOffsetMirror"].is_boolean()){
				newConfig.galaxyXr.skeletonOffsetMirror = galaxyXrData["skeletonOffsetMirror"].get<bool>();
			}
		}
		if(data["streamFrame"].is_object()){
			json streamFrameData = data["streamFrame"];
			if(streamFrameData["enable"].is_boolean()){
				newConfig.streamFrame.enable = streamFrameData["enable"].get<bool>();
			}
			if(streamFrameData["saturation"].is_number()){
				newConfig.streamFrame.saturation = streamFrameData["saturation"].get<double>();
			}
			if(streamFrameData["vibrance"].is_number()){
				newConfig.streamFrame.vibrance = streamFrameData["vibrance"].get<double>();
			}
			if(streamFrameData["contrast"].is_number()){
				newConfig.streamFrame.contrast = streamFrameData["contrast"].get<double>();
			}
			if(streamFrameData["contrastMidpoint"].is_number()){
				newConfig.streamFrame.contrastMidpoint = streamFrameData["contrastMidpoint"].get<double>();
			}
			if(streamFrameData["contrastLinear"].is_boolean()){
				newConfig.streamFrame.contrastLinear = streamFrameData["contrastLinear"].get<bool>();
			}
			if(streamFrameData["gamma"].is_number()){
				newConfig.streamFrame.gamma = streamFrameData["gamma"].get<double>();
			}
			if(streamFrameData["colorMultiplier"].is_object()){
				json colorData = streamFrameData["colorMultiplier"];
				if(colorData["r"].is_number()){
					newConfig.streamFrame.colorMultiplier.r = colorData["r"].get<double>();
				}
				if(colorData["g"].is_number()){
					newConfig.streamFrame.colorMultiplier.g = colorData["g"].get<double>();
				}
				if(colorData["b"].is_number()){
					newConfig.streamFrame.colorMultiplier.b = colorData["b"].get<double>();
				}
			}
			if(streamFrameData["srgbMatrix"].is_array()){
				newConfig.streamFrame.srgbMatrix.clear();
				for(auto &value : streamFrameData["srgbMatrix"]){
					if(value.is_number()){
						newConfig.streamFrame.srgbMatrix.push_back(value.get<double>());
					}
				}
			}
			if(streamFrameData["fxaa"].is_boolean()){
				// pre-1.6.6 boolean: true was the in-pass fast path
				newConfig.streamFrame.fxaaMode = streamFrameData["fxaa"].get<bool>() ? 1 : 0;
			}
			if(streamFrameData["fxaa"].is_string()){
				std::string fx = streamFrameData["fxaa"].get<std::string>();
				newConfig.streamFrame.fxaaMode = fx == "quality" ? 2 : (fx == "fast" ? 1 : 0);
			}
			if(streamFrameData["cas"].is_object()){
				json casData = streamFrameData["cas"];
				if(casData["enable"].is_boolean()){
					newConfig.streamFrame.cas.enable = casData["enable"].get<bool>();
				}
				if(casData["strength"].is_number()){
					newConfig.streamFrame.cas.strength = casData["strength"].get<double>();
				}
				if(casData["perEye"].is_boolean()){
					newConfig.streamFrame.cas.perEye = casData["perEye"].get<bool>();
				}
				if(casData["strengthLeft"].is_number()){
					newConfig.streamFrame.cas.strengthLeft = casData["strengthLeft"].get<double>();
				}
				if(casData["strengthRight"].is_number()){
					newConfig.streamFrame.cas.strengthRight = casData["strengthRight"].get<double>();
				}
			}
			if(streamFrameData["dither"].is_boolean()){
				newConfig.streamFrame.dither = streamFrameData["dither"].get<bool>();
			}
			if(streamFrameData["blackFloor"].is_object()){
				json blackFloorData = streamFrameData["blackFloor"];
				if(blackFloorData["rampBar"].is_boolean()){
					newConfig.streamFrame.blackFloor.rampBar = blackFloorData["rampBar"].get<bool>();
				}
				if(blackFloorData["rangeMode"].is_string()){
					std::string rm = blackFloorData["rangeMode"].get<std::string>();
					newConfig.streamFrame.blackFloor.rangeMode = rm == "expand" ? 2 : (rm == "compress" ? 1 : 0);
				}
				if(blackFloorData["shadowLift"].is_boolean()){
					newConfig.streamFrame.blackFloor.shadowLift = blackFloorData["shadowLift"].get<bool>();
				}
				if(blackFloorData["floorCode"].is_number()){
					newConfig.streamFrame.blackFloor.floorCode = blackFloorData["floorCode"].get<double>();
				}
				if(blackFloorData["kneeCode"].is_number()){
					newConfig.streamFrame.blackFloor.kneeCode = blackFloorData["kneeCode"].get<double>();
				}
				if(blackFloorData["blackPointCode"].is_number()){
					newConfig.streamFrame.blackFloor.blackPointCode = blackFloorData["blackPointCode"].get<double>();
				}
			}
			if(streamFrameData["stationaryDimming"].is_object()){
				json dimmingData = streamFrameData["stationaryDimming"];
				if(dimmingData["enable"].is_boolean()){
					newConfig.streamFrame.stationaryDimming.enable = dimmingData["enable"].get<bool>();
				}
				if(dimmingData["movementThreshold"].is_number()){
					newConfig.streamFrame.stationaryDimming.movementThreshold = dimmingData["movementThreshold"].get<double>();
				}
				if(dimmingData["movementTime"].is_number()){
					newConfig.streamFrame.stationaryDimming.movementTime = dimmingData["movementTime"].get<double>();
				}
				if(dimmingData["dimSeconds"].is_number()){
					newConfig.streamFrame.stationaryDimming.dimSeconds = dimmingData["dimSeconds"].get<double>();
				}
				if(dimmingData["brightenSeconds"].is_number()){
					newConfig.streamFrame.stationaryDimming.brightenSeconds = dimmingData["brightenSeconds"].get<double>();
				}
			}
			if(streamFrameData["distortion"].is_object()){
				json distortionData = streamFrameData["distortion"];
				if(distortionData["mode"].is_string()){
					newConfig.streamFrame.distortion.mode = distortionData["mode"].get<std::string>();
				}
				if(distortionData["points"].is_array()){
					newConfig.streamFrame.distortion.points.clear();
					for(auto &pointData : distortionData["points"]){
						if(pointData.is_object() && pointData["r"].is_number() && pointData["scale"].is_number()){
							StreamFrameDistortionPoint point;
							point.r = pointData["r"].get<double>();
							point.scale = pointData["scale"].get<double>();
							newConfig.streamFrame.distortion.points.push_back(point);
						}
					}
				}
				if(distortionData["gain"].is_number()){
					newConfig.streamFrame.distortion.gain = distortionData["gain"].get<double>();
				}
				if(distortionData["centerTune"].is_object()){
					json centerData = distortionData["centerTune"];
					if(centerData["enable"].is_boolean()){
						newConfig.streamFrame.distortion.centerTune.enable = centerData["enable"].get<bool>();
					}
					if(centerData["breatheAmp"].is_number()){
						newConfig.streamFrame.distortion.centerTune.breatheAmp = centerData["breatheAmp"].get<double>();
					}
				}
				if(distortionData["tune"].is_object()){
					json tuneData = distortionData["tune"];
					if(tuneData["enable"].is_boolean()){
						newConfig.streamFrame.distortion.tune.enable = tuneData["enable"].get<bool>();
					}
					if(tuneData["rate"].is_number()){
						newConfig.streamFrame.distortion.tune.rate = tuneData["rate"].get<double>();
					}
					if(tuneData["stepSize"].is_number()){
						newConfig.streamFrame.distortion.tune.stepSize = tuneData["stepSize"].get<double>();
					}
					if(tuneData["ringOpacity"].is_number()){
						newConfig.streamFrame.distortion.tune.ringOpacity = tuneData["ringOpacity"].get<double>();
					}
					if(tuneData["forceGrid"].is_boolean()){
						newConfig.streamFrame.distortion.tune.forceGrid = tuneData["forceGrid"].get<bool>();
					}
					if(tuneData["segments"].is_number_integer()){
						newConfig.streamFrame.distortion.tune.segments = tuneData["segments"].get<int>();
					}
					if(tuneData["segmentLayout"].is_array()){
						newConfig.streamFrame.distortion.tune.segmentLayout.clear();
						for(auto &v : tuneData["segmentLayout"]){
							if(v.is_number()){
								int c = v.get<int>();
								if(c < 1){ c = 1; }
								if(c > 32){ c = 32; }
								newConfig.streamFrame.distortion.tune.segmentLayout.push_back(c);
							}
						}
					}
					if(tuneData["bands"].is_array()){
						std::vector<double> bands;
						for(auto &band : tuneData["bands"]){
							if(band.is_number()){
								bands.push_back(band.get<double>());
							}
						}
						if(!bands.empty()){
							newConfig.streamFrame.distortion.tune.bands = bands;
						}
					}
				}
				if(distortionData["perEye"].is_boolean()){
					newConfig.streamFrame.distortion.perEye = distortionData["perEye"].get<bool>();
				}
				if(distortionData["perAxis"].is_boolean()){
					newConfig.streamFrame.distortion.perAxis = distortionData["perAxis"].get<bool>();
				}
				if(distortionData["segments"].is_number_integer()){
					newConfig.streamFrame.distortion.segments = distortionData["segments"].get<int>();
				}
				if(distortionData["curves"].is_object()){
					newConfig.streamFrame.distortion.curves.clear();
					for(auto &curveItem : distortionData["curves"].items()){
						if(!curveItem.value().is_object()){
							continue;
						}
						StreamFrameCurve curve;
						if(curveItem.value()["k1"].is_number()){
							curve.k1 = curveItem.value()["k1"].get<double>();
						}
						if(curveItem.value()["k2"].is_number()){
							curve.k2 = curveItem.value()["k2"].get<double>();
						}
						if(curveItem.value()["points"].is_array()){
							for(auto &pointData : curveItem.value()["points"]){
								if(pointData.is_object() && pointData["r"].is_number() && pointData["scale"].is_number()){
									StreamFrameDistortionPoint point;
									point.r = pointData["r"].get<double>();
									point.scale = pointData["scale"].get<double>();
									curve.points.push_back(point);
								}
							}
						}
						newConfig.streamFrame.distortion.curves[curveItem.key()] = curve;
					}
				}
				if(distortionData["map"].is_object()){
					json mapData = distortionData["map"];
					StreamFrameDisplacementMap &map = newConfig.streamFrame.distortion.map;
					if(mapData["enable"].is_boolean()){
						map.enable = mapData["enable"].get<bool>();
					}
					if(mapData["cols"].is_number_integer()){
						map.cols = mapData["cols"].get<int>();
					}
					if(mapData["rows"].is_number_integer()){
						map.rows = mapData["rows"].get<int>();
					}
					if(mapData["source"].is_string()){
						map.source = mapData["source"].get<std::string>();
					}
					auto readEye = [&](const char *key, std::vector<double> &out){
						if(!mapData[key].is_array()){
							return;
						}
						out.clear();
						out.reserve(mapData[key].size());
						for(auto &v : mapData[key]){
							if(v.is_number()){
								out.push_back(v.get<double>());
							}else{
								// a non numeric entry corrupts the layout, treat as identity
								out.clear();
								return;
							}
						}
					};
					readEye("left", map.left);
					readEye("right", map.right);
					// clamp to something the bake will accept; the bake also
					// re-validates lengths (cols * rows * 2) per eye
					if(map.cols < 0){ map.cols = 0; }
					if(map.rows < 0){ map.rows = 0; }
					if(map.cols > 257){ map.cols = 0; }
					if(map.rows > 257){ map.rows = 0; }
				}
				if(distortionData["annulus"].is_object()){
					json annulusData = distortionData["annulus"];
					if(annulusData["enable"].is_boolean()){
						newConfig.streamFrame.distortion.annulus.enable = annulusData["enable"].get<bool>();
					}
					if(annulusData["rMin"].is_number()){
						newConfig.streamFrame.distortion.annulus.rMin = annulusData["rMin"].get<double>();
					}
					if(annulusData["rMax"].is_number()){
						newConfig.streamFrame.distortion.annulus.rMax = annulusData["rMax"].get<double>();
					}
					if(annulusData["feather"].is_number()){
						newConfig.streamFrame.distortion.annulus.feather = annulusData["feather"].get<double>();
					}
				}
			}
			if(streamFrameData["k1"].is_number()){
				newConfig.streamFrame.k1 = streamFrameData["k1"].get<double>();
			}
			if(streamFrameData["k2"].is_number()){
				newConfig.streamFrame.k2 = streamFrameData["k2"].get<double>();
			}
			if(streamFrameData["centerOffsetXLeft"].is_number()){
				newConfig.streamFrame.centerOffsetXLeft = streamFrameData["centerOffsetXLeft"].get<double>();
			}
			if(streamFrameData["centerOffsetXRight"].is_number()){
				newConfig.streamFrame.centerOffsetXRight = streamFrameData["centerOffsetXRight"].get<double>();
			}
			if(streamFrameData["centerOffsetY"].is_number()){
				newConfig.streamFrame.centerOffsetY = streamFrameData["centerOffsetY"].get<double>();
			}
			if(streamFrameData["alignment"].is_object()){
				json alignData = streamFrameData["alignment"];
				if(alignData["leftH"].is_number()){
					newConfig.streamFrame.alignment.leftH = alignData["leftH"].get<double>();
				}
				if(alignData["leftV"].is_number()){
					newConfig.streamFrame.alignment.leftV = alignData["leftV"].get<double>();
				}
				if(alignData["rightH"].is_number()){
					newConfig.streamFrame.alignment.rightH = alignData["rightH"].get<double>();
				}
				if(alignData["rightV"].is_number()){
					newConfig.streamFrame.alignment.rightV = alignData["rightV"].get<double>();
				}
			}
			if(streamFrameData["skipColorWhileDashboardOpen"].is_boolean()){
				newConfig.streamFrame.skipColorWhileDashboardOpen = streamFrameData["skipColorWhileDashboardOpen"].get<bool>();
			}
			if(streamFrameData["processAtSubmitLayer"].is_boolean()){
				newConfig.streamFrame.processAtSubmitLayer = streamFrameData["processAtSubmitLayer"].get<bool>();
			}
			if(streamFrameData["brightness"].is_number()){
				newConfig.streamFrame.brightness = streamFrameData["brightness"].get<double>();
			}
			if(streamFrameData["calib"].is_object()){
				json calibData = streamFrameData["calib"];
				StreamFrameCalibConfig &calib = newConfig.streamFrame.calib;
				if(calibData["blackout"].is_boolean()){
					calib.blackout = calibData["blackout"].get<bool>();
				}
				if(calibData["eye"].is_number_integer()){
					calib.eye = calibData["eye"].get<int>();
					if(calib.eye < -1 || calib.eye > 1){ calib.eye = -1; }
				}
				if(calibData["patternBrightness"].is_number()){
					calib.patternBrightness = calibData["patternBrightness"].get<double>();
					if(calib.patternBrightness < 0.0){ calib.patternBrightness = 0.0; }
					if(calib.patternBrightness > 1.0){ calib.patternBrightness = 1.0; }
				}
				if(calibData["captureMode"].is_boolean()){
					calib.captureMode = calibData["captureMode"].get<bool>();
				}
				if(calibData["pattern"].is_number_integer()){
					calib.pattern = calibData["pattern"].get<int>();
				}
				if(calibData["patternBits"].is_number_integer()){
					calib.patternBits = calibData["patternBits"].get<int>();
					if(calib.patternBits < 1){ calib.patternBits = 1; }
					if(calib.patternBits > 12){ calib.patternBits = 12; }
				}
			}
			if(streamFrameData["poseLogging"].is_boolean()){
				newConfig.streamFrame.poseLogging = streamFrameData["poseLogging"].get<bool>();
			}
			if(streamFrameData["poseLogBurst"].is_boolean()){
				newConfig.streamFrame.poseLogBurst = streamFrameData["poseLogBurst"].get<bool>();
			}
			// RETIRED 1.6.7 (prune-on-save incident 2026-08-11): the legacy
			// "velocityFix" bool no longer selects a mode. The GUI's
			// default-diff serializer prunes an explicit velocityFixMode the
			// moment it equals the published default, after which this
			// round-trip preserved fossil used to take over and select the
			// rejected FULL blend. The key is inert; the GUI migration
			// deletes it from the file. Mode selection: the
			// "velocityFixMode" string only (classic/full/derive stay
			// reachable by string per the graveyard law).
			if(streamFrameData["zeroCopyV3"].is_boolean()){
				newConfig.streamFrame.zeroCopyV3 = streamFrameData["zeroCopyV3"].get<bool>();
			}
			if(streamFrameData["nvencTap"].is_boolean()){
				newConfig.streamFrame.nvencTap = streamFrameData["nvencTap"].get<bool>();
			}
			if(streamFrameData["velocityFixMode"].is_string()){
				std::string mode = streamFrameData["velocityFixMode"].get<std::string>();
				newConfig.streamFrame.velocityFixMode = mode == "kalmanCA" ? 6 : (mode == "kalmanCAM" ? 5 : (mode == "kalman" ? 4 : (mode == "derive" ? 3 : (mode == "full" ? 2 : (mode == "classic" ? 1 : 0)))));
			}
			// mode provenance (2x incident 2026-08-11): a round-trip
			// preserved legacy "velocityFix" bool with no
			// "velocityFixMode" string silently downgraded kalman to the
			// retired FULL blend and cost a field session. resolve loudly
			// so the very first grep answers "which estimator ran".
			{
				bool legacyBool = streamFrameData["velocityFix"].is_boolean();
				bool modeString = streamFrameData["velocityFixMode"].is_string();
				DriverLog("Config: velocityFixMode=%d source=%s",
					newConfig.streamFrame.velocityFixMode,
					modeString ? "string" : "default");
				if(legacyBool){
					DriverLog("Config: legacy \"velocityFix\" key present and IGNORED (retired 1.6.7); the GUI removes it on its next save. Mode selection uses \"velocityFixMode\" only.");
				}
			}
			if(streamFrameData["deriveSmoothTauSlowMs"].is_number()){
				newConfig.streamFrame.deriveSmoothTauSlowMs = streamFrameData["deriveSmoothTauSlowMs"].get<double>();
			}
			if(streamFrameData["deriveSmoothTauFastMs"].is_number()){
				newConfig.streamFrame.deriveSmoothTauFastMs = streamFrameData["deriveSmoothTauFastMs"].get<double>();
			}
			if(streamFrameData["deriveSmoothSpeedLow"].is_number()){
				newConfig.streamFrame.deriveSmoothSpeedLow = streamFrameData["deriveSmoothSpeedLow"].get<double>();
			}
			if(streamFrameData["deriveSmoothSpeedHigh"].is_number()){
				newConfig.streamFrame.deriveSmoothSpeedHigh = streamFrameData["deriveSmoothSpeedHigh"].get<double>();
			}
			if(streamFrameData["deriveSmoothAngSeparate"].is_boolean()){
				newConfig.streamFrame.deriveSmoothAngSeparate = streamFrameData["deriveSmoothAngSeparate"].get<bool>();
			}
			if(streamFrameData["deriveSmoothAngTauSlowMs"].is_number()){
				newConfig.streamFrame.deriveSmoothAngTauSlowMs = streamFrameData["deriveSmoothAngTauSlowMs"].get<double>();
			}
			if(streamFrameData["deriveSmoothAngTauFastMs"].is_number()){
				newConfig.streamFrame.deriveSmoothAngTauFastMs = streamFrameData["deriveSmoothAngTauFastMs"].get<double>();
			}
			if(streamFrameData["deriveSmoothAngSpeedLow"].is_number()){
				newConfig.streamFrame.deriveSmoothAngSpeedLow = streamFrameData["deriveSmoothAngSpeedLow"].get<double>();
			}
			if(streamFrameData["deriveSmoothAngSpeedHigh"].is_number()){
				newConfig.streamFrame.deriveSmoothAngSpeedHigh = streamFrameData["deriveSmoothAngSpeedHigh"].get<double>();
			}
			if(streamFrameData["deriveSplitDirLinear"].is_boolean()){
				newConfig.streamFrame.deriveSplitDirLinear = streamFrameData["deriveSplitDirLinear"].get<bool>();
			}
			if(streamFrameData["deriveSplitDirAngular"].is_boolean()){
				newConfig.streamFrame.deriveSplitDirAngular = streamFrameData["deriveSplitDirAngular"].get<bool>();
			}
			if(streamFrameData["deriveDirWindowMs"].is_number()){
				newConfig.streamFrame.deriveDirWindowMs = streamFrameData["deriveDirWindowMs"].get<double>();
			}
			if(streamFrameData["deriveDirWeightPow"].is_number()){
				newConfig.streamFrame.deriveDirWeightPow = streamFrameData["deriveDirWeightPow"].get<double>();
			}
			if(streamFrameData["deriveDirSource"].is_string()){
				std::string dirSrc = streamFrameData["deriveDirSource"].get<std::string>();
				newConfig.streamFrame.deriveDirSource = dirSrc == "runtime" ? 2 : (dirSrc == "window" ? 0 : 1);
			}
			if(streamFrameData["deriveMagSource"].is_string()){
				newConfig.streamFrame.deriveMagSource = streamFrameData["deriveMagSource"].get<std::string>() == "scalar" ? 1 : 0;
			}
			if(streamFrameData["deriveReleaseLatch"].is_boolean()){
				newConfig.streamFrame.deriveReleaseLatch = streamFrameData["deriveReleaseLatch"].get<bool>();
			}
			if(streamFrameData["deriveLatchWindowMs"].is_number()){
				newConfig.streamFrame.deriveLatchWindowMs = streamFrameData["deriveLatchWindowMs"].get<double>();
			}
			if(streamFrameData["deriveLatchHoldMs"].is_number()){
				newConfig.streamFrame.deriveLatchHoldMs = streamFrameData["deriveLatchHoldMs"].get<double>();
			}
			if(streamFrameData["deriveLatchMinSpeed"].is_number()){
				newConfig.streamFrame.deriveLatchMinSpeed = streamFrameData["deriveLatchMinSpeed"].get<double>();
			}
			if(streamFrameData["deriveLatchAngMinSpeed"].is_number()){
				newConfig.streamFrame.deriveLatchAngMinSpeed = streamFrameData["deriveLatchAngMinSpeed"].get<double>();
			}
			if(streamFrameData["derivePreFilter"].is_string()){
				newConfig.streamFrame.derivePreFilter = streamFrameData["derivePreFilter"].get<std::string>() == "median3" ? 1 : 0;
			}
			if(streamFrameData["derivePreSmoothMs"].is_number()){
				newConfig.streamFrame.derivePreSmoothMs = streamFrameData["derivePreSmoothMs"].get<double>();
			}
			if(streamFrameData["derivePreSmoothScope"].is_string()){
				newConfig.streamFrame.derivePreSmoothScope = streamFrameData["derivePreSmoothScope"].get<std::string>() == "both" ? 1 : 0;
			}
			if(streamFrameData["deriveDiagVelocity"].is_string()){
				newConfig.streamFrame.deriveDiagVelocity = streamFrameData["deriveDiagVelocity"].get<std::string>() == "zero" ? 1 : 0;
			}
			if(streamFrameData["deriveLatchPoseAssist"].is_boolean()){
				newConfig.streamFrame.deriveLatchPoseAssist = streamFrameData["deriveLatchPoseAssist"].get<bool>();
			}
			if(streamFrameData["kalmanProcessAccel"].is_number()){
				newConfig.streamFrame.kalmanProcessAccel = streamFrameData["kalmanProcessAccel"].get<double>();
			}
			if(streamFrameData["kalmanPosNoiseMm"].is_number()){
				newConfig.streamFrame.kalmanPosNoiseMm = streamFrameData["kalmanPosNoiseMm"].get<double>();
			}
			if(streamFrameData["kalmanProcessAngAccel"].is_number()){
				newConfig.streamFrame.kalmanProcessAngAccel = streamFrameData["kalmanProcessAngAccel"].get<double>();
			}
			if(streamFrameData["kalmanOriNoiseDeg"].is_number()){
				newConfig.streamFrame.kalmanOriNoiseDeg = streamFrameData["kalmanOriNoiseDeg"].get<double>();
			}
			if(streamFrameData["kalmanLeadMs"].is_number()){
				newConfig.streamFrame.kalmanLeadMs = streamFrameData["kalmanLeadMs"].get<double>();
			}
			if(streamFrameData["kalmanReleaseRewindMs"].is_number()){
				newConfig.streamFrame.kalmanReleaseRewindMs = streamFrameData["kalmanReleaseRewindMs"].get<double>();
			}
			if(streamFrameData["kalmanRewindHoldMs"].is_number()){
				newConfig.streamFrame.kalmanRewindHoldMs = streamFrameData["kalmanRewindHoldMs"].get<double>();
			}
			if(streamFrameData["kalmanDirSmoothMs"].is_number()){
				newConfig.streamFrame.kalmanDirSmoothMs = streamFrameData["kalmanDirSmoothMs"].get<double>();
			}
			if(streamFrameData["kalmanAngDirSmoothMs"].is_number()){
				newConfig.streamFrame.kalmanAngDirSmoothMs = streamFrameData["kalmanAngDirSmoothMs"].get<double>();
			}
			if(streamFrameData["kalmanMagSource"].is_string()){
				newConfig.streamFrame.kalmanMagSource = streamFrameData["kalmanMagSource"].get<std::string>() == "fast" ? 1 : 0;
			}
			if(streamFrameData["kalmanMagAccel"].is_number()){
				newConfig.streamFrame.kalmanMagAccel = streamFrameData["kalmanMagAccel"].get<double>();
			}
			if(streamFrameData["kalmanMagScale"].is_number()){
				newConfig.streamFrame.kalmanMagScale = streamFrameData["kalmanMagScale"].get<double>();
			}
			if(streamFrameData["kalmanAngMagScale"].is_number()){
				newConfig.streamFrame.kalmanAngMagScale = streamFrameData["kalmanAngMagScale"].get<double>();
			}
			if(streamFrameData["kalmanDupSkip"].is_boolean()){
				// legacy bool from older settings files: true = coast
				newConfig.streamFrame.kalmanDupMode = streamFrameData["kalmanDupSkip"].get<bool>() ? 1 : 0;
			}
			if(streamFrameData["kalmanDupMode"].is_string()){
				std::string dupModeStr = streamFrameData["kalmanDupMode"].get<std::string>();
				newConfig.streamFrame.kalmanDupMode = dupModeStr == "age" ? 4 : (dupModeStr == "soft" ? 3 : (dupModeStr == "drop" ? 2 : (dupModeStr == "coast" ? 1 : 0)));
			}
			if(streamFrameData["kalmanDupRScale"].is_number()){
				newConfig.streamFrame.kalmanDupRScale = streamFrameData["kalmanDupRScale"].get<double>();
			}
			if(streamFrameData["kalmanTeleportM"].is_number()){
				newConfig.streamFrame.kalmanTeleportM = streamFrameData["kalmanTeleportM"].get<double>();
			}
			if(streamFrameData["kalmanLossCoastMs"].is_number()){
				newConfig.streamFrame.kalmanLossCoastMs = streamFrameData["kalmanLossCoastMs"].get<double>();
			}
			if(streamFrameData["streamFrameSchema"].is_number()){
				newConfig.streamFrame.streamFrameSchema = streamFrameData["streamFrameSchema"].get<int>();
			}
			if(streamFrameData["graveyardEnable"].is_boolean()){
				newConfig.streamFrame.graveyardEnable = streamFrameData["graveyardEnable"].get<bool>();
			}
			if(streamFrameData["kalmanDeviceTime"].is_boolean()){
				newConfig.streamFrame.kalmanDeviceTime = streamFrameData["kalmanDeviceTime"].get<bool>();
			}
			if(streamFrameData["kalmanPosFreeze3dof"].is_boolean()){
				newConfig.streamFrame.kalmanPosFreeze3dof = streamFrameData["kalmanPosFreeze3dof"].get<bool>();
			}
			if(streamFrameData["kalmanAngularOutFrame"].is_number()){
				newConfig.streamFrame.kalmanAngularOutFrame = streamFrameData["kalmanAngularOutFrame"].get<int>();
			}else if(streamFrameData["kalmanAngularOutFrame"].is_string()){
				std::string f = streamFrameData["kalmanAngularOutFrame"].get<std::string>();
				newConfig.streamFrame.kalmanAngularOutFrame = f == "world" ? 0 : (f == "zero" ? 2 : 1);
			}
			if(streamFrameData["kalmanFreezeCoastTurn"].is_number()){
				newConfig.streamFrame.kalmanFreezeCoastTurn = streamFrameData["kalmanFreezeCoastTurn"].get<double>();
			}
			if(streamFrameData["kalmanPosFreezeVelDecayMs"].is_number()){
				newConfig.streamFrame.kalmanPosFreezeVelDecayMs = streamFrameData["kalmanPosFreezeVelDecayMs"].get<double>();
			}
			if(streamFrameData["kalmanDupCoastMaxMs"].is_number()){
				newConfig.streamFrame.kalmanDupCoastMaxMs = streamFrameData["kalmanDupCoastMaxMs"].get<double>();
			}
			if(streamFrameData["kalmanGazeAssist"].is_number()){
				newConfig.streamFrame.kalmanGazeAssist = streamFrameData["kalmanGazeAssist"].get<double>();
			}
			if(streamFrameData["kalmanGazeMaxDeg"].is_number()){
				newConfig.streamFrame.kalmanGazeMaxDeg = streamFrameData["kalmanGazeMaxDeg"].get<double>();
			}
			if(streamFrameData["kalmanGazeMinSpeed"].is_number()){
				newConfig.streamFrame.kalmanGazeMinSpeed = streamFrameData["kalmanGazeMinSpeed"].get<double>();
			}
			if(streamFrameData["kalmanSmoothLagMs"].is_number()){
				newConfig.streamFrame.kalmanSmoothLagMs = streamFrameData["kalmanSmoothLagMs"].get<double>();
			}
			if(streamFrameData["kalmanSmoothLagEpoch"].is_number()){
				newConfig.streamFrame.kalmanSmoothLagEpoch = streamFrameData["kalmanSmoothLagEpoch"].get<int>();
			}
			if(streamFrameData["kalmanDirLeadMs"].is_number()){
				newConfig.streamFrame.kalmanDirLeadMs = streamFrameData["kalmanDirLeadMs"].get<double>();
			}
			if(streamFrameData["kalmanDirLeadAdaptive"].is_boolean()){
				newConfig.streamFrame.kalmanDirLeadAdaptive = streamFrameData["kalmanDirLeadAdaptive"].get<bool>();
			}
			if(streamFrameData["kalmanDirLeadBaseMs"].is_number()){
				newConfig.streamFrame.kalmanDirLeadBaseMs = streamFrameData["kalmanDirLeadBaseMs"].get<double>();
			}
			if(streamFrameData["kalmanDirLeadWMs"].is_number()){
				newConfig.streamFrame.kalmanDirLeadWMs = streamFrameData["kalmanDirLeadWMs"].get<double>();
			}
			if(streamFrameData["kalmanAdaptiveR"].is_boolean()){
				newConfig.streamFrame.kalmanAdaptiveR = streamFrameData["kalmanAdaptiveR"].get<bool>();
			}
			if(streamFrameData["kalmanAdaptiveRMaxDiv"].is_number()){
				newConfig.streamFrame.kalmanAdaptiveRMaxDiv = streamFrameData["kalmanAdaptiveRMaxDiv"].get<double>();
			}
			if(streamFrameData["kalmanCaJerk"].is_number()){
				newConfig.streamFrame.kalmanCaJerk = streamFrameData["kalmanCaJerk"].get<double>();
			}
			if(streamFrameData["kalmanCaAngJerk"].is_number()){
				newConfig.streamFrame.kalmanCaAngJerk = streamFrameData["kalmanCaAngJerk"].get<double>();
			}
			if(streamFrameData["kalmanCaPosNoiseMm"].is_number()){
				newConfig.streamFrame.kalmanCaPosNoiseMm = streamFrameData["kalmanCaPosNoiseMm"].get<double>();
			}
			if(streamFrameData["kalmanCaOriNoiseDeg"].is_number()){
				newConfig.streamFrame.kalmanCaOriNoiseDeg = streamFrameData["kalmanCaOriNoiseDeg"].get<double>();
			}
			if(streamFrameData["kalmanCaAccelTauMs"].is_number()){
				newConfig.streamFrame.kalmanCaAccelTauMs = streamFrameData["kalmanCaAccelTauMs"].get<double>();
			}
			if(streamFrameData["kalmanCaMagJerk"].is_number()){
				newConfig.streamFrame.kalmanCaMagJerk = streamFrameData["kalmanCaMagJerk"].get<double>();
			}
			if(streamFrameData["kalmanCaMagAccelTauMs"].is_number()){
				newConfig.streamFrame.kalmanCaMagAccelTauMs = streamFrameData["kalmanCaMagAccelTauMs"].get<double>();
			}
			if(streamFrameData["kalmanCaReportAccel"].is_boolean()){
				newConfig.streamFrame.kalmanCaReportAccel = streamFrameData["kalmanCaReportAccel"].get<bool>();
			}
			if(streamFrameData["kalmanCaExactCov"].is_boolean()){
				newConfig.streamFrame.kalmanCaExactCov = streamFrameData["kalmanCaExactCov"].get<bool>();
			}
			if(streamFrameData["kalmanGripEnable"].is_boolean()){
				newConfig.streamFrame.kalmanGripEnable = streamFrameData["kalmanGripEnable"].get<bool>();
			}
			if(streamFrameData["kalmanGripBlend"].is_number()){
				newConfig.streamFrame.kalmanGripBlend = streamFrameData["kalmanGripBlend"].get<double>();
			}
			{
				const char* gripAxes[3] = {"x", "y", "z"};
				if(streamFrameData["kalmanGripLeftCm"].is_object()){
					for(int i = 0; i < 3; i++){
						if(streamFrameData["kalmanGripLeftCm"][gripAxes[i]].is_number()){
							newConfig.streamFrame.kalmanGripLeftCm[i] = streamFrameData["kalmanGripLeftCm"][gripAxes[i]].get<double>();
						}
					}
				}
				if(streamFrameData["kalmanGripRightCm"].is_object()){
					for(int i = 0; i < 3; i++){
						if(streamFrameData["kalmanGripRightCm"][gripAxes[i]].is_number()){
							newConfig.streamFrame.kalmanGripRightCm[i] = streamFrameData["kalmanGripRightCm"][gripAxes[i]].get<double>();
						}
					}
				}
			}
			if(streamFrameData["eyeGaze"].is_object()){
				json eyeGazeData = streamFrameData["eyeGaze"];
				if(eyeGazeData["debugRing"].is_boolean()){
					newConfig.streamFrame.eyeGaze.debugRing = eyeGazeData["debugRing"].get<bool>();
				}
				if(eyeGazeData["tanHalfFovX"].is_number()){
					newConfig.streamFrame.eyeGaze.tanHalfFovX = eyeGazeData["tanHalfFovX"].get<double>();
				}
				if(eyeGazeData["tanHalfFovY"].is_number()){
					newConfig.streamFrame.eyeGaze.tanHalfFovY = eyeGazeData["tanHalfFovY"].get<double>();
				}
				if(eyeGazeData["predictionMs"].is_number()){
					newConfig.streamFrame.eyeGaze.predictionMs = eyeGazeData["predictionMs"].get<double>();
				}
				if(eyeGazeData["debugGrid"].is_boolean()){
					newConfig.streamFrame.eyeGaze.debugGrid = eyeGazeData["debugGrid"].get<bool>();
				}
				if(eyeGazeData["gridMode"].is_string()){
					newConfig.streamFrame.eyeGaze.gridMode = eyeGazeData["gridMode"].get<std::string>();
				}
				if(eyeGazeData["gridAngularDeg"].is_number()){
					newConfig.streamFrame.eyeGaze.gridAngularDeg = eyeGazeData["gridAngularDeg"].get<double>();
				}
				if(eyeGazeData["calibDot"].is_boolean()){
					newConfig.streamFrame.eyeGaze.calibDot = eyeGazeData["calibDot"].get<bool>();
				}
				if(eyeGazeData["probeCapture"].is_boolean()){
					newConfig.streamFrame.eyeGaze.probeCapture = eyeGazeData["probeCapture"].get<bool>();
				}
				if(eyeGazeData["gridWorldLocked"].is_boolean()){
					newConfig.streamFrame.eyeGaze.gridWorldLocked = eyeGazeData["gridWorldLocked"].get<bool>();
				}
				if(eyeGazeData["gridOpaque"].is_boolean()){
					newConfig.streamFrame.eyeGaze.gridOpaque = eyeGazeData["gridOpaque"].get<bool>();
				}
				if(eyeGazeData["swimProbe"].is_boolean()){
					newConfig.streamFrame.eyeGaze.swimProbe = eyeGazeData["swimProbe"].get<bool>();
				}
				if(eyeGazeData["overlayWarped"].is_boolean()){
					newConfig.streamFrame.eyeGaze.overlayWarped = eyeGazeData["overlayWarped"].get<bool>();
				}
			}
			if(streamFrameData["pupilSwim"].is_object()){
				json pupilSwimData = streamFrameData["pupilSwim"];
				if(pupilSwimData["centerStrengthX"].is_number()){
					newConfig.streamFrame.pupilSwim.centerStrengthX = pupilSwimData["centerStrengthX"].get<double>();
				}
				if(pupilSwimData["centerStrengthY"].is_number()){
					newConfig.streamFrame.pupilSwim.centerStrengthY = pupilSwimData["centerStrengthY"].get<double>();
				}
			}
			if(streamFrameData["syncTimeoutMs"].is_number()){
				newConfig.streamFrame.syncTimeoutMs = streamFrameData["syncTimeoutMs"].get<int>();
			}
			if(streamFrameData["reconLogger"].is_boolean()){
				newConfig.streamFrame.reconLogger = streamFrameData["reconLogger"].get<bool>();
			}
			if(streamFrameData["hitchDiag"].is_boolean()){
				newConfig.streamFrame.hitchDiag = streamFrameData["hitchDiag"].get<bool>();
			}
			if(streamFrameData["deferredEviction"].is_boolean()){
				newConfig.streamFrame.deferredEviction = streamFrameData["deferredEviction"].get<bool>();
			}
			if(streamFrameData["directRender"].is_boolean()){
				newConfig.streamFrame.directRender = streamFrameData["directRender"].get<bool>();
			}
			if(streamFrameData["zeroCopy"].is_boolean()){
				newConfig.streamFrame.zeroCopy = streamFrameData["zeroCopy"].get<bool>();
			}
		}
		if(data["controllers"].is_object()){
			json controllersData = data["controllers"];
			const char* axes[3] = {"x", "y", "z"};
			if(controllersData["spaceVelocityFix"].is_string()){
				std::string svMode = controllersData["spaceVelocityFix"].get<std::string>();
				newConfig.controllers.spaceVelocityFixMode = svMode == "world" ? 1 : (svMode == "driver" ? 2 : 0);
			}
			if(controllersData["mirrorOffsetsForRightHand"].is_boolean()){
				newConfig.controllers.mirrorOffsetsForRightHand = controllersData["mirrorOffsetsForRightHand"].get<bool>();
			}
			if(controllersData["rotationOffsetDeg"].is_object()){
				for(int i = 0; i < 3; i++){
					if(controllersData["rotationOffsetDeg"][axes[i]].is_number()){
						newConfig.controllers.rotationOffsetDeg[i] = controllersData["rotationOffsetDeg"][axes[i]].get<double>();
					}
				}
			}
			if(controllersData["aligner"].is_object() && controllersData["aligner"]["enable"].is_boolean()){
				newConfig.controllers.aligner.enable = controllersData["aligner"]["enable"].get<bool>();
			}
			if(controllersData["positionOffsetCm"].is_object()){
				for(int i = 0; i < 3; i++){
					if(controllersData["positionOffsetCm"][axes[i]].is_number()){
						newConfig.controllers.positionOffsetCm[i] = controllersData["positionOffsetCm"][axes[i]].get<double>();
					}
				}
			}
			// per-hand unmirrored trims: {"left": {"rotationOffsetDeg": {..}, "positionOffsetCm": {..}}, "right": {..}}
			{
				struct HandSlot{ const char* key; double* rot; double* pos; };
				HandSlot slots[2] = {
					{"left", newConfig.controllers.leftRotationOffsetDeg, newConfig.controllers.leftPositionOffsetCm},
					{"right", newConfig.controllers.rightRotationOffsetDeg, newConfig.controllers.rightPositionOffsetCm},
				};
				for(HandSlot &slot : slots){
					if(!controllersData[slot.key].is_object()){ continue; }
					json handData = controllersData[slot.key];
					for(int i = 0; i < 3; i++){
						if(handData["rotationOffsetDeg"].is_object() && handData["rotationOffsetDeg"][axes[i]].is_number()){
							slot.rot[i] = handData["rotationOffsetDeg"][axes[i]].get<double>();
						}
						if(handData["positionOffsetCm"].is_object() && handData["positionOffsetCm"][axes[i]].is_number()){
							slot.pos[i] = handData["positionOffsetCm"][axes[i]].get<double>();
						}
					}
				}
			}
		}
		if(data["forceTracking"].is_boolean()){
			newConfig.forceTracking = data["forceTracking"].get<bool>();
		}
		if(data["takeCompositorScreenshots"].is_boolean()){
			newConfig.takeCompositorScreenshots = data["takeCompositorScreenshots"].get<bool>();
		}
		if(data["onlyHandlePrivateFunctionality"].is_boolean()){
			newConfig.onlyHandlePrivateFunctionality = data["onlyHandlePrivateFunctionality"].get<bool>();
		}
		// if(data["watchDistortionProfiles"].is_boolean()){
		// 	newConfig.watchDistortionProfiles = data["watchDistortionProfiles"].get<bool>();
		// }
		// version-gated migration to schema 2 (2026-08-15 release):
		// upgrade ONLY configs still on the exact old defaults — explicit
		// CV mode with untouched CV knobs, or a CA mode with the old
		// pre-ratification CA tuning — to the ratified CA-Full defaults.
		// any custom tuning or non-default mode choice is respected
		// untouched. idempotent: runs in-memory every load until the GUI
		// persists streamFrameSchema=2; post-migration states no longer
		// match the old-default patterns, so re-running is a no-op.
		if(newConfig.streamFrame.streamFrameSchema < 2){
			auto &sf = newConfig.streamFrame;
			bool cvDefaults = sf.kalmanProcessAccel == 1.0
				&& sf.kalmanPosNoiseMm == 2.7
				&& sf.kalmanProcessAngAccel == 400.0
				&& sf.kalmanOriNoiseDeg == 1.25;
			bool caOldDefaults = sf.kalmanCaJerk == 10.0
				&& sf.kalmanCaAngJerk == 1500.0
				&& sf.kalmanCaPosNoiseMm == 4.2
				&& sf.kalmanCaOriNoiseDeg == 1.25;
			if(sf.velocityFixMode == 4 && cvDefaults){
				sf.velocityFixMode = 6;
				DriverLog("Config: schema migration - default-tuned Kalman CV upgraded to Kalman CA (ratified defaults)");
			}else if((sf.velocityFixMode == 5 || sf.velocityFixMode == 6) && caOldDefaults){
				sf.velocityFixMode = 6;
				sf.kalmanCaJerk = 17.0;
				sf.kalmanCaPosNoiseMm = 5.7;
				sf.kalmanCaOriNoiseDeg = 5.75;
				DriverLog("Config: schema migration - old CA default tuning upgraded to ratified J=17 P=5.7 O=5.75");
			}
			sf.streamFrameSchema = 2;
		}
		// schema 3 (2026-08-16, composition-fix session): upgrade exact
		// schema-2 ratified CA configs to the new ratified defaults
		// (J=4 P=1.5 O=1.5 tau=20 excov=1). same contract as schema 2:
		// only untouched ratified tunings migrate, anything custom passes
		// through; idempotent until the GUI persists the schema number.
		if(newConfig.streamFrame.streamFrameSchema < 3){
			auto &sf = newConfig.streamFrame;
			bool caSchema2Defaults = sf.kalmanCaJerk == 17.0
				&& sf.kalmanCaAngJerk == 1500.0
				&& sf.kalmanCaPosNoiseMm == 5.7
				&& sf.kalmanCaOriNoiseDeg == 5.75
				&& sf.kalmanCaAccelTauMs == 150.0
				&& sf.kalmanCaExactCov == false;
			if(sf.velocityFixMode == 6 && caSchema2Defaults){
				sf.kalmanCaJerk = 4.0;
				sf.kalmanCaPosNoiseMm = 1.5;
				sf.kalmanCaOriNoiseDeg = 1.5;
				sf.kalmanCaAccelTauMs = 20.0;
				sf.kalmanCaExactCov = true;
				DriverLog("Config: schema migration - schema-2 CA defaults upgraded to ratified J=4 P=1.5 O=1.5 tau=20 excov=1");
			}
			sf.streamFrameSchema = 3;
		}
		// write to global config
		{
			std::lock_guard<std::mutex> lock(driverConfigLock);
			driverConfigOld = driverConfig;
			driverConfig = newConfig;
		}
	}catch(const std::exception& e){
		DriverLog("Failed to parse config file: %s", e.what());
		return;
	}
}

DistortionProfileConfig ConfigLoader::ParseDistortionConfig(std::string name){
	std::string profilePath = GetConfigFolder() + "Distortion/" + name + ".json";
	std::ifstream configFile(profilePath);
	if(!configFile.is_open()){
		DriverLog("Distortion profile not found at %s", profilePath.c_str());
		return {};
	}
	DriverLog("Loading distortion profile from %s", profilePath.c_str());
	try{
		// parse with support for comments
		json data = json::parse(configFile, nullptr, true, true);
		DistortionProfileConfig profile = {};
		profile.modifiedTime = std::chrono::duration_cast<std::chrono::nanoseconds>(std::filesystem::last_write_time(profilePath).time_since_epoch()).count() / 1000000000.0;
		profile.name = name;
		if(data["description"].is_string()){
			profile.description = data["description"].get<std::string>();
		}
		if(data["type"].is_string()){
			profile.type = data["type"].get<std::string>();
		}
		if(data["distortions"].is_array()){
			profile.distortions = data["distortions"].get<std::vector<double>>();
		}
		if(data["distortionsRed"].is_array()){
			profile.distortionsRed = data["distortionsRed"].get<std::vector<double>>();
		}
		if(data["distortionsBlue"].is_array()){
			profile.distortionsBlue = data["distortionsBlue"].get<std::vector<double>>();
		}
		if(data["offsetX"].is_number()){
			profile.offsetX = data["offsetX"].get<double>();
		}
		if(data["offsetY"].is_number()){
			profile.offsetY = data["offsetY"].get<double>();
		}
		if(data["legacySmoothing"].is_boolean()){
			profile.legacySmoothing = data["legacySmoothing"].get<bool>();
		}
		if(data["smoothAmount"].is_number()){
			profile.smoothAmount = data["smoothAmount"].get<double>();
		}
		return profile;
	}catch(const std::exception& e){
		DriverLog("Failed to parse distortion profile: %s", e.what());
		return {};
	}
}

ordered_json baseHeadsetInfo(const Config::BaseHeadsetConfig& headsetConfig){
	return {
		{"enable", headsetConfig.enable},
		{"ipd", headsetConfig.ipd},
		{"ipdOffset", headsetConfig.ipdOffset},
		{"horizontalIPDOffset", headsetConfig.horizontalIPDOffset},
		{"blackLevel", headsetConfig.blackLevel},
		{"colorMultiplier", {
			{"r", headsetConfig.colorMultiplier.r},
			{"g", headsetConfig.colorMultiplier.g},
			{"b", headsetConfig.colorMultiplier.b},
		}},
		{"distortionProfile", headsetConfig.distortionProfile},
		{"distortionZoom", headsetConfig.distortionZoom},
		{"fovZoom", headsetConfig.fovZoom},
		{"flatFovZoom", headsetConfig.flatFovZoom},
		{"subpixelShift", headsetConfig.subpixelShift},
		{"subpixelOffsets", headsetConfig.subpixelOffsets},
		{"resolutionX", headsetConfig.resolutionX},
		{"resolutionY", headsetConfig.resolutionY},
		{"displayRotation", headsetConfig.displayRotation},
		{"maxFovX", headsetConfig.maxFovX},
		{"maxFovY", headsetConfig.maxFovY},
		{"distortionMeshResolution", headsetConfig.distortionMeshResolution},
		{"fovBurnInPrevention", headsetConfig.fovBurnInPrevention},
		{"fovClamping", headsetConfig.fovClamping},
		{"distortionProfileDeviceType", headsetConfig.distortionProfileDeviceType},
		{"renderResolutionMultiplierX", headsetConfig.renderResolutionMultiplierX},
		{"renderResolutionMultiplierY", headsetConfig.renderResolutionMultiplierY},
		{"superSamplingFilterPercent", headsetConfig.superSamplingFilterPercent},
		{"secondsFromVsyncToPhotons", headsetConfig.secondsFromVsyncToPhotons},
		{"secondsFromPhotonsToVblank", headsetConfig.secondsFromPhotonsToVblank},
		{"eyeRotation", headsetConfig.eyeRotation},
		{"disableEye", headsetConfig.disableEye},
		{"disableEyeDecreaseFov", headsetConfig.disableEyeDecreaseFov},
		{"useViveBluetooth", headsetConfig.useViveBluetooth},
		{"directMode", headsetConfig.directMode},
		{"replaceIcons", headsetConfig.replaceIcons},
		{"edidVendorIdOverride", headsetConfig.edidVendorIdOverride},
		{"edidProductIdOverride", headsetConfig.edidProductIdOverride},
		{"dscVersion", headsetConfig.dscVersion},
		{"dscSliceCount", headsetConfig.dscSliceCount},
		{"dscBPPx16", headsetConfig.dscBPPx16},
		{"forceEnable", headsetConfig.forceEnable},
		{"parallelProjection", headsetConfig.parallelProjection},
		{"enableEyeTracking", headsetConfig.enableEyeTracking},
		{"hiddenArea", {
			{"enable", headsetConfig.hiddenArea.enable},
			{"testMode", headsetConfig.hiddenArea.testMode},
			{"detailLevel", headsetConfig.hiddenArea.detailLevel},
			{"radiusTopOuter", headsetConfig.hiddenArea.radiusTopOuter},
			{"radiusTopInner", headsetConfig.hiddenArea.radiusTopInner},
			{"radiusBottomInner", headsetConfig.hiddenArea.radiusBottomInner},
			{"radiusBottomOuter", headsetConfig.hiddenArea.radiusBottomOuter},
		}},
		{"stationaryDimming", {
			{"enable", headsetConfig.stationaryDimming.enable},
			{"movementThreshold", headsetConfig.stationaryDimming.movementThreshold},
			{"movementTime", headsetConfig.stationaryDimming.movementTime},
			{"dimBrightnessPercent", headsetConfig.stationaryDimming.dimBrightnessPercent},
			{"dimSeconds", headsetConfig.stationaryDimming.dimSeconds},
			{"brightenSeconds", headsetConfig.stationaryDimming.brightenSeconds},
		}},
	};
}
void ConfigLoader::WriteInfo(){
	std::lock_guard<std::mutex> lock(infoWriteLock);
	info.needToWrite = false;
	std::string infoPath = GetConfigFolder() + "info.json";
	std::ofstream infoFile(infoPath);
	if(!infoFile.is_open()){
		#ifdef _WIN32
		DriverLog("Failed to open info.json for writing: %d", GetLastError());
		#else
		DriverLog("Failed to open info.json for writing");
		#endif
		return;
	}
	Config defaultSettings = {};
	std::map<std::string,int> emptyObject = {};
	ordered_json data = {
		{"about", "This file is not for configuration. It provides info from the driver for other utilities to use."},
		{"defaultSettings", {
			{"meganeX8K", baseHeadsetInfo(defaultSettings.meganeX8K)},
			{"dreamAir", baseHeadsetInfo(defaultSettings.dreamAir)},
			{"generalHeadset", {
				{"useViveBluetooth", defaultSettings.generalHeadset.useViveBluetooth},
			}},
			{"customShader", {
				{"enable", defaultSettings.customShader.enable},
				{"enableForMeganeX8K", defaultSettings.customShader.enableForMeganeX8K},
				{"enableForDreamAir", defaultSettings.customShader.enableForDreamAir},
				{"enableForOther", defaultSettings.customShader.enableForOther},
				{"contrast", defaultSettings.customShader.contrast},
				{"contrastMidpoint", defaultSettings.customShader.contrastMidpoint},
				{"contrastLinear", defaultSettings.customShader.contrastLinear},
				{"contrastPerEye", defaultSettings.customShader.contrastPerEye},
				{"contrastPerEyeLinear", defaultSettings.customShader.contrastPerEyeLinear},
				{"contrastLeft", defaultSettings.customShader.contrastLeft},
				{"contrastMidpointLeft", defaultSettings.customShader.contrastMidpointLeft},
				{"contrastRight", defaultSettings.customShader.contrastRight},
				{"contrastMidpointRight", defaultSettings.customShader.contrastMidpointRight},
				{"saturation", defaultSettings.customShader.saturation},
				{"gamma", defaultSettings.customShader.gamma},
				{"subpixelShift", defaultSettings.customShader.subpixelShift},
				{"disableMuraCorrection", defaultSettings.customShader.disableMuraCorrection},
				{"disableBlackLevels", defaultSettings.customShader.disableBlackLevels},
				{"srgbColorCorrection", defaultSettings.customShader.srgbColorCorrection},
				{"srgbWhitePointCorrection", defaultSettings.customShader.srgbWhitePointCorrection},
				{"srgbColorCorrectionMatrix", defaultSettings.customShader.srgbColorCorrectionMatrix},
				{"lensColorCorrection", defaultSettings.customShader.lensColorCorrection},
				{"dither10Bit", defaultSettings.customShader.dither10Bit},
				{"enableFilterForOverlay", defaultSettings.customShader.enableFilterForOverlay},
				{"enableFilterForDashboard", defaultSettings.customShader.enableFilterForDashboard},
				{"samplingFilter", defaultSettings.customShader.samplingFilter},
				{"samplingFilterFXAA2SharpenStrength", defaultSettings.customShader.samplingFilterFXAA2SharpenStrength},
				{"samplingFilterFXAA2SharpenClamp", defaultSettings.customShader.samplingFilterFXAA2SharpenClamp},
				{"samplingFilterFXAA2CASStrength", defaultSettings.customShader.samplingFilterFXAA2CASStrength},
				{"samplingFilterFXAA2CASContrast", defaultSettings.customShader.samplingFilterFXAA2CASContrast},
				{"samplingFilterLumaSharpenStrength", defaultSettings.customShader.samplingFilterLumaSharpenStrength},
				{"samplingFilterLumaSharpenClamp", defaultSettings.customShader.samplingFilterLumaSharpenClamp},
				{"samplingFilterLumaSharpenPattern", defaultSettings.customShader.samplingFilterLumaSharpenPattern},
				{"samplingFilterLumaSharpenRadius", defaultSettings.customShader.samplingFilterLumaSharpenRadius},
				{"samplingFilterCASStrength", defaultSettings.customShader.samplingFilterCASStrength},
				{"samplingFilterCASContrast", defaultSettings.customShader.samplingFilterCASContrast},
				{"colorMultiplier", {
					{"r", defaultSettings.customShader.colorMultiplier.r},
					{"g", defaultSettings.customShader.colorMultiplier.g},
					{"b", defaultSettings.customShader.colorMultiplier.b},
				}},
			}},
			{"forceTracking", defaultSettings.forceTracking},
			{"controllers", {
				{"spaceVelocityFix", defaultSettings.controllers.spaceVelocityFixMode == 1 ? "world" : (defaultSettings.controllers.spaceVelocityFixMode == 2 ? "driver" : "off")},
				{"rotationOffsetDeg", {
					{"x", defaultSettings.controllers.rotationOffsetDeg[0]},
					{"y", defaultSettings.controllers.rotationOffsetDeg[1]},
					{"z", defaultSettings.controllers.rotationOffsetDeg[2]},
				}},
				{"positionOffsetCm", {
					{"x", defaultSettings.controllers.positionOffsetCm[0]},
					{"y", defaultSettings.controllers.positionOffsetCm[1]},
					{"z", defaultSettings.controllers.positionOffsetCm[2]},
				}},
				{"left", {
					{"rotationOffsetDeg", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
					{"positionOffsetCm", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
				}},
				{"right", {
					{"rotationOffsetDeg", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
					{"positionOffsetCm", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
				}},
				{"aligner", {{"enable", defaultSettings.controllers.aligner.enable}}},
			}},
			{"streamFrame", {
				{"enable", defaultSettings.streamFrame.enable},
				{"saturation", defaultSettings.streamFrame.saturation},
				{"vibrance", defaultSettings.streamFrame.vibrance},
				{"contrast", defaultSettings.streamFrame.contrast},
				{"contrastMidpoint", defaultSettings.streamFrame.contrastMidpoint},
				{"contrastLinear", defaultSettings.streamFrame.contrastLinear},
				{"gamma", defaultSettings.streamFrame.gamma},
				{"colorMultiplier", {
					{"r", defaultSettings.streamFrame.colorMultiplier.r},
					{"g", defaultSettings.streamFrame.colorMultiplier.g},
					{"b", defaultSettings.streamFrame.colorMultiplier.b},
				}},
				{"srgbMatrix", defaultSettings.streamFrame.srgbMatrix},
				{"fxaa", defaultSettings.streamFrame.fxaaMode == 2 ? "quality" : (defaultSettings.streamFrame.fxaaMode == 1 ? "fast" : "off")},
				{"cas", {
					{"enable", defaultSettings.streamFrame.cas.enable},
					{"strength", defaultSettings.streamFrame.cas.strength},
					{"perEye", defaultSettings.streamFrame.cas.perEye},
					{"strengthLeft", defaultSettings.streamFrame.cas.strengthLeft},
					{"strengthRight", defaultSettings.streamFrame.cas.strengthRight},
				}},
				{"dither", defaultSettings.streamFrame.dither},
				{"blackFloor", {
					{"rampBar", defaultSettings.streamFrame.blackFloor.rampBar},
					{"rangeMode", defaultSettings.streamFrame.blackFloor.rangeMode == 2 ? "expand" : (defaultSettings.streamFrame.blackFloor.rangeMode == 1 ? "compress" : "off")},
					{"shadowLift", defaultSettings.streamFrame.blackFloor.shadowLift},
					{"floorCode", defaultSettings.streamFrame.blackFloor.floorCode},
					{"kneeCode", defaultSettings.streamFrame.blackFloor.kneeCode},
					{"blackPointCode", defaultSettings.streamFrame.blackFloor.blackPointCode},
				}},
				{"stationaryDimming", {
					{"enable", defaultSettings.streamFrame.stationaryDimming.enable},
					{"movementThreshold", defaultSettings.streamFrame.stationaryDimming.movementThreshold},
					{"movementTime", defaultSettings.streamFrame.stationaryDimming.movementTime},
					{"dimSeconds", defaultSettings.streamFrame.stationaryDimming.dimSeconds},
					{"brightenSeconds", defaultSettings.streamFrame.stationaryDimming.brightenSeconds},
				}},
				{"k1", defaultSettings.streamFrame.k1},
				{"k2", defaultSettings.streamFrame.k2},
				{"distortion", {
					{"mode", defaultSettings.streamFrame.distortion.mode},
					{"points", json::array()},
					{"gain", defaultSettings.streamFrame.distortion.gain},
					{"centerTune", {
						{"enable", defaultSettings.streamFrame.distortion.centerTune.enable},
						{"breatheAmp", defaultSettings.streamFrame.distortion.centerTune.breatheAmp},
					}},
					{"tune", {
						{"enable", defaultSettings.streamFrame.distortion.tune.enable},
						{"rate", defaultSettings.streamFrame.distortion.tune.rate},
						{"stepSize", defaultSettings.streamFrame.distortion.tune.stepSize},
						{"ringOpacity", defaultSettings.streamFrame.distortion.tune.ringOpacity},
						{"forceGrid", defaultSettings.streamFrame.distortion.tune.forceGrid},
						{"segments", defaultSettings.streamFrame.distortion.tune.segments},
						{"segmentLayout", defaultSettings.streamFrame.distortion.tune.segmentLayout},
						{"bands", defaultSettings.streamFrame.distortion.tune.bands},
					}},
					{"perEye", defaultSettings.streamFrame.distortion.perEye},
					{"perAxis", defaultSettings.streamFrame.distortion.perAxis},
					{"curves", json::object()},
					{"segments", defaultSettings.streamFrame.distortion.segments},
					{"annulus", {
						{"enable", defaultSettings.streamFrame.distortion.annulus.enable},
						{"rMin", defaultSettings.streamFrame.distortion.annulus.rMin},
						{"rMax", defaultSettings.streamFrame.distortion.annulus.rMax},
						{"feather", defaultSettings.streamFrame.distortion.annulus.feather},
					}},
				}},
				{"centerOffsetXLeft", defaultSettings.streamFrame.centerOffsetXLeft},
				{"centerOffsetXRight", defaultSettings.streamFrame.centerOffsetXRight},
				{"centerOffsetY", defaultSettings.streamFrame.centerOffsetY},
				{"alignment", {
					{"leftH", defaultSettings.streamFrame.alignment.leftH},
					{"leftV", defaultSettings.streamFrame.alignment.leftV},
					{"rightH", defaultSettings.streamFrame.alignment.rightH},
					{"rightV", defaultSettings.streamFrame.alignment.rightV},
				}},
				{"skipColorWhileDashboardOpen", defaultSettings.streamFrame.skipColorWhileDashboardOpen},
				{"processAtSubmitLayer", defaultSettings.streamFrame.processAtSubmitLayer},
				{"poseLogging", defaultSettings.streamFrame.poseLogging},
				{"poseLogBurst", defaultSettings.streamFrame.poseLogBurst},
				{"syncTimeoutMs", defaultSettings.streamFrame.syncTimeoutMs},
				{"reconLogger", defaultSettings.streamFrame.reconLogger},
				{"hitchDiag", defaultSettings.streamFrame.hitchDiag},
				{"deferredEviction", defaultSettings.streamFrame.deferredEviction},
				{"directRender", defaultSettings.streamFrame.directRender},
				{"zeroCopy", defaultSettings.streamFrame.zeroCopy},
				{"eyeGaze", {
					{"debugRing", defaultSettings.streamFrame.eyeGaze.debugRing},
					{"tanHalfFovX", defaultSettings.streamFrame.eyeGaze.tanHalfFovX},
					{"tanHalfFovY", defaultSettings.streamFrame.eyeGaze.tanHalfFovY},
					{"predictionMs", defaultSettings.streamFrame.eyeGaze.predictionMs},
					{"debugGrid", defaultSettings.streamFrame.eyeGaze.debugGrid},
					{"gridMode", defaultSettings.streamFrame.eyeGaze.gridMode},
					{"gridAngularDeg", defaultSettings.streamFrame.eyeGaze.gridAngularDeg},
					{"calibDot", defaultSettings.streamFrame.eyeGaze.calibDot},
					{"probeCapture", defaultSettings.streamFrame.eyeGaze.probeCapture},
					{"gridWorldLocked", defaultSettings.streamFrame.eyeGaze.gridWorldLocked},
					{"gridOpaque", defaultSettings.streamFrame.eyeGaze.gridOpaque},
					{"swimProbe", defaultSettings.streamFrame.eyeGaze.swimProbe},
					{"overlayWarped", defaultSettings.streamFrame.eyeGaze.overlayWarped},
				}},
				{"pupilSwim", {
					{"centerStrengthX", defaultSettings.streamFrame.pupilSwim.centerStrengthX},
					{"centerStrengthY", defaultSettings.streamFrame.pupilSwim.centerStrengthY},
				}},
				{"zeroCopyV3", defaultSettings.streamFrame.zeroCopyV3},
				{"nvencTap", defaultSettings.streamFrame.nvencTap},
				{"velocityFixMode", defaultSettings.streamFrame.velocityFixMode == 6 ? "kalmanCA" : (defaultSettings.streamFrame.velocityFixMode == 5 ? "kalmanCAM" : (defaultSettings.streamFrame.velocityFixMode == 4 ? "kalman" : (defaultSettings.streamFrame.velocityFixMode == 3 ? "derive" : (defaultSettings.streamFrame.velocityFixMode == 2 ? "full" : (defaultSettings.streamFrame.velocityFixMode == 1 ? "classic" : "off")))))},
				{"deriveSmoothTauSlowMs", defaultSettings.streamFrame.deriveSmoothTauSlowMs},
				{"deriveSmoothTauFastMs", defaultSettings.streamFrame.deriveSmoothTauFastMs},
				{"deriveSmoothSpeedLow", defaultSettings.streamFrame.deriveSmoothSpeedLow},
				{"deriveSmoothSpeedHigh", defaultSettings.streamFrame.deriveSmoothSpeedHigh},
				{"deriveSmoothAngSeparate", defaultSettings.streamFrame.deriveSmoothAngSeparate},
				{"deriveSmoothAngTauSlowMs", defaultSettings.streamFrame.deriveSmoothAngTauSlowMs},
				{"deriveSmoothAngTauFastMs", defaultSettings.streamFrame.deriveSmoothAngTauFastMs},
				{"deriveSmoothAngSpeedLow", defaultSettings.streamFrame.deriveSmoothAngSpeedLow},
				{"deriveSmoothAngSpeedHigh", defaultSettings.streamFrame.deriveSmoothAngSpeedHigh},
				{"deriveSplitDirLinear", defaultSettings.streamFrame.deriveSplitDirLinear},
				{"deriveSplitDirAngular", defaultSettings.streamFrame.deriveSplitDirAngular},
				{"deriveDirWindowMs", defaultSettings.streamFrame.deriveDirWindowMs},
				{"deriveDirWeightPow", defaultSettings.streamFrame.deriveDirWeightPow},
				{"deriveDirSource", defaultSettings.streamFrame.deriveDirSource == 2 ? "runtime" : (defaultSettings.streamFrame.deriveDirSource == 0 ? "window" : "secant")},
				{"deriveMagSource", defaultSettings.streamFrame.deriveMagSource == 1 ? "scalar" : "vector"},
				{"deriveReleaseLatch", defaultSettings.streamFrame.deriveReleaseLatch},
				{"deriveLatchWindowMs", defaultSettings.streamFrame.deriveLatchWindowMs},
				{"deriveLatchHoldMs", defaultSettings.streamFrame.deriveLatchHoldMs},
				{"deriveLatchMinSpeed", defaultSettings.streamFrame.deriveLatchMinSpeed},
				{"deriveLatchAngMinSpeed", defaultSettings.streamFrame.deriveLatchAngMinSpeed},
				{"derivePreFilter", defaultSettings.streamFrame.derivePreFilter == 1 ? "median3" : "off"},
				{"derivePreSmoothMs", defaultSettings.streamFrame.derivePreSmoothMs},
				{"derivePreSmoothScope", defaultSettings.streamFrame.derivePreSmoothScope == 1 ? "both" : "direction"},
				{"deriveDiagVelocity", defaultSettings.streamFrame.deriveDiagVelocity == 1 ? "zero" : "off"},
				{"deriveLatchPoseAssist", defaultSettings.streamFrame.deriveLatchPoseAssist},
				{"kalmanProcessAccel", defaultSettings.streamFrame.kalmanProcessAccel},
				{"kalmanPosNoiseMm", defaultSettings.streamFrame.kalmanPosNoiseMm},
				{"kalmanProcessAngAccel", defaultSettings.streamFrame.kalmanProcessAngAccel},
				{"kalmanOriNoiseDeg", defaultSettings.streamFrame.kalmanOriNoiseDeg},
				{"kalmanLeadMs", defaultSettings.streamFrame.kalmanLeadMs},
				{"kalmanReleaseRewindMs", defaultSettings.streamFrame.kalmanReleaseRewindMs},
				{"kalmanRewindHoldMs", defaultSettings.streamFrame.kalmanRewindHoldMs},
				{"kalmanDirSmoothMs", defaultSettings.streamFrame.kalmanDirSmoothMs},
				{"kalmanAngDirSmoothMs", defaultSettings.streamFrame.kalmanAngDirSmoothMs},
				{"kalmanMagSource", defaultSettings.streamFrame.kalmanMagSource == 1 ? "fast" : "state"},
				{"kalmanMagAccel", defaultSettings.streamFrame.kalmanMagAccel},
				{"kalmanMagScale", defaultSettings.streamFrame.kalmanMagScale},
				{"kalmanAngMagScale", defaultSettings.streamFrame.kalmanAngMagScale},
				{"kalmanDupMode", defaultSettings.streamFrame.kalmanDupMode == 4 ? "age" : (defaultSettings.streamFrame.kalmanDupMode == 3 ? "soft" : (defaultSettings.streamFrame.kalmanDupMode == 2 ? "drop" : (defaultSettings.streamFrame.kalmanDupMode == 1 ? "coast" : "off")))},
				{"kalmanDupRScale", defaultSettings.streamFrame.kalmanDupRScale},
				{"kalmanTeleportM", defaultSettings.streamFrame.kalmanTeleportM},
				{"kalmanLossCoastMs", defaultSettings.streamFrame.kalmanLossCoastMs},
				{"streamFrameSchema", defaultSettings.streamFrame.streamFrameSchema},
				{"graveyardEnable", defaultSettings.streamFrame.graveyardEnable},
				{"kalmanDeviceTime", defaultSettings.streamFrame.kalmanDeviceTime},
				{"kalmanPosFreeze3dof", defaultSettings.streamFrame.kalmanPosFreeze3dof},
				{"kalmanAngularOutFrame", defaultSettings.streamFrame.kalmanAngularOutFrame},
				{"kalmanFreezeCoastTurn", defaultSettings.streamFrame.kalmanFreezeCoastTurn},
				{"kalmanPosFreezeVelDecayMs", defaultSettings.streamFrame.kalmanPosFreezeVelDecayMs},
				{"kalmanDupCoastMaxMs", defaultSettings.streamFrame.kalmanDupCoastMaxMs},
				{"kalmanGazeAssist", defaultSettings.streamFrame.kalmanGazeAssist},
				{"kalmanGazeMaxDeg", defaultSettings.streamFrame.kalmanGazeMaxDeg},
				{"kalmanGazeMinSpeed", defaultSettings.streamFrame.kalmanGazeMinSpeed},
				{"kalmanSmoothLagMs", defaultSettings.streamFrame.kalmanSmoothLagMs},
				{"kalmanSmoothLagEpoch", defaultSettings.streamFrame.kalmanSmoothLagEpoch},
				{"kalmanDirLeadMs", defaultSettings.streamFrame.kalmanDirLeadMs},
				{"kalmanDirLeadAdaptive", defaultSettings.streamFrame.kalmanDirLeadAdaptive},
				{"kalmanDirLeadBaseMs", defaultSettings.streamFrame.kalmanDirLeadBaseMs},
				{"kalmanDirLeadWMs", defaultSettings.streamFrame.kalmanDirLeadWMs},
				{"kalmanAdaptiveR", defaultSettings.streamFrame.kalmanAdaptiveR},
				{"kalmanAdaptiveRMaxDiv", defaultSettings.streamFrame.kalmanAdaptiveRMaxDiv},
				{"kalmanCaJerk", defaultSettings.streamFrame.kalmanCaJerk},
				{"kalmanCaAngJerk", defaultSettings.streamFrame.kalmanCaAngJerk},
				{"kalmanCaPosNoiseMm", defaultSettings.streamFrame.kalmanCaPosNoiseMm},
				{"kalmanCaOriNoiseDeg", defaultSettings.streamFrame.kalmanCaOriNoiseDeg},
				{"kalmanCaAccelTauMs", defaultSettings.streamFrame.kalmanCaAccelTauMs},
				{"kalmanCaMagJerk", defaultSettings.streamFrame.kalmanCaMagJerk},
				{"kalmanCaMagAccelTauMs", defaultSettings.streamFrame.kalmanCaMagAccelTauMs},
				{"kalmanCaReportAccel", defaultSettings.streamFrame.kalmanCaReportAccel},
				{"kalmanCaExactCov", defaultSettings.streamFrame.kalmanCaExactCov},
				{"kalmanGripEnable", defaultSettings.streamFrame.kalmanGripEnable},
				{"kalmanGripBlend", defaultSettings.streamFrame.kalmanGripBlend},
				{"kalmanGripLeftCm", {
					{"x", defaultSettings.streamFrame.kalmanGripLeftCm[0]},
					{"y", defaultSettings.streamFrame.kalmanGripLeftCm[1]},
					{"z", defaultSettings.streamFrame.kalmanGripLeftCm[2]},
				}},
				{"kalmanGripRightCm", {
					{"x", defaultSettings.streamFrame.kalmanGripRightCm[0]},
					{"y", defaultSettings.streamFrame.kalmanGripRightCm[1]},
					{"z", defaultSettings.streamFrame.kalmanGripRightCm[2]},
				}},
			}},
			{"takeCompositorScreenshots", defaultSettings.takeCompositorScreenshots},
			{"onlyHandlePrivateFunctionality", defaultSettings.onlyHandlePrivateFunctionality},
			// {"watchDistortionProfiles", defaultSettings.watchDistortionProfiles}
		}},
		{"builtInDistortionProfiles", emptyObject},
		{"resolution", {
			{"fovX", info.renderFovX},
			{"fovY", info.renderFovY},
			{"fovMaxX", info.renderFovMaxX},
			{"fovMaxY", info.renderFovMaxY},
			{"combinedFovX", info.combinedFovX},
			{"combinedFovY", info.combinedFovY},
			{"renderResolution1To1X", info.renderResolution1To1X},
			{"renderResolution1To1Y", info.renderResolution1To1Y},
			{"renderResolution1To1Percent", info.renderResolution1To1Percent},
			{"renderResolution100PercentX", info.renderResolution100PercentX},
			{"renderResolution100PercentY", info.renderResolution100PercentY},
			{"outputResolutionX", info.outputResolutionX},
			{"outputResolutionY", info.outputResolutionY},
		}},
		{"connectedHeadset", (int)info.connectedHeadset},
		{"nonNativeHeadsetFound", info.nonNativeHeadsetFound},
		{"isDashboardOpen", info.isDashboardOpen},
		{"debugLog", info.debugLog},
		{"driverName", info.driverName},
		{"driverResources", info.driverResources},
		{"steamvrResources", info.steamvrResources},
		{"driverVersion", driverVersion}
	};
	ordered_json& distortionProfilesJson = data["builtInDistortionProfiles"];
	for(auto profilePair : builtInDistortionProfiles){
		auto profile = profilePair.second;
		ordered_json profileJson = {
			{"device", profile.device},
			{"description", profile.description},
			{"author", profile.author},
			{"creationDate", profile.creationDate},
			{"type", profile.type},
			{"distortions", profile.distortions},
			{"distortionsRed", profile.distortionsRed},
			{"distortionsBlue", profile.distortionsBlue},
			{"legacySmoothing", profile.legacySmoothing},
			{"smoothAmount", profile.smoothAmount},
			{"offsetX", profile.offsetX},
			{"offsetY", profile.offsetY},
		};
		distortionProfilesJson[profile.name] = profileJson;
	}
	infoFile << data.dump(1, '\t');
	infoFile.close();
}

void ConfigLoader::ReadInfo(){
	std::string infoPath = GetConfigFolder() + "info.json";
	std::ifstream infoFile(infoPath);
	if(!infoFile.is_open()){
		#ifdef _WIN32
		DriverLog("Failed to open info.json for reading: %d", GetLastError());
		#else
		DriverLog("Failed to open info.json for reading");
		#endif
		return;
	}
	try{
		std::lock_guard<std::mutex> lock(driverConfigLock);
		json data = json::parse(infoFile, nullptr, true, true);
		if(data["driverResources"].is_string()){
			info.driverResources = data["driverResources"].get<std::string>();
		}
		if(data["steamvrResources"].is_string()){
			info.steamvrResources = data["steamvrResources"].get<std::string>();
		}
		if(data["connectedHeadset"].is_number()){
			info.connectedHeadset = (Config::HeadsetType)data["connectedHeadset"].get<int>();
		}
		if(data["resolution"].is_object()){
			auto res = data["resolution"];
			if(res["fovX"].is_number()) info.renderFovX = res["fovX"].get<double>();
			if(res["fovY"].is_number()) info.renderFovY = res["fovY"].get<double>();
			if(res["fovMaxX"].is_number()) info.renderFovMaxX = res["fovMaxX"].get<double>();
			if(res["fovMaxY"].is_number()) info.renderFovMaxY = res["fovMaxY"].get<double>();
			if(res["combinedFovX"].is_number()) info.combinedFovX = res["combinedFovX"].get<double>();
			if(res["combinedFovY"].is_number()) info.combinedFovY = res["combinedFovY"].get<double>();
			if(res["renderResolution1To1X"].is_number()) info.renderResolution1To1X = res["renderResolution1To1X"].get<int>();
			if(res["renderResolution1To1Y"].is_number()) info.renderResolution1To1Y = res["renderResolution1To1Y"].get<int>();
			if(res["renderResolution1To1Percent"].is_number()) info.renderResolution1To1Percent = res["renderResolution1To1Percent"].get<double>();
			if(res["renderResolution100PercentX"].is_number()) info.renderResolution100PercentX = res["renderResolution100PercentX"].get<int>();
			if(res["renderResolution100PercentY"].is_number()) info.renderResolution100PercentY = res["renderResolution100PercentY"].get<int>();
			if(res["outputResolutionX"].is_number()) info.outputResolutionX = res["outputResolutionX"].get<int>();
			if(res["outputResolutionY"].is_number()) info.outputResolutionY = res["outputResolutionY"].get<int>();
		}
		if(data["isDashboardOpen"].is_boolean()){
			info.isDashboardOpen = data["isDashboardOpen"].get<bool>();
		}
		info.hasBeenUpdated = true;
	}catch(const std::exception& e){
		DriverLog("Failed to parse info.json: %s", e.what());
		return;
	}
}

void ConfigLoader::WriteDiagnosticInfo(){
	std::lock_guard<std::mutex> lock(diagnosticWriteLock);
	std::string diagnosticPath = GetConfigFolder() + "diagnostic.json";
	std::ofstream diagnosticFile(diagnosticPath);
	if(!diagnosticFile.is_open()){
		#ifdef _WIN32
		DriverLog("Failed to open diagnostic.json for writing: %d", GetLastError());
		#else
		DriverLog("Failed to open diagnostic.json for writing");
		#endif
		return;
	}
	ordered_json data = {
		{"about", "This file provides diagnostic information from the driver, updated 4 times per second."},
		{"vrserverPID", diagnosticInfo.vrserverPID},
		{"eyeTracking", {
			{"valid", diagnosticInfo.eyeTrackingValid},
			{"left", {
				{"angleX", diagnosticInfo.leftAngleX},
				{"angleY", diagnosticInfo.leftAngleY},
			}},
			{"right", {
				{"angleX", diagnosticInfo.rightAngleX},
				{"angleY", diagnosticInfo.rightAngleY},
			}},
			{"focalPoint", {
				{"x", diagnosticInfo.focalPointX},
				{"y", diagnosticInfo.focalPointY},
				{"z", diagnosticInfo.focalPointZ},
			}},
		}},
		{"streamFrame", {
			{"active", diagnosticInfo.streamFrameActive},
			{"frameCounter", diagnosticInfo.streamFrameCounter},
			{"calibPatternShown", diagnosticInfo.calibPatternShown},
			{"calibPatternFrames", diagnosticInfo.calibPatternFrames},
			{"calibBlackout", diagnosticInfo.calibBlackout},
			{"projValid", diagnosticInfo.projValid},
			{"projLeft", {diagnosticInfo.proj[0][0], diagnosticInfo.proj[0][1], diagnosticInfo.proj[0][2], diagnosticInfo.proj[0][3]}},
			{"projRight", {diagnosticInfo.proj[1][0], diagnosticInfo.proj[1][1], diagnosticInfo.proj[1][2], diagnosticInfo.proj[1][3]}},
			{"eyeTexWidth", diagnosticInfo.eyeTexWidth},
			{"eyeTexHeight", diagnosticInfo.eyeTexHeight},
			{"eyeAspect", diagnosticInfo.eyeAspect},
			{"mapCols", diagnosticInfo.mapCols},
			{"mapRows", diagnosticInfo.mapRows},
			{"mapActive", diagnosticInfo.mapActive},
		}},
	};
	diagnosticFile << data.dump(1, '\t');
	diagnosticFile.close();
}

void ConfigLoader::WriteDiagnosticInfoThread(){
	while(started){
		if(info.needToWrite){
			// also handle writing info asynchronously
			WriteInfo();
		}
		WriteDiagnosticInfo();
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}

// void ConfigLoader::WriteInfoThread(){
	// while(started){
	// 	WriteInfo();
	// 	std::this_thread::sleep_for(std::chrono::milliseconds(10000));
	// }
// }
#ifdef _WIN32
void ConfigLoader::WatcherThread(){
	// watch for changes in the config file directory
	std::string configPath = GetConfigFolder();
	HANDLE hDir = CreateFileA(configPath.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if(hDir == INVALID_HANDLE_VALUE){
		DriverLog("Failed to open config directory for watching: %d", GetLastError());
		return;
	}
	while(started){
		DWORD bytesReturned;
		char buffer[1024]{};
		FILE_NOTIFY_INFORMATION* pNotify;
		BOOL success = ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, &bytesReturned, NULL, NULL);
		if(!success){
			DriverLog("Failed to read directory changes: %d", GetLastError());
			break;
		}
		pNotify = (FILE_NOTIFY_INFORMATION*)buffer;
		bool hasReloadedConfig = false;
		bool hasReloadedInfo = false;
		do{
			std::wstring fileName(pNotify->FileName, pNotify->FileNameLength / sizeof(wchar_t));
			if(!hasReloadedConfig && fileName == L"settings.json" && (pNotify->Action == FILE_ACTION_MODIFIED || pNotify->Action == FILE_ACTION_ADDED || pNotify->Action == FILE_ACTION_RENAMED_NEW_NAME)){
				DriverLog("Config file changed, reloading...");
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				while(driverConfig.hasBeenUpdated){
					// wait for the last config update to be used before doing another one
					std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
				}
				ParseConfig();
				hasReloadedConfig = true;
			}
			if(watchInfo && !hasReloadedInfo && fileName == L"info.json" && (pNotify->Action == FILE_ACTION_MODIFIED || pNotify->Action == FILE_ACTION_ADDED || pNotify->Action == FILE_ACTION_RENAMED_NEW_NAME)){
				DriverLog("Info file changed, reloading...");
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				ReadInfo();
				hasReloadedInfo = true;
			}
			// advance AFTER processing; the old form checked the NEXT
			// entry's offset before processing it, so the last entry of a
			// multi-entry buffer (e.g. temp file + rename to settings.json)
			// was silently dropped and hot reload appeared flaky
			if(pNotify->NextEntryOffset == 0){
				break;
			}
			pNotify = (FILE_NOTIFY_INFORMATION*)((char*)pNotify + pNotify->NextEntryOffset);
		}while(true);
		//DriverLog("Waiting for next change...");
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
	}
}

void ConfigLoader::WatcherThreadDistortions(){
	std::string configPath = GetConfigFolder() + "Distortion/";
	HANDLE hDir = CreateFileA(configPath.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if(hDir == INVALID_HANDLE_VALUE){
		DriverLog("Failed to open distortion directory for watching: %d", GetLastError());
		return;
	}
	while(started){
		DWORD bytesReturned;
		char buffer[1024]{};
		FILE_NOTIFY_INFORMATION* pNotify;
		BOOL success = ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, &bytesReturned, NULL, NULL);
		if(!success){
			DriverLog("Failed to read directory changes: %d", GetLastError());
			break;
		}
		pNotify = (FILE_NOTIFY_INFORMATION*)buffer;
		do{
			std::wstring fileName(pNotify->FileName, pNotify->FileNameLength / sizeof(wchar_t));
			if(fileName.size() >= 5 && fileName.substr(fileName.size() - 5) == L".json" && (pNotify->Action == FILE_ACTION_MODIFIED || pNotify->Action == FILE_ACTION_ADDED || pNotify->Action == FILE_ACTION_RENAMED_NEW_NAME)){
				DriverLog("Distortion profile changed, reloading...");
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				while(driverConfig.hasBeenUpdated){
					// wait for the last config update to be used before doing another one
					std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
				}
				ParseConfig();
				break;
			}
			if(pNotify->NextEntryOffset == 0){
				break;
			}
			pNotify = (FILE_NOTIFY_INFORMATION*)((char*)pNotify + pNotify->NextEntryOffset);
		}while(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
	}
}
#elif __linux__
// use the c inotify system to watch for changes to the config file
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

void ConfigLoader::WatcherThread(){
	std::string configPath = GetConfigFolder();
	int fd = inotify_init();
	if(fd == -1){
		DriverLog("Error initializing inotify\n");
		return;
	}
	int wd = inotify_add_watch(fd, configPath.c_str(), IN_MODIFY | IN_CREATE);
	if(wd == -1){
		DriverLog("Error adding inotify watch to config folder\n");
		return;
	}
	char buffer[sizeof(struct inotify_event) * 16];
	while(started){
		int length = read(fd, buffer, sizeof(buffer));
		if(length == -1){
			DriverLog("Error reading inotify events\n");
			break;
		}
		// process events
		int i = 0;
		
		bool hasReloadedConfig = false;
		bool hasReloadedInfo = false;
		while(i < length){
			struct inotify_event *event = (struct inotify_event *) &buffer[i];
			if(event->mask & IN_MODIFY || event->mask & IN_CREATE){
				if(event->len){
					std::string fileName = event->name;
					if(fileName == "settings.json" && !hasReloadedConfig){
						DriverLog("Config file changed, reloading...");
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
						while(driverConfig.hasBeenUpdated){
							// wait for the last config update to be used before doing another one
							std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
						}
						ParseConfig();
						hasReloadedConfig = true;
					}
					if(fileName == "info.json" && !hasReloadedInfo){
						DriverLog("Info file changed, reloading...");
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
						ReadInfo();
						hasReloadedInfo = true;
					}
				}
			}
			i += sizeof(struct inotify_event) + event->len;
		}
	}
	inotify_rm_watch(fd, wd);
	close(fd);
}


void ConfigLoader::WatcherThreadDistortions(){
	std::string configPath = GetConfigFolder() + "Distortion/";
	int fd = inotify_init();
	if(fd == -1){
		DriverLog("Error initializing inotify\n");
		return;
	}
	int wd = inotify_add_watch(fd, configPath.c_str(), IN_MODIFY | IN_CREATE);
	if(wd == -1){
		DriverLog("Error adding inotify watch to distortion folder\n");
		return;
	}
	char buffer[sizeof(struct inotify_event) * 16];
	while(started){
		int length = read(fd, buffer, sizeof(buffer));
		if(length == -1){
			DriverLog("Error reading inotify events\n");
			break;
		}
		// process events
		int i = 0;
		while(i < length){
			struct inotify_event *event = (struct inotify_event *) &buffer[i];
			if(event->mask & IN_MODIFY || event->mask & IN_CREATE){
				if(event->len){
					std::string fileName = event->name;
					if(fileName.size() >= 5 && fileName.substr(fileName.size() - 5) == ".json"){
						DriverLog("Distortion profile changed, reloading...");
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							while(driverConfig.hasBeenUpdated){
								// wait for the last config update to be used before doing another one
								std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
							}
						ParseConfig();
						break;
					}
				}
			}
			i += sizeof(struct inotify_event) + event->len;
		}
	}
	inotify_rm_watch(fd, wd);
	close(fd);
}
#endif

				
// only define settings that most users will change and are unlikely to have their default changed
// settings not defined here will easily be able to have their defaults changed in the future for everyone
std::string defaultConfig = R"({
	"meganeX8K": {
		"enable": true
	}
})";


void ConfigLoader::Start(){
	if(started){
		return;
	}
	started = true;
	
	// migrate legacy settings before the default-config check below, so an
	// existing user's settings are found instead of writing fresh defaults
	MigrateLegacyConfig();
	
	try{
		// create directory
		std::filesystem::create_directories(GetConfigFolder());
		
		// create default config if it doesn't exist
		std::string configPath = GetConfigFolder() + "settings.json";
		if(!std::filesystem::exists(configPath)){
			std::ofstream configFile(configPath);
			configFile << defaultConfig;
			configFile.close();
		}
	}catch(const std::exception& e){
		DriverLog("Failed to create settings.json %s", e.what());
	}
	
	// load config for the first time
	ParseConfig();
	
	bool onlyHandlePrivateFunctionality = false;
	#ifdef HAS_PRIVATE
	onlyHandlePrivateFunctionality = driverConfig.onlyHandlePrivateFunctionality;
	#endif
	
	try{
		if(watchInfo || onlyHandlePrivateFunctionality){
			ReadInfo();
		}else{
			WriteInfo();
			// start info thread
			// std::thread infoThread(&ConfigLoader::WriteInfoThread, this);
			// infoThread.detach();
		}
	}catch(const std::exception& e){
		DriverLog("Failed to manage info.json: %s", e.what());
	}
	if(!watchInfo && !onlyHandlePrivateFunctionality){
		// Set the current process ID
		#ifdef _WIN32
		diagnosticInfo.vrserverPID = (uint32_t)GetCurrentProcessId();
		#else
		diagnosticInfo.vrserverPID = (uint32_t)getpid();
		#endif
		try{
			// start diagnostic info thread
			std::thread diagnosticThread(&ConfigLoader::WriteDiagnosticInfoThread, this);
			diagnosticThread.detach();
		}catch(const std::exception& e){
			DriverLog("Failed to start diagnostic thread: %s", e.what());
		}
	}
	try{	
		// start watcher thread
		std::thread watcher(&ConfigLoader::WatcherThread, this);
		// detach the thread to run forever
		watcher.detach();
		
		// create distortion profiles directory and watch if configured
		std::filesystem::create_directories(GetConfigFolder() + "Distortion/");
		// if(driverConfig.watchDistortionProfiles){
		std::thread distortionWatcher(&ConfigLoader::WatcherThreadDistortions, this);
		distortionWatcher.detach();
		// }
	}catch(const std::exception& e){
		DriverLog("Failed to start config watcher: %s", e.what());
	}
}


ConfigLoader driverConfigLoader = {};