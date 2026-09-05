#pragma once

#include <set>
#include <map>
#include <vector>
#include <mutex>
#include <string>
#include <atomic>

#include "openvr_driver.h"

class ShimDefinition;

#define VREvent_VendorSpecific_ContextCollection (vr::EVREventType)(vr::VREvent_VendorSpecific_Reserved_Start + 5872)
#define VREvent_VendorSpecific_ContextCollection_MagicDataNumber 32643216579172981


class CustomHeadsetDeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError Init(vr::IVRDriverContext *pDriverContext) override;
	const char *const *GetInterfaceVersions() override;
	
	// called by the main loop of the server
	void RunFrame() override;
	// deprecated function, but still must be defined
	bool ShouldBlockStandbyMode() override;
	// SteamVR is entering/leaving standby mode
	void EnterStandby() override;
	void LeaveStandby() override;
	// cleanup on exit
	void Cleanup() override;
	
	// handle hook of TrackedDevicePoseUpdated
	bool HandleDevicePoseUpdated(uint32_t openVRID, vr::DriverPose_t &pose);
	// handle hook of TrackedDeviceAdded
	bool HandleDeviceAdded(const char* &pchDeviceSerialNumber, vr::ETrackedDeviceClass &eDeviceClass, vr::ITrackedDeviceServerDriver* &pDriver);
	// set of driver conexts collected by the hooking process
	std::set<vr::IVRDriverContext*> driverContexts = {};
	// map of driver contexts by device id
	// this is populated by VREvent_VendorSpecific_ContextCollection events
	std::map<uint32_t, vr::IVRDriverContext*> driverContextsByDeviceId = {};
	// sends out VREvent_VendorSpecific_ContextCollection events for a given device id
	// after some time, the driverContextsByDeviceId map should be contain the context for this device
	void SendContextCollectionEvents(uint32_t id);
	// attempt to send the event if the context is available, returns true if successful.
	// if false was returned the message has queued to be sent if the driver context can be found
	// events must be sent from the context that owns the device, so this is necessary
	bool SendVendorEvent(uint32_t unWhichDevice, vr::EVREventType eventType, const vr::VREvent_Data_t & eventData, double eventTimeOffset);
	// a set of all shim objects to manage
	// this allows them to have RunThread called
	std::set<ShimDefinition*> shims;
private:
	struct QueuedEvent {
		vr::EVREventType eventType;
		vr::VREvent_Data_t eventData;
		double eventTimeOffset;
	};
	// events that are waiting for a context to be found
	std::map<uint32_t, std::vector<QueuedEvent>> queuedEvents = {};
	bool customShaderEnabled = false;
	
	// pose logging diagnostic state (streamFrame.poseLogging), per device.
	// pose updates arrive on the source drivers' own threads, hence the lock.
	struct PoseLogState {
		double lastSteadyLog = 0;
		double lastBurstLog = 0;
		// peak linear speed observed since the last steady log line
		double peakSpeed = 0;
		// serial announced once on first sight (maps openVRID -> device)
		bool announced = false;
		// tracking state transition + post throw window logging
		bool haveTrackState = false;
		bool lastLoggedValid = false;
		int lastLoggedResult = 0;
		double recentFastTime = 0;
		// finite difference velocity from positions, to compare against the
		// velocity the driver reports (suspected ~2 m/s clamp in vrlink)
		bool havePos = false;
		double lastPos[3] = {0, 0, 0};
		double lastSampleTime = 0;
		double fdSpeedEma = 0;
		// quaternion-derived angular speed, same idea as fdSpeed: compare
		// against the driver's reported |w| to see if angular velocity is
		// smoothed the same way linear velocity is
		bool haveQuat = false;
		vr::HmdQuaternion_t lastQuat = {1, 0, 0, 0};
		double fdAngSpeedEma = 0;
	};
	std::map<uint32_t, PoseLogState> poseLogStates = {};
	std::mutex poseLogLock = {};
	// one-shot per-device announcement of a non-identity WorldFromDriver
	// (mixed-space setups); lock free for the pose hot path
	std::atomic<uint64_t> spaceFixLoggedMask{0};
	void LogDevicePose(uint32_t openVRID, const vr::DriverPose_t &pose);
	
	// throw/velocity fix state: short ring of recent positions per device,
	// used to recompute linear velocity over a ~50ms window (endpoint
	// difference across the ring rejects sample-to-sample jitter that a
	// plain adjacent diff amplifies). guarded by poseLogLock.
	struct VelFixState {
		static constexpr int ringSize = 8;
		double pos[ringSize][3] = {};
		// input prefilter history: last raw positions for the per-axis
		// median-of-3 that feeds the ring when derivePreFilter is on
		// (kills single-sample network spikes before the fit AND secant)
		double rawPos[3][3] = {};
		int rawCount = 0;
		// pre-smoothed parallel streams (EMA, derivePreSmoothMs): the
		// secant reads these when smoothing is on; the fit also reads the
		// smoothed positions when scope=both
		double smPos[ringSize][3] = {};
		vr::HmdQuaternion_t smQuat[ringSize] = {};
		double smPosEma[3] = {};
		vr::HmdQuaternion_t smQuatEma = {1, 0, 0, 0};
		bool haveSm = false;
		double time[ringSize] = {};
		int count = 0;   // valid entries
		int head = 0;    // next write slot
		// orientation alongside position, for angular velocity derivation
		vr::HmdQuaternion_t quat[ringSize] = {};
		// EMA over the least squares slope, so the substituted velocity is
		// smooth in time (discontinuities here become rendered pose jumps
		// through the runtime's forward prediction)
		bool haveEma = false;
		double emaVel[3] = {};
		bool haveEmaAng = false;
		double emaAng[3] = {};
		// joint peak hold: v and w captured at the most recent linear speed
		// peak, replayed with decay for ~90ms so a game sampling just after
		// release reads the intended throw instead of the hand's snap back
		double peakVel[3] = {};
		double peakAng[3] = {};
		double peakSpeed = 0;
		double peakTime = 0;
		// previous output speed, for peak plausibility (a single spiked
		// sample must not become a held peak)
		double lastOutSpeed = 0;
		double lastOutTime = 0;
		// hold is latched by violent deceleration and stays engaged until
		// the decay window ends or a new peak latches
		bool holdActive = false;
		// release gesture anchor (v5): set when the grip/trigger scalar
		// starts FALLING from its held plateau — the biomechanical moment
		// the hand begins letting go, which precedes every game's release
		// threshold. during the anchor window the output ratchets up with
		// rising motion and freezes against falling motion, so whatever
		// instant the game samples, it reads the throw's peak.
		double anchorTime = 0;
		bool anchorHasValue = false;
		double anchorVel[3] = {};
		double anchorAng[3] = {};
		double anchorSpeed = 0;
	};
	// pose component tracking (ET hunt: gaze may be published as a pose
	// component; log creates and throttle updates from the HMD container)
	struct PoseComponentInfo {
		vr::PropertyContainerHandle_t container = 0;
		std::string name;
		uint64_t updates = 0;
		double lastLogTime = 0;
	};
	std::map<vr::VRInputComponentHandle_t, PoseComponentInfo> poseComponents = {};
public:
	void AnchorReleaseGesture(uint32_t openVRID);
private:
	std::map<uint32_t, VelFixState> velFixStates = {};
	// returns true and writes the derived velocity when the window is usable
	bool DeriveMotion(uint32_t openVRID, const vr::DriverPose_t &pose, double derivedVel[3], double derivedAng[3], double secantVel[3], double secantAng[3]);
	static void ComputeRingSecant(const VelFixState &state, bool useSmoothed, double secantVel[3], double secantAng[3]);
	// cached device classes (Prop_DeviceClass_Int32), resolved on first pose
	std::map<uint32_t, int> deviceClasses = {};
	// streamed-controller identity cache (serial prefix VRLINK*/SamsungVST*
	// = vrlink device). the velocity fix must never touch lighthouse
	// devices: their native velocity is correct and mixed sessions
	// (knuckles + playspace override) are a supported setup. queried once
	// per id OUTSIDE any lock, then cached.
	std::map<uint32_t, int> streamedControllerCache = {};
	std::mutex streamedIdentityLock;
	bool IsStreamedController(uint32_t openVRID);
	// derive-mode adaptive smoothing state (pure math under its own lock;
	// never calls out — lock discipline)
	// one coherent estimated kinematic state per controller (kalman mode):
	// per-axis constant-velocity Kalman for position/velocity, quaternion
	// state integrated by the filtered angular velocity and corrected by
	// measurements (MEKF-lite: residual rotation vector drives per-axis
	// CV Kalman filters for the angular channel). guarded by
	// deriveFilterLock; pure math only under the lock.
	struct KalState {
		bool have = false;
		double time = 0;
		double p[3] = {};
		double v[3] = {};
		// per-axis covariance [Ppp, Ppv, Pvv]
		double P[3][3] = {};
		vr::HmdQuaternion_t q = {1, 0, 0, 0};
		double w[3] = {};
		double Pa[3][3] = {}; // angular per-axis covariance
		bool announced = false;
		// telemetry only: EMA of normalized innovation squared (NIS ~ 1
		// when the noise models match reality) + diag log throttle
		double nisEma = 1.0;
		double lastDiagLog = 0;
		// raw input-step telemetry (FOV / tracking artifact watch): max
		// single-step measurement distance and count of near-zero steps
		// while the state was moving, since the last KALDIAG line
		double lastMeas[3] = {};
		// last measured quaternion: the 3dof-fallback discriminator.
		// GxR optical tracking loses POSITION on fast/occluded hands and
		// freezes it while the IMU keeps ORIENTATION live — a frozen
		// position with a moving quaternion is a position-only freeze,
		// NOT stillness, so the dup run cap's rationale does not apply.
		double lastMeasQ[4] = {1, 0, 0, 0};
		// Observe-only angular-space probe.  This deliberately has its own
		// raw quaternion clock rather than reusing lastMeasQ/tFresh: position
		// can freeze while orientation remains live, which is exactly the
		// case this diagnostic is intended to distinguish.
		bool diagRawQHave = false;
		double diagRawQ[4] = {1, 0, 0, 0};
		double diagRawQT = 0;
		double diagSpaceLastLog = 0;
		int posFreeze3dof = 0;
		bool haveMeas = false;
		double stepMax = 0;
		int stepFrozen = 0;
		int dupSkipped = 0;
		// device-time measurement stamping (correctness pass): timestamp
		// of the last ACCEPTED measurement on the device clock
		// (receipt + poseTimeOffset); receipt-time ks.time stays as the
		// legacy clock so the off-toggle reproduces old behavior exactly
		double tMeas = 0;
		// continuous dup-coast tracking for the runaway cap: receipt time
		// the current coast run began, or -1 when not coasting
		double coastStart = -1.0;
		// per-window diag: dropped out-of-order samples, accepted-dt
		// stats (ms), longest continuous coast (ms)
		int dtBack = 0;
		double dtSumMs = 0;
		int dtN = 0;
		double dtMaxMs = 0;
		double coastMaxMs = 0;
		// angular-channel NIS (same EMA treatment as linear nisEma)
		double nisAEma = 1.0;
		// fresh-to-fresh clock: device-time stamp of last DISTINCT raw
		// sample + per-window stats of the tracker's true cadence
		double tFresh = 0;
		double fdtSumMs = 0;
		int fdtN = 0;
		double fdtMaxMs = 0;
		// acceleration field probe (2026-08-12, observe-only): does
		// vrlink populate DriverPose_t vecAcceleration /
		// vecAngularAcceleration? if yes, acceleration as a CONTROL
		// INPUT to the predict step is the first internal door onto
		// the ramp-lag deficit (attacks the 30x model-honesty gap of
		// the throw ramp WITHOUT raising process noise). per-window
		// max magnitudes + count of nonzero samples, measured on the
		// raw stream before any accept/drop decision. zero across a
		// moving session = fields unpopulated, door closed for free.
		double accMax = 0;
		double wAccMax = 0;
		int accNZ = 0;
		// flagged-loss + teleport bookkeeping (2026-08-12): vrlink
		// zero-fills result during hard losses; the estimator gate
		// (raw status) already skips those samples, this records them
		// and pins a clean reinit on reacquire. teleports counts
		// physically impossible accepted steps (unflagged reacquires)
		// converted to reinits by the teleport guard.
		bool lost = false;
		double lossStartT = 0;
		int lossRuns = 0;
		double lossMsSum = 0;
		int teleports = 0;
		// Measurement-integrity reacquisition.  This is deliberately
		// separate from throw/release logic: an impossible position jump is
		// missing/untrusted sensor data, not a motion-model event.
		//
		// reacqCheck: first good-status sample after a short flagged loss
		// must still pass the ordinary teleport-continuity test before it is
		// allowed to innovate the carried state.
		// reacqActive: a discontinuity was found; candidate raw positions are
		// collected without touching the Kalman state until a short coherent
		// trajectory exists.  This prevents the old dt>200ms timer from
		// eventually reinitializing onto a still-walking multi-metre glitch.
		bool reacqCheck = false;
		bool reacqActive = false;
		int reacqCount = 0;
		double reacqFirstT = 0;
		double reacqLastT = 0;
		double reacqFirstP[3] = {};
		double reacqLastP[3] = {};
		// Candidate trajectory proof: enough distinct position samples to
		// estimate one coherent velocity instead of promoting a lucky
		// first/last secant from a walking coordinate solution.
		static constexpr int reacqFitN = 7;
		double reacqT[reacqFitN] = {};
		double reacqP[reacqFitN][3] = {};
		double reacqPrevStepV[3] = {};
		bool reacqHavePrevStepV = false;
		int gazeBends = 0;
		int turnCoastSteps = 0; // coordinated-turn coast steps this diag window
		double gazeBendSum = 0;
		double gazeBendMax = 0;
		// velocity history ring for the release-rewind experiment: ~260ms
		// of (t, v, w) at stream cadence, plus the rewind window armed by
		// the input tap when the experiment is enabled
		static constexpr int histSize = 24;
		double histT[histSize] = {};
		double histV[histSize][3] = {};
		double histW[histSize][3] = {};
		double histP[histSize][3] = {};
		vr::HmdQuaternion_t histQ[histSize] = {};
		int histHead = 0;
		int histCount = 0;
		double rewindUntil = 0;
		double rewindTarget = 0;
		// slow-direction EMA copies for split reporting
		double vSlow[3] = {};
		double wSlow[3] = {};
		bool haveSlow = false;
		// knob echo, so live tuning re-announces in the log (CA knobs
		// carry their own sig: summing them into lastQa would vanish
		// below double epsilon next to the 1e16-scale legacy terms)
		double lastQa = -1;
		double lastCaSig = -1;
		// parallel fast velocity estimator (magnitude channel): per-axis
		// CV kalman over the same measurements with its own accel
		double pF[3] = {};
		double vF[3] = {};
		double PF[3][3] = {};
		bool haveFast = false;
		// mode echo: switching between the CV and CA layouts mid-session
		// forces a clean reinit (the covariance layouts differ)
		int lastMode = 0;
		// constant-acceleration (Singer) states + covariances for the
		// CA experiment modes. covariance layout per axis:
		// [P00 P01 P02 P11 P12 P22] (symmetric upper triangle)
		double ca[3] = {};      // linear acceleration state (CA-full)
		double P6[3][6] = {};   // linear CA covariance (CA-full)
		double caW[3] = {};     // angular acceleration state (CA-full)
		double Pa6[3][6] = {};  // angular CA covariance (CA-full)
		double caF[3] = {};     // fast-channel acceleration state (CA-M)
		double PF6[3][6] = {};  // fast-channel CA covariance (CA-M)
		// per-window peak of the CA acceleration state magnitudes
		// (KALDIAG: watch for phantom accel during coasts/stops)
		double caAccPk = 0;
		double caWAccPk = 0;
		// STUCKDIAG state-vs-measurement divergence watchdog (2026-08-14):
		// the stuck-hand adjudicator. |state p - measurement| > 0.25m
		// opens a run, closing under 0.10m logs duration + max + entry
		// speed. a STUCKDIAG line = the FILTER diverged (state momentum
		// gliding past a frozen/true measurement); a stuck moment with
		// KALLOSS but no STUCKDIAG = raw-pose passthrough during a
		// flagged tracking loss. the two partition the failure space.
		bool stuckRun = false;
		double stuckStartT = 0;
		double stuckMax = 0;
		double stuckV0 = 0;
		// corrupt-payload gate telemetry (field 2026-08-14, the stuck-hand
		// kill chain): vrlink occasionally delivers POSITION GARBAGE
		// (~4e18m coordinates, bursts at a constant ~2.7ms dt) while
		// flagged Running_OK. before the gate, the teleport guard
		// "handled" these by REINITIALIZING AT THE GARBAGE, then
		// haveMeas=false disarmed the guard for one sample, so the next
		// real measurement innovated across ~1e18m and kicked the
		// velocity state into orbit — the measured lagLin=1264ms /
		// dirOff=92deg post-throw park. garbageN counts rejects per
		// KALDIAG window; garbageRun throttles the burst log to one
		// line per run.
		int garbageN = 0;
		bool garbageRun = false;
		// reported-velocity insanity clamp count (belt and suspenders)
		int vClampN = 0;
		// release-instant direction snapshot (2026-08-15): RELDIAG's
		// relOffPk compares release-vs-peak OUTPUT — both post-shaping,
		// so a direction transform like the derotation lead CANCELS in
		// it and the instrument is blind to Td. these store, per frame,
		// the SHAPED output (what the game reads) and the ring secant
		// (displacement ground truth); at the release edge RELDIAG then
		// scores output-vs-truth at the one instant the game samples —
		// the direction analog of rel/pk, and the Td/O adjudicator.
		bool relSnapHave = false;
		double relOutV[3] = {};
		double relOutW[3] = {};
		double relSecV[3] = {};
		double relSecW[3] = {};
		// adaptive-R scheduler state: FAST EMA (~25ms) of the BASE-R
		// normalized NIS per channel — the control statistic, separate
		// from the slower telemetry nisEma, and normalized against the
		// unadapted R so shrinking R cannot latch the very statistic
		// that shrinks it. divisor window peaks feed KALDIAG so a
		// session can verify the trust ramp engages on whips only.
		double schedNis = 1.0;
		double schedANis = 1.0;
		double rDivPk = 1.0;
		double rADivPk = 1.0;
		// PEAKDIAG per-gesture scorer: peak of the reported output, the
		// calm and magnitude channels and the ring secant (displacement
		// ground truth), plus the direction vectors at each peak
		bool pkActive = false;
		double pkOut = 0, pkSec = 0, pkCalm = 0, pkMag = 0;
		double pkAngOut = 0, pkAngSec = 0;
		double pkOutVec[3] = {};
		double pkSecVec[3] = {};
		// peak TIMES (channel-lag instrument): when each channel's peak
		// happened. lagLin/lagAng = each channel's filter lag vs its own
		// secant; the linear and angular secants share one ring window,
		// so their windowing delay is common-mode and cancels in the
		// lagAng - lagLin mismatch — the number that decides whether the
		// angular channel peaks in phase with the linear one.
		double pkOutT = 0, pkSecT = 0, pkAngOutT = 0, pkAngSecT = 0;
		// this frame's channel speeds, written under deriveFilterLock in
		// the kalman block and read by the scorer after DeriveMotion
		double diagCalmSp = 0;
		double diagMagSp = 0;
		// grip-point compensator: this frame's grip-transported velocity
		// (shadow-computed whenever rGrip is nonzero, reported only when
		// enabled) and the w x r contamination magnitude, plus the
		// per-gesture peaks the PEAKDIAG scorer accumulates from them
		bool diagGripHave = false;
		double diagGripV[3] = {};
		double diagGripWr = 0;
		double pkGrip = 0;
		double pkGripVec[3] = {};
		double pkWr = 0;
		// grip knob echo (own sig: the CA sig's epsilon floor sits at
		// ~1e-2 next to its 1e13-scale terms — cm-resolution grip values
		// would vanish there)
		double lastGripSig = -1;
		// RAW reference ring (instrument correctness pass 2026-08-16):
		// PEAKDIAG/RELDIAG's "secant" comes from DeriveMotion, which in
		// kalman modes is fed the pose AFTER the report block overwrote
		// it with the filtered state — so out/sec, dirOff, relDirOff and
		// lagLin were filtered-vs-filtered comparisons (lagLin ~ 0 and
		// relDirOff growing ~ |w|*Td by construction). this ring holds
		// only FRESH raw measurements (distinct position, device-time
		// stamped) and yields a raw displacement secant attributed to
		// the window center, plus a running peak of that secant that
		// is segmented on the RAW speed itself (not on the lagged
		// output, which would miss the raw peak on short flicks). the
		// raw fields appended to both diag lines and the new SKEW
		// instrument read from here. pure telemetry.
		static constexpr int rawRingN = 5;
		double rawT[rawRingN] = {};   // device clock (tMeas): secant span
		double rawTr[rawRingN] = {};  // receipt clock (now): timing vs edges/output
		double rawP[rawRingN][3] = {};
		vr::HmdQuaternion_t rawQ[rawRingN] = {};
		int rawHead = 0;
		int rawCount = 0;
		// latest raw secant (window center time, linear + angular)
		bool rawSecHave = false;
		double rawSecT = 0;
		double rawSecV[3] = {};
		double rawSecW[3] = {};
		// raw linear peak: segmented on raw secant speed (>1.0 opens,
		// <0.8 closes); values persist after close as "latest raw peak"
		bool rawPkActive = false;
		double rawPkSp = 0;
		double rawPkT = 0;   // receipt-clock window center of the peak
		double rawPkV[3] = {};
		// raw angular peak: own segmentation (>4 rad/s opens, <3 closes)
		bool rawPkWActive = false;
		double rawPkWSp = 0;
		double rawPkWT = 0;  // receipt-clock window center of the peak
		double rawPkW[3] = {};
		// last REPORTED velocities (written in the report block itself,
		// every callback). RELDIAG's raw-referenced fields read these:
		// the derive-branch snapshot (relOutV) is only refreshed when
		// DeriveMotion accepts the frame, which it did not during fast
		// throws (see the teleStep note in DeriveMotion).
		bool repHave = false;
		double repV[3] = {};
		double repW[3] = {};
		// ---- fixed-lag RTS smoother (CA-full only, kalmanSmoothLagMs>0) ----
		// per accepted callback: the Singer step's predicted and filtered
		// (p,v,a) per axis with covariances, the step dt, and the angular
		// state (q, w, wdot) after the step. the report block runs the
		// Rauch-Tung-Striebel backward recursion from the newest entry
		// down to the entry at t-L and reports the smoothed linear state
		// there (velocity accurate AT t-L, but calmer than any causal
		// filter with L of lag, because samples after t-L also vote);
		// the angular state is taken filtered at t-L (its own filter lag
		// is ~7ms, smoothing gains nothing worth the linearization). ring
		// stamps are on the device clock (tMeas) so poseTimeOffset can
		// describe the reported epoch exactly. ~96 x 3ms = ~290ms depth.
		static constexpr int rtsN = 96;
		double rtsT[rtsN] = {};
		double rtsDt[rtsN] = {};
		double rtsXp[rtsN][3][3] = {};
		double rtsPp[rtsN][3][6] = {};
		double rtsXf[rtsN][3][3] = {};
		double rtsPf[rtsN][3][6] = {};
		double rtsW[rtsN][3] = {};
		double rtsWa[rtsN][3] = {};
		vr::HmdQuaternion_t rtsQ[rtsN] = {};
		int rtsHead = 0;
		int rtsCount = 0;
		// telemetry: frames smoothed / frames reported, mean depth
		int rtsFrames = 0;
		int rtsRepFrames = 0;
		double rtsDepthSum = 0;
		// submitted-position ring (receipt clock) for the RELDIAG
		// pose-history channel (fdOut / fd/rawPk / fdRawDir)
		static constexpr int subN = 32;
		double subT[subN] = {};
		double subP[subN][3] = {};
		int subHead = 0;
		int subCount = 0;
	};
	std::map<uint32_t, KalState> kalStates;
	// latest HMD orientation, for rotating the head-space gaze ray into
	// driver space (guarded by deriveFilterLock)
	vr::HmdQuaternion_t hmdQuatForGaze = {1, 0, 0, 0};
	bool haveHmdQuat = false;
	bool gazeAssistAnnounced = false;
	struct DeriveFilterState {
		double vel[3] = {};
		double ang[3] = {};
		double time = 0;
		bool have = false;
		// split-direction support: short ring of RAW estimator outputs.
		// the axis-wise EMA above smooths magnitude well but its
		// direction is noise dominated except at the highest speeds
		// (field data 2026-08-09: 73-83 deg/sample median direction
		// swings at ~0.5 m/s). direction is instead taken from a
		// speed^pow weighted sum of these raw samples inside a short
		// window, so high-SNR samples pin it. 16 slots at 3-10ms
		// spacing covers the whole allowed window range (5-200ms is
		// clamped in the consumer; older entries simply age out).
		static const int dirRingSize = 16;
		double dirTime[dirRingSize] = {};
		double dirVel[dirRingSize][3] = {};
		double dirAng[dirRingSize][3] = {};
		int dirHead = 0;
		int dirCount = 0;
		bool splitLogged = false;
		// scalar magnitude channel: EMA of |raw| directly. the vector EMA's
		// magnitude CANCELS during direction changes (opposing components
		// average toward zero), which both jitters and under-reads; the
		// scalar EMA smooths the speed itself (field data 2026-08-10: the
		// vector-EMA output still jittered 8-15%/sample at 10ms cadence)
		double magEma = 0;
		double angMagEma = 0;
		// release latch: per-channel rolling peaks of the OUTPUT over the
		// latch window. v replays from the linear-peak moment, w from the
		// angular-peak moment (a single combined key poisoned arm throws
		// with the windup vector — field 2026-08-10)
		double linPeakMag = 0;
		double linPeakVel[3] = {};
		double linPeakTime = 0;
		double angPeakMag = 0;
		double angPeakVel[3] = {};
		double angPeakTime = 0;
		double latchUntil = 0;
		// last direction source, so a mid-session source switch re-logs
		int lastSource = -1;
		// throttle for the BURSTDIR diagnostic line (direction-source
		// comparison data; written from the fix block outside all locks)
		double lastDirLogTime = 0;
	};
	std::map<uint32_t, DeriveFilterState> deriveFilterStates = {};
	std::mutex deriveFilterLock;
	int GetDeviceClass(uint32_t openVRID);
	
	// ---- release ground truth tap ----
	// vrlink publishes grip/trigger through IVRDriverInput booleans; the
	// injector forwards creates and updates here. on grip/trigger
	// transitions we log a snapshot of the motion state so every release in
	// a session shows exactly what velocity a game could have read and what
	// the tracking state was. this replaces theorizing about WHY a given
	// throw died (snap back? dropout? zero?) with direct evidence.
	struct InputComponentInfo {
		vr::PropertyContainerHandle_t container = 0;
		std::string name;
		bool lastValue = false;
		bool haveValue = false;
		bool interesting = false; // grip / trigger / squeeze / grab / pinch
		bool isScalar = false;
		float lastScalar = 0;
		bool scalarPressed = false;
		// 2026-09-06 grip capacitive touch synthesis: vrlink only creates
		// /input/grip/value for the Galaxy XR controllers (no grip/touch
		// boolean), so our profile's grip touch never lit. we create the
		// boolean on the same container when grip/value appears and drive
		// it from the value with hysteresis.
		vr::VRInputComponentHandle_t gripTouchHandle = vr::k_ulInvalidInputComponentHandle;
		bool gripTouched = false;
		// distortion tuner control role, classified from the path at create:
		// 0 none, 1 joystick y (nudge), 2 a (band out), 3 b (band in),
		// 4 x (eye cycle), 5 y (reset band), 6 grip value (hold to save).
		// tuner values live in their own fields so the tuner never disturbs
		// lastValue/lastScalar, which the release forensics and velocity fix
		// use for edge and gesture detection.
		int tunerRole = 0;
		float tunerScalar = 0;
		bool tunerBool = false;
	};
	std::map<vr::VRInputComponentHandle_t, InputComponentInfo> inputComponents = {};
	// gate for tuner input capture on the hot component-update path
	std::atomic<bool> tunerInputActive {false};

	std::map<vr::PropertyContainerHandle_t, uint32_t> containerToId = {};
	struct MotionSnapshot {
		double time = 0;
		double outVel[3] = {};
		double outAng[3] = {};
		double outSpeed = 0;
		bool trackingOk = false;
		int result = 0;
	};
	std::map<uint32_t, MotionSnapshot> motionSnapshots = {};
	double lastReleaseLogTime = 0;
	double lastEdgeLogTime = 0;
	void LogReleaseSnapshot(vr::PropertyContainerHandle_t container, const std::string &name);
	uint32_t ResolveContainerId(vr::PropertyContainerHandle_t container);
	// the vrlink HMD device, stored at TrackedDeviceAdded so the real
	// per-eye projection frusta can be queried from its display component
	// (used for the gaze -> viewport mapping, same math the runtime uses
	// for GetEyeTrackedFoveationCenter)
	vr::ITrackedDeviceServerDriver* hmdDevice = nullptr;
	bool hmdProjectionQueried = false;
	bool hmdProjectionValid = false;
	float hmdProjection[2][4] = {}; // [eye][left,right,top,bottom]
public:
	// returns false until the display component has been queried successfully
	bool GetHmdProjectionRaw(int eye, float &left, float &right, float &top, float &bottom);
private:
public:
	// ---- distortion tuner input surface ----
	// aggregated latest controller state for the interactive distortion
	// tuner: largest-magnitude joystick y across hands, band/eye/reset
	// click states, and the max grip value. capture only happens while the
	// tuner is armed (cheap atomic gate on the hot update path).
	struct TunerInputState {
		float stickY = 0;
		float stickX = 0;
		bool bandOut = false;   // a click
		bool bandIn = false;    // b click
		bool eyeToggle = false; // x click
		bool resetBand = false; // y click
		bool segToggle = false; // joystick click (either stick). band tuner with
		                        // segments > 1 uses it for the EYE cycle (X walks
		                        // segments there); unused in classic sessions
		float grip = 0;
		float trigger = 0;
	};
	void SetTunerInputActive(bool active){ tunerInputActive.store(active, std::memory_order_relaxed); }
	void GetTunerInput(TunerInputState &out);
	// ---- controller aligner surface ----
	// latest post-offset controller pose + vrlink tip offset per hand
	// (0 = left, 1 = right), for the aligner's tip marker and pivot solve
	struct AlignControllerState {
		bool poseValid = false;
		double pos[3] = {0, 0, 0};
		vr::HmdQuaternion_t rot = {1, 0, 0, 0};
		double poseTime = 0;      // NowSeconds of last update
		bool tipValid = false;
		double tipLocal[3] = {0, 0, 0};
	};
	void GetAlignController(int hand, AlignControllerState &out);
	// while the aligner is active its WORKING offsets replace the configured
	// controller offsets in the pose path, so edits are live
	void SetAlignerOffsets(bool active, const double rotDeg[3], const double posCm[3]);
	void SetAlignerGrip(bool active, const double gripCm[2][3]);
private:
	// controller aligner state (guarded by poseLogLock): per-hand pose/tip
	// capture, container->hand classification, live offset override
	AlignControllerState alignControllers[2] = {};
	std::map<vr::PropertyContainerHandle_t, int> containerHand;
	// containers whose /pose/tip belongs to left(0)/right(1), resolved from
	// the container's own controller-role property: vrlink publishes tip
	// poses on the paired hand devices, NOT the button controllers, so the
	// button-derived containerHand map cannot associate them (session 25)
	std::map<vr::PropertyContainerHandle_t, int> containerTipHand;
	bool alignerAppliedLogged = false;
	std::map<uint32_t, int> openVRIDHand;
	std::atomic<bool> alignerOverrideActive {false};
	double alignerRotDeg[3] = {0, 0, 0};
	double alignerPosCm[3] = {0, 0, 0};
	// aligner working grip offsets (per hand, cm, controller local frame):
	// while the aligner is active these replace the configured
	// kalmanGrip*Cm so grip captures and stick edits are live in the very
	// next pose (guarded by poseLogLock, same as the pose offsets above)
	std::atomic<bool> alignerGripActive {false};
	double alignerGripCm[2][3] = {};
public:
	void OnInputComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle);
	void OnBooleanComponentUpdated(vr::VRInputComponentHandle_t handle, bool value);
	void OnScalarComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle);
	void OnScalarComponentUpdated(vr::VRInputComponentHandle_t handle, float value);
	void OnPoseComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle);
	void OnSkeletonComponentCreated(vr::PropertyContainerHandle_t container, const char* name, const char* skeletonPath, vr::VRInputComponentHandle_t handle);
	bool HandleSkeletonUpdate(vr::VRInputComponentHandle_t handle, const vr::VRBoneTransform_t* bones, uint32_t count, vr::VRBoneTransform_t* outBones);
	void OnPoseComponentUpdated(vr::VRInputComponentHandle_t handle, const vr::HmdMatrix34_t* offset, double timeOffset);
private:
};

// defined in HmdDriverFactory.cpp
extern CustomHeadsetDeviceProvider deviceProvider;
