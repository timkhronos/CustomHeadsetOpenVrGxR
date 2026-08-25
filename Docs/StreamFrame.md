# Galaxy XR Driver: Setup and Image Processing

A standalone SteamVR vendor driver for the Samsung Galaxy XR over Steam Link / vrlink. It does three things:

- **Identity.** The headset and controllers show up in SteamVR as what they are: Galaxy XR model name, icons, official controller models and bindings.
- **Controllers.** Corrected grip origin and pose components, plus a Kalman filter that fixes throw velocity and improves tracking dropouts.
- **Image processing.** Color, sharpening, anti-aliasing and distortion correction applied to the streamed frames right before the driver encodes them.

Image processing settings apply live within about a second. Identity, input profile, resolution and quality need a SteamVR restart.

## Install

1. Unpack the entire release zip. The GUI folder and the `GalaxyXRNative` folder must stay next to each other.
2. Run `CustomHeadsetGUI.exe`, go to About, press Install.
3. Older copies of this fork are removed automatically on the first install. If the stock CustomHeadsetOpenVR driver is also enabled, the GUI shows a notice and a "Switch to this driver" button, press it, two drivers must not claim the headset at once.
4. Restart SteamVR. The Galaxy XR page in the GUI has everything.

Settings live in `%APPDATA%\GalaxyXR\CustomHeadset\settings.json`, separate from the stock driver's `%APPDATA%\CustomHeadset\` so the two never share state.

## Recommended configuration

Galaxy XR page:

- **Headset**: Native Identity on, Native Render Resolution on (default), Stream Quality Preset: High.
- **Controllers**: Official Controller Input Profile on. Controller Fix Mode: Kalman CA (default). Leave the offsets alone unless the controllers visibly sit wrong in your hand, the shipped values were measured against the official models.
- **Image Processing**: Enable on.

Device pages: leave **Custom Shader disabled**. The Galaxy XR image processing replaces it for streamed HMDs, the page warns if both are active with color adjustments.

After changing anything in the Headset section or the input profile, restart SteamVR.

## Controllers

With the official input profile on, SteamVR sees `galaxy_xr_controller`. Games that ship a native Galaxy XR binding use it, everything else falls back to Index controller bindings through the official remapping, so most titles will show Index controllers in their binding UI. Custom per-game bindings made before enabling the profile (when the controllers were Touch) do not carry over.

Controller Fix Mode drives the pose filter. Kalman CA is the default, the tuning row below it shows the gains for whichever mode is selected, and Kalman Advanced Settings holds the rarely-touched knobs. Since 1.0.0 the reported angular velocity is in controller-local frame, which is what SteamVR's own prediction expects.

Controller Offsets (under Controllers Advanced) are authored for the left hand and mirrored to the right by default. Per-hand trims appear when Mirror is off and are added on top of the shared offsets.

## Image processing notes

- **Saturation / Contrast / Gamma**: same semantics as the original shader (50 and 2.2 are neutral).
- **Vibrance**: smart saturation, changes muted colors most and vivid colors least. Stacks with Saturation.
- **FXAA**: anti-aliasing before the encode.
- **CAS Sharpening**: applied before the video encode, per-eye strength available.
- **Dither**: helps banding in dark gradients. The encoder eats some of it, but low amplitude noise before quantization still helps.
- **Stationary Dimming**: dims the image when the headset is not moving for a while.
- **Color Matrix**: advanced gamut/white point correction. Empty = off.

## Distortion correction (static)

Symptom: fixate a point, rotate your head, the world ripples or swims. Most likely cause: the headset's built-in lens correction is slightly off; the error is a function of distance from the optical center. The correction applies a small counter-warp to the streamed image.

There are two ways to get a correction. The camera measured displacement map is the primary path; the radial curves are kept as a by-eye and smooth-prior path.

### Camera measured (displacement map)

A per-eye displacement map measured with a calibrated camera in front of the lens, either an automatic Gray-code sweep or manual editing against a ground truth grid. See [TunerUsage.md](TunerUsage.md) for the full workflow, `streamFrame.calib.*` for blackout / capture mode / patterns, and `streamFrame.brightness` for the always-on brightness multiplier.

### By eye (radial curves)

The curve sets a radial scale per ring around the optical center: 1.0 leaves that ring untouched, above 1.0 pulls its content toward the center, below pushes it outward. Real corrections are within about a percent of 1.0.

Suggested calibration workflow (WIP):

1. Stand in front of straight lines or text (the SteamVR construct grid works, or testHMD works).
2. Start in k1/k2 mode. Fixate a grid/text, rotate your head, nudge k1 in ±0.005 steps, refine periphery with k2.
3. Press "Convert k1/k2 to spline" to keep the shape and gain per-radius control points. Drag points, double click adds, right click removes.
4. Use the Annulus Tuning Band to isolate one radius band while tuning its control point (diagnostic only, disable for normal use).
5. If the residual differs between eyes or directions, enable Per Eye and/or Per Axis curves. The existing curve is copied as a starting point; tabs above the plot select which curve you are editing, siblings draw dimmed.
6. Optical Center Offsets move where the rings are anchored, per eye, if the residual is asymmetric around the center.
7. Eye Alignment (inside Distortion Correction) shifts each eye's image for prism-type misalignment. It is not a distortion fix; use it only if lines converge differently in each eye.

Calibrated one eye only? `python tools/gxr_mirror.py` mirrors it to the other. Lens distortion should be symmetric by design and a shared mirrored profile avoids inter-eye disparity, which is worse for the visual system than a shared error.

Share Distortion Profile exports and imports profiles as text or JSON so they can be posted and compared.

## Advanced: blackout

Blackout Headset Screens (Advanced section) turns both panels black while tracking, streaming and the game keep running. Use it when the headset stays connected but nobody is looking through it, while developing or testing, to avoid burn in. It needs Enable Image Processing on, and the calibration banner reminds you while it is active.

## Troubleshooting

Check `Steam\logs\vrserver.txt`:

- `FrameComponentShim: wrapping IVRDriverDirectModeComponent`: the frame path is hooked. Missing: the headset driver is not direct mode, or the driver did not load (check for duplicate registrations).
- `GalaxyXR: wrote driver_vrlink render 3552x3840 @90`: native resolution is set. Takes effect on the next SteamVR start.
- `GalaxyXR: stream quality '<preset>'`: the preset was applied. Takes effect at the next headset connect.
- `FrameProcessor: matched hmd adapter <gpu>`: processing runs on the GPU SteamVR renders on. `no adapter matched` / `failed to create D3D11 device`: report with your GPU setup.
- `FrameProcessor: OpenSharedResource failed`: shared texture access problem, usually wrong adapter (see above).
- `FrameProcessor: AcquireSync returned 0x00000102` occasionally is normal (busy frame skipped); constant spam is a problem you should report.
- `FrameProcessor: baked distortion lut (...)` appears whenever curve settings change.
- `FrameProcessor: PS compile error: ...`: a live edit of `resources\shaders\d3d11\vrlink_layer_ps.hlsl` has a typo; the previous working shader stays active.
- `Config: schema migration ...` on first start after upgrading: expected once. It resets Direction Lead and the shared controller offsets to the new defaults.

"Changes nothing": make sure Enable Image Processing is on and you are testing with something non-neutral (saturation 0 is unmissable).

"Controllers show as Index": that is the official profile's fallback and expected. See Controllers above.

"Headset still shows as vrlink / generic": Native Identity needs a full SteamVR restart, not a compositor restart.

If you have any issues, open an issue on the GitHub page linked in the GUI's About page and attach your `Steam\logs\vrserver.txt`. Please don't open them on the original sboys3 repository.
