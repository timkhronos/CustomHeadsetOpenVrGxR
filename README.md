# CustomHeadsetOpenVR GxR

A SteamVR driver for the Samsung Galaxy XR over Steam Link / vrlink. It makes the headset and its controllers show up in SteamVR as what they are, fixes controller tracking and throw velocity, and processes the streamed image (color, sharpening, anti-aliasing, distortion correction) right before it is encoded.

It is a fork of [CustomHeadsetOpenVR by sboys3](https://github.com/sboys3/CustomHeadsetOpenVR) and ships as a separate vendor driver (`GalaxyXRNative`), so it can be installed next to the original and switched with one click.

<picture><img src="./CustomHeadsetGUI/public/CustomHeadsetCropped.png" height="96"><img/></picture>

## What it does

**Headset**
- Native Galaxy XR identity in SteamVR: model name, manufacturer, icons. No more generic vrlink HMD / Quest Pro.
- Native render resolution: SteamVR renders at the panel's 3552×3840 per eye instead of the streamer's default.
- Stream quality presets for the vrlink encoder.

**Controllers**
- Galaxy XR controller models, input profile and compositor bindings. Games without a native binding see Index controllers.
- Corrected grip origin and pose components so held objects sit where the game expects them.
- Kalman pose filter that fixes throw velocity and improves tracking dropouts from the streamed pose.

**Image processing**
- Saturation, vibrance, contrast, gamma, color matrix, brightness.
- CAS sharpening, FXAA, dither, stationary dimming.
- Distortion correction. Unlike the MeganeX, the Galaxy XR owns its own lens correction, so this driver pre-warps the image before handing it off rather than replacing the profile. Corrections come from camera-measured per-eye displacement maps or by-eye radial curves, with profile export/import.
- Blackout for leaving the headset connected without burn-in.

Everything is configured from the GUI's **Galaxy XR** page. Identity, input profile, resolution and quality need a SteamVR restart.

## Installing

1. Download the latest release from the [releases page](https://github.com/timkhronos/CustomHeadsetOpenVrGxR/releases/latest).
2. Extract the whole folder from the zip. The `CustomHeadsetGUI` and `GalaxyXRNative` folders must stay next to each other.
3. Run `custom-headset-gui.exe` in `CustomHeadsetGUI`, go to About, press Install.
4. If the stock CustomHeadsetOpenVR driver is also enabled, the GUI shows a notice and a "Switch to this driver" button. Press it.
5. Restart SteamVR.

Older copies of this fork that were installed into `SteamVR\drivers\CustomHeadsetOpenVR` are removed automatically on the first install, and their settings and distortion profiles are copied into the new settings folder.

![Installation Tutorial](Docs/Media/CustomHeadsetInstall.webp)

## Updating

Same as installing: extract the new zip, run the GUI, go to About and press Install (or Re-Install). The GUI checks this repository's releases and tells you when a newer version is available.

## Recommended settings

On the Galaxy XR page:

- Headset: Native Identity on, Native Render Resolution on (default), Stream Quality Preset High.
- Controllers: Official Controller Input Profile on. Leave Controller Fix Mode on Kalman CA.
- Image Processing: Enable on. Leave the Custom Shader on the device pages disabled, the image processing replaces it.

Then restart SteamVR once.

## Documentation

- [Setup and image processing guide](Docs/StreamFrame.md): install, recommended configuration, feature notes, distortion correction by eye, troubleshooting from `vrserver.txt`.
- [Camera based distortion tuning](Docs/TunerUsage.md): measuring the lens correction with a fisheye camera, the sweep and overlay tools, mirroring one eye onto the other.

## Reporting issues and Feature Requests

Open an issue [here](https://github.com/timkhronos/CustomHeadsetOpenVrGxR/issues) and attach `Steam\logs\vrserver.txt` if applicable. Please do not report problems with this fork on the original CustomHeadsetOpenVR repository.

## Manual configuration

Settings live in `%APPDATA%\GalaxyXR\CustomHeadset\settings.json` and are hot-reloaded by the driver. Distortion profiles go in a `Distortion` subfolder and are referenced by name.

## Tools

`tools/` holds the camera calibration and distortion tuning scripts (`gxr_sweep.py`, `gxr_overlay.py`, `gxr_mirror.py`, and the ChArUco / chessboard camera calibration). Python 3.9+, `pip install -r requirements.txt`. See the [tuner guide](Docs/TunerUsage.md).

## Other headsets

The MeganeX 8K and Dream Air support from the original project is still in this driver and still configured from the Driver Settings tab, but it is not what this fork is tested on. If you own one of those, use [sboys3's releases](https://github.com/sboys3/CustomHeadsetOpenVR/releases/latest), the two drivers coexist and the GUI switches between them.

The image processing features should work on other direct-mode streamed headsets but are untested.

## Credits

Built on [CustomHeadsetOpenVR](https://github.com/sboys3/CustomHeadsetOpenVR) by sboys3, which provides the installer, settings system, and the MeganeX / Dream Air support. The camera calibration scripts in `tools/` started from sboys3's calibration code (see `tools/LICENSE-sboys3-camera-calibration`).

## License

GPLv2, same as the original project.
