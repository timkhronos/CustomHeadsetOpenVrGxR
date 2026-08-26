import { Component, effect, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { MatSliderModule } from '@angular/material/slider';
import { MatSelectModule } from '@angular/material/select';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatInputModule } from '@angular/material/input';
import { DriverSettingService } from '../../services/driver-setting.service';
import { DriverInfoService } from '../../services/driver-info.service';
import { Settings, StreamFrameConfig, ControllersConfig, GalaxyXrConfig, HandOffsets } from '../../services/JsonFileDefines';
import { vendor } from '../../../environment';
import { FieldTipComponent } from '../../utilities/field-tip/field-tip.component';
import { ResetButtonComponent } from '../../utilities/reset-button/reset-button.component';
import { StreamFrameCurveComponent } from '../../utilities/stream-frame-curve/stream-frame-curve.component';
import { DriverEnableBannerComponent } from '../../utilities/driver-enable-banner/driver-enable-banner.component';

function defaultStreamFrame(): StreamFrameConfig {
  return {
    enable: false,
    saturation: 50,
    vibrance: 0,
    contrast: 50,
    contrastMidpoint: 50,
    contrastLinear: false,
    gamma: 2.2,
    brightness: 1,
    colorMultiplier: { r: 1, g: 1, b: 1 },
    srgbMatrix: [],
    cas: { enable: false, strength: 0.5, perEye: false, strengthLeft: 0.5, strengthRight: 0.5 },
    dither: false,
    stationaryDimming: { enable: false, movementThreshold: 0.4, movementTime: 15, dimSeconds: 10, brightenSeconds: 1 },
    k1: 0,
    k2: 0,
    distortion: {
      gain: 1, mode: 'k1k2',
      points: [],
      perEye: false,
      perAxis: false,
      curves: {},
      segments: 1,
      annulus: { enable: false, rMin: 0, rMax: 0.75, feather: 0.05 },
      tune: { enable: false, rate: 0.08, bands: [0.15, 0.22, 0.3, 0.38, 0.46, 0.55, 0.65], stepSize: 0, ringOpacity: 0.55, forceGrid: true, segments: 1, segmentLayout: [] },
      centerTune: { enable: false, breatheAmp: 0.05 }
    },
    centerOffsetXLeft: 0,
    centerOffsetXRight: 0,
    centerOffsetY: 0,
    alignment: { leftH: 0, leftV: 0, rightH: 0, rightV: 0 },
    skipColorWhileDashboardOpen: false,
    processAtSubmitLayer: false,
    syncTimeoutMs: 10,
    directRender: true,
    zeroCopyV3: false,
    nvencTap: false,
    fxaa: 'off',
    hitchDiag: true,
    deferredEviction: true,
    velocityFixMode: 'kalmanCA',
    // serializer default 0 (real schema is 2): keeps the written 2 from
    // being pruned by the default-diff serializer, so post-migration
    // choices stay sticky (see migration in the settings effect)
    streamFrameSchema: 0,
    deriveSmoothTauSlowMs: 90,
    deriveSmoothTauFastMs: 6,
    deriveSmoothSpeedLow: 0.25,
    deriveSmoothSpeedHigh: 1.6,
    deriveSmoothAngSeparate: false,
    deriveSmoothAngTauSlowMs: 90,
    deriveSmoothAngTauFastMs: 6,
    deriveSmoothAngSpeedLow: 1.7,
    deriveSmoothAngSpeedHigh: 10.5,
    deriveSplitDirLinear: false,
    deriveSplitDirAngular: false,
    deriveDirWindowMs: 50,
    deriveDirWeightPow: 2,
    deriveDirSource: 'secant',
    deriveMagSource: 'vector',
    deriveReleaseLatch: false,
    deriveLatchWindowMs: 150,
    deriveLatchHoldMs: 120,
    deriveLatchMinSpeed: 0.8,
    deriveLatchAngMinSpeed: 6,
    derivePreFilter: 'off',
    derivePreSmoothMs: 0,
    derivePreSmoothScope: 'direction',
    deriveDiagVelocity: 'off',
    deriveLatchPoseAssist: false,
    kalmanProcessAccel: 1,
    kalmanPosNoiseMm: 2.7,
    kalmanProcessAngAccel: 400,
    kalmanOriNoiseDeg: 1.25,
    kalmanLeadMs: 0,
    kalmanReleaseRewindMs: 0,
    kalmanRewindHoldMs: 100,
    kalmanDirSmoothMs: 0,
    kalmanDirLeadMs: 0,
    kalmanDirLeadAdaptive: false,
    kalmanDirLeadBaseMs: 5,
    kalmanDirLeadWMs: 0.3,
    kalmanAdaptiveR: false,
    kalmanAdaptiveRMaxDiv: 16,
    kalmanLossCoastMs: 250,
    kalmanAngDirSmoothMs: 0,
    kalmanMagSource: 'state',
    kalmanMagAccel: 60,
    kalmanMagScale: 1,
    kalmanAngMagScale: 1,
    kalmanDupMode: 'soft',
    kalmanDupRScale: 3,
    kalmanDeviceTime: true,
    kalmanPosFreeze3dof: true,
    kalmanAngularOutFrame: 'body',
    kalmanFreezeCoastTurn: 0,
    kalmanPosFreezeVelDecayMs: 0,
    kalmanDupCoastMaxMs: 90,
    kalmanGazeAssist: 0,
    kalmanGazeMaxDeg: 30,
    kalmanGazeMinSpeed: 1.2,
    kalmanSmoothLagMs: 0,
    kalmanSmoothLagEpoch: 0,
    kalmanCaJerk: 4,
    kalmanCaAngJerk: 1500,
    kalmanCaPosNoiseMm: 1.5,
    kalmanCaOriNoiseDeg: 1.5,
    kalmanCaAccelTauMs: 20,
    kalmanCaMagJerk: 800,
    kalmanCaMagAccelTauMs: 150,
    kalmanCaReportAccel: false,
    kalmanCaExactCov: true,
    kalmanGripEnable: false,
    kalmanGripBlend: 1,
    kalmanGripLeftCm: { x: 0, y: 0, z: 0 },
    kalmanGripRightCm: { x: 0, y: 0, z: 0 },
    eyeGaze: { debugRing: false, tanHalfFovX: 1.19, tanHalfFovY: 1.19, predictionMs: 30, debugGrid: false, gridMode: 'uv', gridAngularDeg: 2.5, calibDot: false, swimProbe: false, overlayWarped: false, probeCapture: false, gridWorldLocked: false, gridOpaque: false },
    blackFloor: { rampBar: false, rangeMode: 'off', shadowLift: false, floorCode: 2, kneeCode: 8, blackPointCode: 0 },
    pupilSwim: { centerStrengthX: 0, centerStrengthY: 0 },
    poseLogging: false,
    poseLogBurst: false,
    graveyardEnable: false
  };
}

// fill missing fields without touching set ones, so older settings files and
// files written before this page existed load into a complete object
function zeroHandOffsets(): HandOffsets {
  return {
    rotationOffsetDeg: { x: 0, y: 0, z: 0 },
    positionOffsetCm: { x: 0, y: 0, z: 0 },
  };
}

function defaultControllers(): ControllersConfig {
  // vendor builds ship the passthrough-measured asymmetric pose residual
  // (mirrored per hand); must match the driver's Config.h vendor defaults
  if (vendor === 'galaxyxr') {
    return {
      rotationOffsetDeg: { x: 0, y: 5, z: 0 },
      positionOffsetCm: { x: 0.5, y: 0, z: 0 },
      mirrorOffsetsForRightHand: true,
      left: zeroHandOffsets(),
      right: zeroHandOffsets(),
      aligner: { enable: false },
    };
  }
  return {
    rotationOffsetDeg: { x: 0, y: 0, z: 0 },
    positionOffsetCm: { x: 0, y: 0, z: 0 },
    mirrorOffsetsForRightHand: false,
    left: zeroHandOffsets(),
    right: zeroHandOffsets(),
    aligner: { enable: false },
  };
}

function fillDefaults(target: any, defaults: any): any {
  if (target === undefined || target === null) {
    return JSON.parse(JSON.stringify(defaults));
  }
  if (typeof defaults === 'object' && defaults !== null && !Array.isArray(defaults) && typeof target === 'object') {
    for (const key of Object.keys(defaults)) {
      target[key] = fillDefaults(target[key], defaults[key]);
    }
  }
  return target;
}

@Component({
  selector: 'app-stream-frame',
  imports: [
    CommonModule,
    FormsModule,
    MatSlideToggleModule,
    MatSliderModule,
    MatSelectModule,
    MatButtonModule,
    MatIconModule,
    MatInputModule,
    FieldTipComponent,
    ResetButtonComponent,
    StreamFrameCurveComponent,
    DriverEnableBannerComponent
  ],
  templateUrl: './stream-frame.component.html',
  styleUrl: './stream-frame.component.scss'
})
export class StreamFrameComponent {
  dss = inject(DriverSettingService);
  dis = inject(DriverInfoService);

  rootSetting?: Settings;
  controllerSettings?: ControllersConfig;
  controllerDefaults: ControllersConfig = defaultControllers();
  settings?: StreamFrameConfig;
  defaults: StreamFrameConfig = defaultStreamFrame();
  // bumped on every edit so the curve component redraws immediately
  revision = signal(0);
  // friendly band layout inputs; the driver consumes the raw bands array,
  // these three regenerate it evenly spaced on change
  tuneBandCount = 7;
  segLayoutText = '';
  tuneBandFirst = 0.15;
  tuneBandLast = 0.65;
  matrixText = signal('');
  matrixError = signal('');

  constructor() {
    // driver-published defaults can arrive AFTER the last settings emission
    // (info file poll race), leaving this.defaults stuck on the TS literals;
    // reset arrows then write stale values (field 2026-08-11: a reset wrote
    // pre-campaign A=40/P=2.0/O=0.5 over the ratified tuning and poisoned a
    // capture). values() is a signal, so this effect re-runs on every info
    // update and the literals become a true last-resort fallback only.
    effect(() => {
      const infoDefaults = (this.dis.values()?.defaultSettings as any)?.streamFrame;
      this.defaults = fillDefaults(infoDefaults ? JSON.parse(JSON.stringify(infoDefaults)) : undefined, defaultStreamFrame());
    });
    effect(() => {
      this.rootSetting = this.dss.values();
      if (this.rootSetting) {
        // schema-2 migration (2026-08-15), on the RAW stored object BEFORE
        // fillDefaults so absent keys are distinguishable from explicit old
        // defaults. mirrors the driver-side migration; this side persists
        // it. only exact-old-default configs are upgraded - custom tuning
        // and deliberate mode choices pass through untouched.
        const rawSf: any = this.rootSetting.streamFrame;
        if (rawSf && (rawSf.streamFrameSchema ?? 1) < 2) {
          const cvDef = (rawSf.kalmanProcessAccel ?? 1) === 1 && (rawSf.kalmanPosNoiseMm ?? 2.7) === 2.7
            && (rawSf.kalmanProcessAngAccel ?? 400) === 400 && (rawSf.kalmanOriNoiseDeg ?? 1.25) === 1.25;
          const caOld = rawSf.kalmanCaJerk === 10 && (rawSf.kalmanCaAngJerk ?? 1500) === 1500
            && rawSf.kalmanCaPosNoiseMm === 4.2 && rawSf.kalmanCaOriNoiseDeg === 1.25;
          if (rawSf.velocityFixMode === 'kalman' && cvDef) {
            rawSf.velocityFixMode = 'kalmanCA';
          } else if ((rawSf.velocityFixMode === 'kalmanCAM' || rawSf.velocityFixMode === 'kalmanCA') && caOld) {
            rawSf.velocityFixMode = 'kalmanCA';
            rawSf.kalmanCaJerk = 17;
            rawSf.kalmanCaPosNoiseMm = 5.7;
            rawSf.kalmanCaOriNoiseDeg = 5.75;
          }
          rawSf.streamFrameSchema = 2;
          queueMicrotask(() => this.save());
        }
        // schema-3 migration (2026-08-16): schema-2 ratified CA tuning ->
        // new ratified defaults. chains after the schema-2 block so a
        // schema-1 config upgraded above matches the pattern here too.
        if (rawSf && (rawSf.streamFrameSchema ?? 1) < 3) {
          const caS2 = rawSf.kalmanCaJerk === 17 && (rawSf.kalmanCaAngJerk ?? 1500) === 1500
            && rawSf.kalmanCaPosNoiseMm === 5.7 && rawSf.kalmanCaOriNoiseDeg === 5.75
            && (rawSf.kalmanCaAccelTauMs ?? 150) === 150 && !(rawSf.kalmanCaExactCov ?? false);
          if (rawSf.velocityFixMode === 'kalmanCA' && caS2) {
            rawSf.kalmanCaJerk = 4;
            rawSf.kalmanCaPosNoiseMm = 1.5;
            rawSf.kalmanCaOriNoiseDeg = 1.5;
            rawSf.kalmanCaAccelTauMs = 20;
            rawSf.kalmanCaExactCov = true;
          }
          rawSf.streamFrameSchema = 3;
          queueMicrotask(() => this.save());
        }
        // schema-4 migration (2026-08-25, 1.0.0): UNCONDITIONAL. the angular
        // velocity frame fix (kalmanAngularOutFrame) invalidated every
        // Direction Lead tuning - under the corrected frame any non-zero Td
        // bends throws off target - and retired Freeze Coast Turn. unlike
        // schema 2/3 this does not check for old defaults: custom values are
        // reset too, on purpose. mirrors the driver-side migration.
        if (rawSf && (rawSf.streamFrameSchema ?? 1) < 4) {
          rawSf.kalmanDirLeadMs = 0;
          rawSf.kalmanFreezeCoastTurn = 0;
          // controller offsets from the previous release were measured
          // against the old grip origin, which moved in 1.0.0 (grip
          // convention + official pose components). they no longer mean
          // anything in the new frame, so they go back to the shipped
          // defaults. per-hand trims did not exist before and are left.
          const dc = defaultControllers();
          const rc = this.rootSetting.controllers;
          if (rc) {
            rc.rotationOffsetDeg = dc.rotationOffsetDeg;
            rc.positionOffsetCm = dc.positionOffsetCm;
            rc.mirrorOffsetsForRightHand = dc.mirrorOffsetsForRightHand;
          }
          rawSf.streamFrameSchema = 4;
          queueMicrotask(() => this.save());
        }
        // kalmanAngularOutFrame is an int in the driver (0 world / 1 body /
        // 2 zero) and a string enum here; older driver builds published the
        // int, and a reset against that wrote the int back into settings.
        // normalise both so the select renders and the reset arrow agrees.
        const frameNames: { [k: number]: string } = { 0: 'world', 1: 'body', 2: 'zero' };
        if (typeof this.defaults.kalmanAngularOutFrame === 'number') {
          this.defaults.kalmanAngularOutFrame = frameNames[this.defaults.kalmanAngularOutFrame as any] ?? 'body';
        }
        if (rawSf && typeof rawSf.kalmanAngularOutFrame === 'number') {
          rawSf.kalmanAngularOutFrame = frameNames[rawSf.kalmanAngularOutFrame] ?? 'body';
          queueMicrotask(() => this.save());
        }
        this.rootSetting.streamFrame = fillDefaults(this.rootSetting.streamFrame, defaultStreamFrame());
        this.rootSetting.controllers = fillDefaults(this.rootSetting.controllers, defaultControllers());
        this.controllerSettings = this.rootSetting.controllers;
        if (this.controllerSettings && this.controllerSettings.mirrorOffsetsForRightHand === undefined) {
          this.controllerSettings.mirrorOffsetsForRightHand = false;
        }
        if (this.controllerSettings) {
          for (const hand of ['left', 'right'] as const) {
            if (!this.controllerSettings[hand]) {
              this.controllerSettings[hand] = zeroHandOffsets();
            }
          }
        }
        this.settings = this.rootSetting.streamFrame;
        this.matrixText.set((this.settings?.srgbMatrix ?? []).join(', '));
        const bands = this.settings?.distortion?.tune?.bands;
        if (bands && bands.length > 0) {
          this.tuneBandCount = bands.length;
          this.tuneBandFirst = bands[0];
          this.tuneBandLast = bands[bands.length - 1];
        }
        const segLayout = this.settings?.distortion?.tune?.segmentLayout;
        this.segLayoutText = Array.isArray(segLayout) ? segLayout.join(', ') : '';
      }
      const infoDefaults = (this.dis.values()?.defaultSettings as any)?.streamFrame;
      this.defaults = fillDefaults(infoDefaults ? JSON.parse(JSON.stringify(infoDefaults)) : undefined, defaultStreamFrame());
      this.revision.update(x => x + 1);
    });
  }

  // custom shader also applying color to streamed frames while the dashboard is
  // open causes double application and the "works only with dashboard" confusion
  customShaderConflict(): boolean {
    const cs = this.rootSetting?.customShader;
    const sf = this.settings;
    if (!cs || !sf || !sf.enable) return false;
    if (!cs.enable || !cs.enableForOther) return false;
    if (sf.skipColorWhileDashboardOpen) return false;
    return (cs as any).saturation !== 50 || cs.contrast !== 50;
  }

  handOffsetsDirty(hand: 'left' | 'right'): boolean {
    const h = this.controllerSettings?.[hand];
    if (!h) return false;
    return ['x', 'y', 'z'].some(a => (h.rotationOffsetDeg as any)[a] !== 0 || (h.positionOffsetCm as any)[a] !== 0);
  }

  resetHandOffsets(hand: 'left' | 'right') {
    if (this.controllerSettings) {
      this.controllerSettings[hand] = zeroHandOffsets();
      this.save();
    }
  }

  resetControllers(group: keyof ControllersConfig) {
    if (this.controllerSettings) {
      this.controllerSettings[group] = JSON.parse(JSON.stringify(this.controllerDefaults[group]));
      this.save();
    }
  }

  // regenerate the tuner band array evenly spaced from the three layout inputs
  updateSegLayout() {
    if (!this.settings) return;
    const parts = this.segLayoutText.split(/[\s,;]+/).filter(x => x.length > 0);
    const layout: number[] = [];
    for (const part of parts) {
      let v = Math.round(Number(part));
      if (!(v >= 1)) v = 1;
      if (v > 32) v = 32;
      layout.push(v);
    }
    this.settings.distortion.tune.segmentLayout = layout;
    this.save();
  }

  updateBands() {
    if (!this.settings) return;
    let count = Math.round(this.tuneBandCount);
    if (!(count >= 2)) count = 2;
    if (count > 12) count = 12;
    let first = this.tuneBandFirst;
    let last = this.tuneBandLast;
    if (!(first > 0.02)) first = 0.02;
    if (!(last > first)) last = first + 0.05;
    if (last > 1.2) last = 1.2;
    const bands: number[] = [];
    for (let i = 0; i < count; i++) {
      bands.push(Math.round((first + (last - first) * i / (count - 1)) * 1000) / 1000);
    }
    this.tuneBandCount = count;
    this.tuneBandFirst = first;
    this.tuneBandLast = last;
    this.settings.distortion.tune.bands = bands;
    this.save();
  }

  // Galaxy XR native identity (vendor builds only; page hides it otherwise)
  vendor = vendor;
  get galaxyXr(): GalaxyXrConfig {
    if (this.rootSetting) {
      if (!this.rootSetting.galaxyXr) {
        this.rootSetting.galaxyXr = { nativeIdentity: false, nativeInputProfile: false, nativeResolution: true, streamQuality: 'default', renderModelScale: 1.15 };
      }
      if (this.rootSetting.galaxyXr.nativeResolution === undefined) {
        this.rootSetting.galaxyXr.nativeResolution = true;
      }
      if (this.rootSetting.galaxyXr.streamQuality === undefined) {
        this.rootSetting.galaxyXr.streamQuality = 'default';
      }
      if (this.rootSetting.galaxyXr.renderModelScale === undefined) {
        this.rootSetting.galaxyXr.renderModelScale = 1.15;
      }
      if (this.rootSetting.galaxyXr.skeletonOffsetXCm === undefined) {
        this.rootSetting.galaxyXr.skeletonOffsetXCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.skeletonOffsetYCm === undefined) {
        this.rootSetting.galaxyXr.skeletonOffsetYCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.skeletonOffsetZCm === undefined) {
        this.rootSetting.galaxyXr.skeletonOffsetZCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.handAnchorXCm === undefined) {
        this.rootSetting.galaxyXr.handAnchorXCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.handAnchorYCm === undefined) {
        this.rootSetting.galaxyXr.handAnchorYCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.handAnchorZCm === undefined) {
        this.rootSetting.galaxyXr.handAnchorZCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.handAnchorPitchDeg === undefined) {
        this.rootSetting.galaxyXr.handAnchorPitchDeg = 0.0;
      }
      if (this.rootSetting.galaxyXr.handAnchorYawDeg === undefined) {
        this.rootSetting.galaxyXr.handAnchorYawDeg = 0.0;
      }
      if (this.rootSetting.galaxyXr.handAnchorRollDeg === undefined) {
        this.rootSetting.galaxyXr.handAnchorRollDeg = 0.0;
      }
      if (this.rootSetting.galaxyXr.meshOffsetXCm === undefined) {
        this.rootSetting.galaxyXr.meshOffsetXCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.meshOffsetYCm === undefined) {
        this.rootSetting.galaxyXr.meshOffsetYCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.meshOffsetZCm === undefined) {
        this.rootSetting.galaxyXr.meshOffsetZCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.officialComponents === undefined) {
        this.rootSetting.galaxyXr.officialComponents = true;
      }
      if (this.rootSetting.galaxyXr.gripConvention === undefined) {
        this.rootSetting.galaxyXr.gripConvention = true;
      }
      if (this.rootSetting.galaxyXr.controllerBypass === undefined) {
        this.rootSetting.galaxyXr.controllerBypass = false;
      }
      if (this.rootSetting.galaxyXr.customEncodeWidth === undefined) {
        this.rootSetting.galaxyXr.customEncodeWidth = 3072;
      }
      if (this.rootSetting.galaxyXr.customStreamFormatWidth === undefined) {
        this.rootSetting.galaxyXr.customStreamFormatWidth = 3072;
      }
      if (this.rootSetting.galaxyXr.customBandwidthMbit === undefined) {
        this.rootSetting.galaxyXr.customBandwidthMbit = 350;
      }
      if (this.rootSetting.galaxyXr.simulateTouch === undefined) {
        this.rootSetting.galaxyXr.simulateTouch = false;
      }
      if (this.rootSetting.galaxyXr.aimTrimXCm === undefined) {
        this.rootSetting.galaxyXr.aimTrimXCm = 0.0;
      }
      if (this.rootSetting.galaxyXr.aimTrimYCm === undefined) {
        this.rootSetting.galaxyXr.aimTrimYCm = -1.0;
      }
      if (this.rootSetting.galaxyXr.aimTrimZCm === undefined) {
        this.rootSetting.galaxyXr.aimTrimZCm = 1.0;
      }
      if (this.rootSetting.galaxyXr.componentRebaseIncludeTrim === undefined) {
        this.rootSetting.galaxyXr.componentRebaseIncludeTrim = true;
      }
      return this.rootSetting.galaxyXr;
    }
    return { nativeIdentity: false };
  }

  save() {
    if (this.rootSetting) {
      this.dss.save(this.rootSetting);
    }
    this.revision.update(x => x + 1);
  }

  alignmentDirty(): boolean {
    if (!this.settings) return false;
    const a = this.settings.alignment, d = this.defaults.alignment;
    return a.leftH != d.leftH || a.leftV != d.leftV || a.rightH != d.rightH || a.rightV != d.rightV;
  }

  velocityFixTip = 'Off: pass the native runtime velocities through untouched. Kalman: a single estimator produces position, rotation, velocity and spin as one coherent state, the same architecture native tracked controllers use. Kalman CA (recommended): A constant-acceleration variant that tracks the throw ramp itself instead of rescaling it away. Replaces the whole estimator.';
  velocityFixTipFull = 'Off: pass the native runtime velocities through untouched. Kalman: a single estimator produces position, rotation, velocity and spin as one coherent state, the same architecture native tracked controllers use. Kalman CA (recommended): A constant-acceleration variant that tracks the throw ramp itself instead of rescaling it away. Replaces the whole estimator. Graveyard modes - Classic/Full: first-generation fixes, superseded. Derive: the legacy pose-derivation pipeline; retired after field testing, kept intact for reproducibility. Kalman CA Magnitude: transitional CA variant that swapped only the throw-strength channel; superseded by CA Full (retired 2026-08-15).';

  resetGraveyard() {
    if (!this.settings) return;
    const archived: (keyof StreamFrameConfig)[] = ['deriveDirSource', 'deriveDirWeightPow', 'deriveDirWindowMs', 'deriveLatchAngMinSpeed', 'deriveLatchHoldMs', 'deriveLatchMinSpeed', 'deriveLatchWindowMs', 'deriveMagSource', 'derivePreFilter', 'derivePreSmoothMs', 'derivePreSmoothScope', 'deriveReleaseLatch', 'deriveSmoothAngSeparate', 'deriveSmoothAngSpeedHigh', 'deriveSmoothAngSpeedLow', 'deriveSmoothAngTauFastMs', 'deriveSmoothAngTauSlowMs', 'deriveSmoothSpeedHigh', 'deriveSmoothSpeedLow', 'deriveSmoothTauFastMs', 'deriveSmoothTauSlowMs', 'deriveSplitDirAngular', 'deriveSplitDirLinear', 'kalmanAngDirSmoothMs', 'kalmanDirSmoothMs', 'kalmanDupCoastMaxMs', 'kalmanGazeAssist', 'kalmanGazeMaxDeg', 'kalmanGazeMinSpeed', 'kalmanReleaseRewindMs', 'kalmanRewindHoldMs', 'kalmanSmoothLagMs', 'nvencTap', 'zeroCopyV3'];
    for (const k of archived) { (this.settings as any)[k] = JSON.parse(JSON.stringify((this.defaults as any)[k])); }
    // FOV tangents live inside eyeGaze; reset only those subkeys so
    // gaze prediction is untouched
    this.settings.eyeGaze.tanHalfFovX = this.defaults.eyeGaze.tanHalfFovX;
    this.settings.eyeGaze.tanHalfFovY = this.defaults.eyeGaze.tanHalfFovY;
    this.save();
  }

  reset(key: keyof StreamFrameConfig) {
    if (!this.settings) return;
    (this.settings as any)[key] = JSON.parse(JSON.stringify((this.defaults as any)[key]));
    if (key === 'srgbMatrix') {
      this.matrixText.set(this.settings.srgbMatrix.join(', '));
      this.matrixError.set('');
    }
    this.save();
  }

  // ---- distortion profile sharing ----
  shareText = signal('');
  shareStatus = signal('');
  // collapsible section state; debug starts closed, everything else open.
  // concrete shape (no index signature) so strict templates allow dot access
  sections = {
    headset: true, controllers: true, ctrlFix: true, kalmanAdv: false, ctrlAdv: false, ctrlOffsets: true, tipOffset: false,
    processing: true, color: true, enhance: true, distortion: true, eyeAlign: false, share: false,
    advanced: false, debug: false, graveyard: false,
  };
  // any calibration overlay/mode that would be visible or disruptive in a
  // normal play session — drives the warning banner at the top of the page
  // the CA experiment modes share the mode-4 machinery (dup handling,
  // device time), so those rows show for any kalman-family mode
  retiredVelocityModes: string[] = ['classic', 'full', 'derive', 'kalmanCAM'];
  retiredVelocityModeLabels: { [k: string]: string } = {
    classic: 'Classic (legacy)',
    full: 'Full (legacy)',
    derive: 'Derive (legacy, retired)',
    kalmanCAM: 'Kalman CA \u2014 Magnitude (retired)',
  };
  // a stored graveyarded mode still renders (as the sole extra option)
  // when the graveyard is hidden, so old configs never break
  isRetiredVelocityMode(m: string | undefined): boolean {
    return !!m && this.retiredVelocityModes.includes(m);
  }

  isKalmanMode(): boolean {
    const m = this.settings?.velocityFixMode;
    return m == 'kalman' || m == 'kalmanCAM' || m == 'kalmanCA';
  }
  calibrationActive(): boolean {
    const s = this.settings;
    const c = this.controllerSettings;
    if (!s) return false;
    return !!(s.distortion?.tune?.enable || s.distortion?.centerTune?.enable
      || c?.aligner?.enable || s.eyeGaze?.probeCapture || s.eyeGaze?.debugGrid
      || s.eyeGaze?.calibDot || s.eyeGaze?.debugRing || s.eyeGaze?.overlayWarped
      || s.eyeGaze?.swimProbe || s.calib?.blackout);
  }
  // calib is normally written by the camera tools; the GUI only exposes
  // blackout, so create the object lazily with the driver's defaults
  setBlackout(on: boolean) {
    if (!this.settings) return;
    if (!this.settings.calib) {
      this.settings.calib = { blackout: false, eye: -1, patternBrightness: 1, captureMode: false, pattern: -1, patternBits: 10 };
    }
    this.settings.calib.blackout = on;
    this.save();
  }
  private buildProfile(): any {
    const s = this.settings!;
    const profile: any = {
      type: 'streamFrameDistortionProfile',
      version: 1,
      name: 'My Galaxy XR profile',
      distortion: JSON.parse(JSON.stringify(s.distortion)),
      k1: s.k1,
      k2: s.k2,
      centerOffsetXLeft: s.centerOffsetXLeft,
      centerOffsetXRight: s.centerOffsetXRight,
      centerOffsetY: s.centerOffsetY
    };
    // the annulus and tuners are tuning diagnostics, not part of a shareable profile
    delete profile.distortion.annulus;
    delete profile.distortion.tune;
    delete profile.distortion.centerTune;
    return profile;
  }
  exportJsonFile() {
    if (!this.settings) return;
    const text = JSON.stringify(this.buildProfile(), null, 2);
    const blob = new Blob([text], { type: 'application/json' });
    const anchor = document.createElement('a');
    anchor.href = URL.createObjectURL(blob);
    const stamp = new Date().toISOString().slice(0, 10);
    anchor.download = 'gxr-distortion-profile-' + stamp + '.json';
    anchor.click();
    URL.revokeObjectURL(anchor.href);
    this.shareStatus.set('Profile downloaded as ' + anchor.download);
  }
  importJsonFile(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) return;
    file.text().then(text => {
      this.shareText.set(text);
      this.importProfile();
      input.value = '';
    });
  }
  exportProfile() {
    if (!this.settings) return;
    const s = this.settings;
    const profile = {
      type: 'streamFrameDistortionProfile',
      version: 1,
      name: 'My Galaxy XR profile',
      distortion: JSON.parse(JSON.stringify(s.distortion)),
      k1: s.k1,
      k2: s.k2,
      centerOffsetXLeft: s.centerOffsetXLeft,
      centerOffsetXRight: s.centerOffsetXRight,
      centerOffsetY: s.centerOffsetY
    };
    // the annulus and tuner are tuning diagnostics, not part of a shareable profile
    delete profile.distortion.annulus;
    delete profile.distortion.tune;
    delete profile.distortion.centerTune;
    const text = JSON.stringify(profile, null, 2);
    this.shareText.set(text);
    this.shareStatus.set('Profile exported below. Copy it anywhere.');
    navigator.clipboard?.writeText(text).then(
      () => this.shareStatus.set('Profile copied to clipboard.'),
      () => {}
    );
  }
  importProfile() {
    if (!this.settings) return;
    let parsed: any;
    try {
      parsed = JSON.parse(this.shareText());
    } catch (e: any) {
      this.shareStatus.set('Not valid JSON: ' + e.message);
      return;
    }
    // controller-aligner save files ({"controllers": {...}}) import here too,
    // applying straight into the offset fields below
    const controllers = parsed?.controllers;
    if (controllers && (controllers.rotationOffsetDeg || controllers.positionOffsetCm) && this.controllerSettings) {
      const applyAxes = (target: { x: number, y: number, z: number }, source: any) => {
        for (const axis of ['x', 'y', 'z'] as const) {
          if (Number.isFinite(source?.[axis])) target[axis] = source[axis];
        }
      };
      applyAxes(this.controllerSettings.rotationOffsetDeg, controllers.rotationOffsetDeg);
      applyAxes(this.controllerSettings.positionOffsetCm, controllers.positionOffsetCm);
      this.save();
      this.shareStatus.set('Controller offsets imported and applied.');
      return;
    }
    if (parsed?.type !== 'streamFrameDistortionProfile' || typeof parsed.distortion !== 'object') {
      this.shareStatus.set('Not a stream frame distortion profile.');
      return;
    }
    const num = (v: any, fallback: number) => (Number.isFinite(v) ? v : fallback);
    const s = this.settings;
    // center-tuner saves apply ONLY the center offsets: importing a stale
    // centers file must never roll the curves back to its embedded snapshot
    if (parsed.centersOnly) {
      s.centerOffsetXLeft = num(parsed.centerOffsetXLeft, s.centerOffsetXLeft);
      s.centerOffsetXRight = num(parsed.centerOffsetXRight, s.centerOffsetXRight);
      s.centerOffsetY = num(parsed.centerOffsetY, s.centerOffsetY);
      this.save();
      this.revision.update(v => v + 1);
      this.shareStatus.set('Center offsets imported and applied (curves untouched).');
      return;
    }
    const d = parsed.distortion;
    s.distortion.mode = d.mode === 'spline' ? 'spline' : 'k1k2';
    s.distortion.perEye = !!d.perEye;
    s.distortion.perAxis = !!d.perAxis;
    s.distortion.segments = (Number.isInteger(d.segments) && d.segments >= 2 && d.segments <= 32) ? d.segments : 1;
    if (Array.isArray(parsed.tuneSegmentLayout)) {
      s.distortion.tune.segmentLayout = parsed.tuneSegmentLayout
        .map((v: any) => Math.round(Number(v)))
        .filter((v: number) => v >= 1 && v <= 32);
    }
    const parsePoints = (arr: any) => Array.isArray(arr)
      ? arr.filter((p: any) => Number.isFinite(p?.r) && Number.isFinite(p?.scale)).map((p: any) => ({ r: p.r, scale: p.scale }))
      : [];
    s.distortion.points = parsePoints(d.points);
    s.distortion.curves = {};
    if (d.curves && typeof d.curves === 'object') {
      for (const key of Object.keys(d.curves)) {
        const c = d.curves[key];
        s.distortion.curves[key] = { k1: num(c?.k1, 0), k2: num(c?.k2, 0), points: parsePoints(c?.points) };
      }
    }
    // dense displacement map (version 2 profiles, camera calibrated).
    // absent = the profile is radial only, so any previous map is dropped
    // (a profile is a complete correction, not a patch)
    const m = d.map;
    const eyeArray = (arr: any, len: number) => Array.isArray(arr) && arr.length === len && arr.every((v: any) => Number.isFinite(v))
      ? arr.map((v: any) => Number(v)) : [];
    if (m && Number.isInteger(m.cols) && Number.isInteger(m.rows) && m.cols >= 2 && m.rows >= 2) {
      const len = m.cols * m.rows * 2;
      s.distortion.map = {
        enable: m.enable !== false,
        cols: m.cols,
        rows: m.rows,
        left: eyeArray(m.left, len),
        right: eyeArray(m.right, len),
        source: typeof m.source === 'string' ? m.source : '',
      };
    } else {
      delete s.distortion.map;
    }
    s.k1 = num(parsed.k1, 0);
    s.k2 = num(parsed.k2, 0);
    s.centerOffsetXLeft = num(parsed.centerOffsetXLeft, 0);
    s.centerOffsetXRight = num(parsed.centerOffsetXRight, 0);
    s.centerOffsetY = num(parsed.centerOffsetY, 0);
    this.save();
    this.shareStatus.set('Profile applied' + (parsed.name ? ': ' + parsed.name : '.'));
  }

  onMatrixTextChanged(text: string) {
    this.matrixText.set(text);
    if (!this.settings) return;
    const trimmed = text.trim();
    if (trimmed === '') {
      this.settings.srgbMatrix = [];
      this.matrixError.set('');
      this.save();
      return;
    }
    const values = trimmed.split(/[\s,;]+/).map(Number);
    if (values.length === 9 && values.every(x => Number.isFinite(x))) {
      this.settings.srgbMatrix = values;
      this.matrixError.set('');
      this.save();
    } else {
      this.matrixError.set('Needs exactly 9 numbers (row major 3x3), or empty to disable');
    }
  }
}
