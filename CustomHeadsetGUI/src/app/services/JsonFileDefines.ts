export type DistortionProfileConfig = {
  name: string;
  description: string;
  modifiedTime: number;
  type: string;
  distortionProfileId: string;
  distortions: number[];
  distortionsRed: number[];
  distortionsBlue: number[];
};
/**
 * See the c++ documentation for documentation on what the settings are for
 */
export type Settings = {
  meganeX8K: MeganeX8KConfig,
  dreamAir: DreamAirConfig,
  generalHeadset: GeneralHeadsetConfig,
  customShader: CustomShaderConfig,
  streamFrame?: StreamFrameConfig,
  galaxyXr?: GalaxyXrConfig,
  controllers?: ControllersConfig,
  forceTracking: boolean,
  takeCompositorScreenshots: boolean,
  watchDistortionProfiles: boolean,
}
export type GalaxyXrConfig = {
  nativeIdentity: boolean,
  nativeInputProfile?: boolean,
  nativeResolution?: boolean,
  streamQuality?: string,
  renderModelScale?: number,
  gripConvention?: boolean,
  skeletonOffsetXCm?: number,
  skeletonOffsetYCm?: number,
  skeletonOffsetZCm?: number,
  skeletonOffsetMirror?: boolean,
  handAnchorXCm?: number,
  handAnchorYCm?: number,
  handAnchorZCm?: number,
  handAnchorPitchDeg?: number,
  handAnchorYawDeg?: number,
  handAnchorRollDeg?: number,
  meshOffsetXCm?: number,
  meshOffsetYCm?: number,
  meshOffsetZCm?: number,
  officialComponents?: boolean,
  simulateTouch?: boolean,
  aimTrimXCm?: number,
  aimTrimYCm?: number,
  aimTrimZCm?: number,
  componentRebaseIncludeTrim?: boolean,
}
export type ControllersConfig = {
  mirrorOffsetsForRightHand?: boolean,
  rotationOffsetDeg: { x: number, y: number, z: number },
  positionOffsetCm: { x: number, y: number, z: number },
  left?: HandOffsets,
  right?: HandOffsets,
  aligner: { enable: boolean },
}
export type HandOffsets = {
  rotationOffsetDeg: { x: number, y: number, z: number },
  positionOffsetCm: { x: number, y: number, z: number },
}
export type StationaryDimmingConfig = {
  enable: boolean,
  movementThreshold: number,
  movementTime: number,
  dimBrightnessPercent: number,
  dimSeconds: number,
  brightenSeconds: number
}
export type HiddenAreaMeshConfig = {
  enable: boolean;
  testMode: boolean;
  detailLevel: number;
  radiusTopOuter: number;
  radiusTopInner: number;
  radiusBottomInner: number;
  radiusBottomOuter: number;
};
export type LinearColor = {
  r: number;
  g: number;
  b: number;
};
export type StreamFrameDistortionPoint = {
  r: number;
  scale: number;
};
export type StreamFrameAnnulusConfig = {
  enable: boolean;
  rMin: number;
  rMax: number;
  feather: number;
};
export type StreamFrameCurveData = {
  k1: number;
  k2: number;
  points: StreamFrameDistortionPoint[];
};
export type StreamFrameDistortionTuneConfig = {
  enable: boolean;
  rate: number;
  bands: number[];
  stepSize: number;
  ringOpacity: number;
  forceGrid: boolean;
  segments: number;
  segmentLayout: number[];
};
export type StreamFrameCenterTuneConfig = {
  enable: boolean;
  breatheAmp: number;
};
// dense per-eye displacement map (camera calibration): cols x rows lattice
// of (du, dv) source sample offsets in bounds uv, row major, v major.
// produced by tools/gxr_sweep.py / tools/gxr_overlay.py, applied after the
// radial curves and scaled by gain. empty = identity.
export type StreamFrameDisplacementMap = {
  enable: boolean;
  cols: number;
  rows: number;
  left: number[];
  right: number[];
  source: string;
};
export type StreamFrameDistortionConfig = {
  gain: number;
  mode: string;
  points: StreamFrameDistortionPoint[];
  perEye: boolean;
  perAxis: boolean;
  curves: { [key: string]: StreamFrameCurveData };
  annulus: StreamFrameAnnulusConfig;
  segments: number;
  tune: StreamFrameDistortionTuneConfig;
  centerTune: StreamFrameCenterTuneConfig;
  map?: StreamFrameDisplacementMap;
};
// camera calibration support driven by tools/gxr_*.py through settings.json
export type StreamFrameCalibConfig = {
  blackout: boolean;
  eye: number;
  patternBrightness: number;
  captureMode: boolean;
  pattern: number;
  patternBits: number;
};
export type StreamFrameCASConfig = {
  enable: boolean;
  strength: number;
  perEye: boolean;
  strengthLeft: number;
  strengthRight: number;
};
export type StreamFrameDimmingConfig = {
  enable: boolean;
  movementThreshold: number;
  movementTime: number;
  dimSeconds: number;
  brightenSeconds: number;
};
export type StreamFrameConfig = {
  enable: boolean;
  saturation: number;
  vibrance: number;
  contrast: number;
  contrastMidpoint: number;
  contrastLinear: boolean;
  gamma: number;
  colorMultiplier: LinearColor;
  srgbMatrix: number[];
  cas: StreamFrameCASConfig;
  dither: boolean;
  stationaryDimming: StreamFrameDimmingConfig;
  k1: number;
  k2: number;
  distortion: StreamFrameDistortionConfig;
  brightness?: number;
  calib?: StreamFrameCalibConfig;
  centerOffsetXLeft: number;
  centerOffsetXRight: number;
  centerOffsetY: number;
  alignment: { leftH: number, leftV: number, rightH: number, rightV: number };
  skipColorWhileDashboardOpen: boolean;
  processAtSubmitLayer: boolean;
  syncTimeoutMs: number;
  directRender: boolean;
  zeroCopyV3: boolean;
  nvencTap: boolean;
  fxaa: string;
  hitchDiag: boolean;
  deferredEviction: boolean;
  velocityFixMode: string;
  streamFrameSchema: number;
  deriveSmoothTauSlowMs: number;
  deriveSmoothTauFastMs: number;
  deriveSmoothSpeedLow: number;
  deriveSmoothSpeedHigh: number;
  deriveSmoothAngSeparate: boolean;
  deriveSmoothAngTauSlowMs: number;
  deriveSmoothAngTauFastMs: number;
  deriveSmoothAngSpeedLow: number;
  deriveSmoothAngSpeedHigh: number;
  deriveSplitDirLinear: boolean;
  deriveSplitDirAngular: boolean;
  deriveDirWindowMs: number;
  deriveDirWeightPow: number;
  deriveDirSource: string;
  deriveMagSource: string;
  deriveReleaseLatch: boolean;
  deriveLatchWindowMs: number;
  deriveLatchHoldMs: number;
  deriveLatchMinSpeed: number;
  deriveLatchAngMinSpeed: number;
  derivePreFilter: string;
  derivePreSmoothMs: number;
  derivePreSmoothScope: string;
  deriveDiagVelocity: string;
  deriveLatchPoseAssist: boolean;
  kalmanProcessAccel: number;
  kalmanPosNoiseMm: number;
  kalmanProcessAngAccel: number;
  kalmanOriNoiseDeg: number;
  kalmanLeadMs: number;
  kalmanReleaseRewindMs: number;
  kalmanRewindHoldMs: number;
  kalmanDirSmoothMs: number;
  kalmanAngDirSmoothMs: number;
  kalmanMagSource: string;
  kalmanMagAccel: number;
  kalmanMagScale: number;
  kalmanAngMagScale: number;
  kalmanDupMode: string;
  kalmanDupRScale: number;
  graveyardEnable: boolean;
  kalmanDeviceTime: boolean;
  kalmanPosFreeze3dof: boolean;
  kalmanAngularOutFrame?: string;
  kalmanFreezeCoastTurn?: number;
  kalmanPosFreezeVelDecayMs: number;
  kalmanDupCoastMaxMs: number;
  kalmanGazeAssist: number;
  kalmanGazeMaxDeg: number;
  kalmanGazeMinSpeed: number;
  kalmanSmoothLagMs: number;
  kalmanSmoothLagEpoch: number;
  kalmanDirLeadMs: number;
  kalmanDirLeadAdaptive: boolean;
  kalmanDirLeadBaseMs: number;
  kalmanDirLeadWMs: number;
  kalmanAdaptiveR: boolean;
  kalmanAdaptiveRMaxDiv: number;
  kalmanLossCoastMs: number;
  kalmanCaJerk: number;
  kalmanCaAngJerk: number;
  kalmanCaPosNoiseMm: number;
  kalmanCaOriNoiseDeg: number;
  kalmanCaAccelTauMs: number;
  kalmanCaMagJerk: number;
  kalmanCaMagAccelTauMs: number;
  kalmanCaReportAccel: boolean;
  kalmanCaExactCov: boolean;
  kalmanGripEnable: boolean;
  kalmanGripBlend: number;
  kalmanGripLeftCm: { x: number, y: number, z: number };
  kalmanGripRightCm: { x: number, y: number, z: number };
  eyeGaze: { debugRing: boolean, tanHalfFovX: number, tanHalfFovY: number, predictionMs: number, debugGrid: boolean, gridMode: string, gridAngularDeg: number, calibDot: boolean, swimProbe: boolean, overlayWarped: boolean, probeCapture: boolean, gridWorldLocked: boolean, gridOpaque: boolean };
  blackFloor: { rampBar: boolean, rangeMode: string, shadowLift: boolean, floorCode: number, kneeCode: number, blackPointCode: number };
  pupilSwim: { centerStrengthX: number, centerStrengthY: number };
  poseLogging: boolean;
  poseLogBurst: boolean;
};
export type CustomShaderConfig = {
  enable: boolean;
  enableForMeganeX8K: boolean,
  enableForDreamAir: boolean,
  enableForOther: boolean,
  contrast: number;
  contrastMidpoint: number;
  contrastLinear: boolean;
  contrastPerEye: boolean;
  contrastPerEyeLinear: boolean;
  contrastLeft: number;
  contrastMidpointLeft: number;
  contrastRight: number;
  contrastMidpointRight: number;
  chroma: number;
  saturation: number;
  gamma: number;
  subpixelShift: boolean;
  disableMuraCorrection: boolean;
  disableBlackLevels: boolean;
  srgbColorCorrection: boolean;
  srgbWhitePointCorrection: boolean;
  srgbColorCorrectionMatrix: number[]; // 3x3 matrix as a flat array of 9 elements
  lensColorCorrection: boolean;
  dither10Bit: boolean;
  enableFilterForOverlay: boolean;
  enableFilterForDashboard: boolean;
  samplingFilter: string;
  samplingFilterFXAA2SharpenStrength: number;
  samplingFilterFXAA2SharpenClamp: number;
  samplingFilterFXAA2CASStrength: number;
  samplingFilterFXAA2CASContrast: number;
  samplingFilterLumaSharpenStrength: number;
  samplingFilterLumaSharpenClamp: number;
  samplingFilterLumaSharpenPattern: number;
  samplingFilterLumaSharpenRadius: number;
  samplingFilterCASStrength: number;
  samplingFilterCASContrast: number;
  colorMultiplier: LinearColor;
}

/**
 * Base configuration type for headset devices that share common settings.
 * Mirrors the BaseHeadsetConfig class in Config.h
 */
export type BaseHeadsetConfig = {
  enable: boolean;
  forceEnable: boolean;
  ipd: number;
  ipdOffset: number;
  horizontalIPDOffset: number;
  blackLevel: number;
  colorMultiplier: LinearColor;
  distortionProfile: string;
  distortionZoom: number;
  fovZoom: number;
  flatFovZoom: number;
  subpixelShift: number;
  subpixelOffsets: number[];
  resolutionX: number;
  resolutionY: number;
  displayRotation: number;
  maxFovX: number;
  maxFovY: number;
  distortionMeshResolution: number;
  fovBurnInPrevention: boolean;
  fovClamping: boolean;
  distortionProfileDeviceType: string;
  renderResolutionMultiplierX: number;
  renderResolutionMultiplierY: number;
  superSamplingFilterPercent: number;
  secondsFromVsyncToPhotons: number;
  secondsFromPhotonsToVblank: number;
  eyeRotation: number;
  disableEye: number;
  disableEyeDecreaseFov: number;
  edidVendorIdOverride: number;
  hiddenArea: HiddenAreaMeshConfig;
  stationaryDimming: StationaryDimmingConfig;
  parallelProjection: boolean;
  enableEyeTracking: boolean;
};

export type MeganeX8KConfig = BaseHeadsetConfig & {
  // MeganeX8K-specific fields can be added here if needed
};

export type DreamAirConfig = BaseHeadsetConfig & {
  // DreamAir-specific fields can be added here if needed
};

export type GeneralHeadsetConfig = {
  useViveBluetooth: boolean;
}

export type AppSetting = {
  colorScheme: 'system' | 'dark' | 'light';
  updateMode: 'replace' | 'rewrite';
  advanceMode: boolean;
  defaultSettingsTab: 'auto' | 'General' | 'MeganeX8K' | 'DreamAir';
  showIncompatibleProfiles: boolean;
  launchPimaxOnStartup: boolean;
}

export const HeadsetType = {
  None: 0,
  Other: 1,
  MeganeX8K: 2,
  Vive: 3,
  DreamAir: 4,
} as const;

export type HeadsetType = typeof HeadsetType[keyof typeof HeadsetType];

export type DriverInfo = {
  about: string;
  defaultSettings: Settings;
  builtInDistortionProfiles: BuiltInDistortionProfiles;
  resolution: ResolutionInfo,
  driverVersion: string,
  connectedHeadset: number,
  nonNativeHeadsetFound: boolean
}
export type ResolutionInfo = {
  fovX: number,
  fovY: number,
  fovMaxX: number,
  fovMaxY: number,
  combinedFovX: number,
  combinedFovY: number,
  renderResolution1To1X: number,
  renderResolution1To1Y: number,
  renderResolution1To1Percent: number,
  renderResolution100PercentX: number,
  renderResolution100PercentY: number
}

export type BuiltInDistortionProfile = {
  device?: string;
  distortionProfileId?: string;
  description?: string;
  author?: string;
  creationDate?: number;
  type?: string;
  distortions?: number[];
  distortionsRed?: number[];
  distortionsBlue?: number[];
  legacySmoothing?: boolean;
  smoothAmount?: number;
  offsetX?: number;
  offsetY?: number;
};

export type BuiltInDistortionProfiles = {
  [profileName: string]: BuiltInDistortionProfile;
};
