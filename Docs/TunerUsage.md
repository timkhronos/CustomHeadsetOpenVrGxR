# Camera Based Distortion Tuning (Galaxy XR / streamed headsets)

Everything lives in `tools/` and talks to the driver through `%APPDATA%\GalaxyXR\CustomHeadset\settings.json` (the driver hot reloads it) and `diagnostic.json` (the driver reports back through it). The tools fall back to the old `%APPDATA%\CustomHeadset\` folder if the vendor one does not exist.

## How it works

The headset applies its own lens correction to the streamed image.

The driver's image processing pass can pre-warp the image with a displacement map: a 33x33 grid of tiny (du, dv) offsets per eye that says "the pixel here should sample the content from slightly over there". To measure what the offsets should be, a calibrated wide angle camera sits where your eye would be, the driver draws known patterns, and the tools compare where the camera sees each panel position against where it should be.

The auto method (`gxr_sweep.py`) measures the whole eye in ~30 s and writes the map. The manual method (`gxr_overlay.py`) shows the camera view with a ground truth grid drawn over it, and lets you drag the picture into place while watching the result live. Both write the same map, so you can sweep first and refine by hand.

## What you need

- A wide angle USB camera (fisheye, 150 deg or more diagonal).
- Python 3.9+ on the PC running SteamVR. In `tools/`:
  `pip install -r requirements.txt`
- A TV or large monitor/printer to display the ChArUco calibration board
  (`make_charuco_board.py` generates it for your screen). The legacy printed chessboard
  (`tools/board.svg`) still works.
- The GxR driver build with the camera calibration support (this repo),
  Enable Image Processing on, on the Galaxy XR page.
- Something to hold the camera in front of one lens. See "Mounting".

## Step 0: camera settings

Copy `tools/gxr_camera.example.json` to `tools/gxr_camera.json`. The default `"mode": "bypass"` opens the camera exactly like sboys3's scripts, except that a `fourcc` given in the config is applied BEFORE the resolution. 

The high-res modes of these sensors exist only in MJPEG, and under the default YUY2 negotiation the camera silently falls back to a low mode (my 8MP IMX179 reports 1600x1200 no matter what I ask for). 

With `"fourcc": "MJPG"` the full 3264x2448 negotiates at ~15 fps, which is fine for static captures and only makes the sweep a little slower. Set `camera_id` (0, 1, 2 ... until the right camera shows) and check the printed resolution on the first run. 

The fisheye calibration is only valid at the resolution it was made at, if you change `width`/`height`, recalibrate!

## Step 1: calibrate the fisheye (once per camera)

The camera's own distortion must be known before it can measure anything.
There are 3 calibration modes, rational, fisheye and charuco.

1. `cd tools`
2. `python make_charuco_board.py --tv-diag 55 --tv-res 3840x2160` (Your screens size and res). Show the resulting `charuco_board.png` on the TV fullscreen at 1:1 / 100% zoom.
3. `python charuco_capture.py`. Move and tilt the camera: near, far, all over the frame, and deliberately past the edge, so the board hangs half out of the image. The bars along the bottom count detected corners per radius ring, center to rim, do not stop until the outer rings have counts too. 30 to 50 captures is typical.
4. `python charuco_calibrate.py`. It fits the fisheye model (and the rational model as a cross-check) with an explicit outlier loop, and prints corner coverage and residual binned by field angle. 

Alternative the rational model is usable too and more simple. Just display the board.svg file, run python capture_calibration_images.py, then python camera_calibration.py.

The result is `tools/output/calibration_data.pkl`. Redo it only if the lens or resolution changes.

## Step 2: mounting and alignment

Goal: the front element of the fisheye sits where the center of your pupil sits when you wear the headset (same distance from the lens, centered on the lens), pointing roughly along the lens axis.

- What matters most is position (left/right, up/down, and eye relief distance): the headset's true mapping depends on where the pupil is, and we correct for a pupil, so measure with the camera at a pupil position.
- Rotation (yaw/pitch/roll) matters too but less: the tools can solve some of the camera's rotation out numerically. Get it roughly right, then let the readout tell you if you should nudge it.

Alignment procedure (all in `python gxr_overlay.py --eye left`):

1. Blackout on (`b`) while you rig, off when ready(blackout helps protect your panels when not actively calibrating).
2. Position the camera at the pupil position, then rotate until the camera drawn white cross lines up with the one diplsayed by the headset

## Step 3: auto measurement

`python gxr_sweep.py --eye left`

What happens: blackout and capture mode are turned off for the run, the driver shows 42 stripe patterns plus black and white on the left eye (right eye blacked out for safety), the camera captures each after the driver has confirmed the pattern in `diagnostic.json`, the decode gives every camera pixel's panel uv, the rotation is solved, the 33x33 map is fitted and:

- written live into `settings.json` (`streamFrame.distortion.map`, at the current `distortion.gain`),
- saved as an importable profile in `%APPDATA%\GalaxyXR\CustomHeadset\Distortion\gxr-map-<stamp>.json` (version 2 profile, contains the radial curves and centers too),
- blackout/capture mode restored to what they were.

Read the printout:

- `contrast ... % of camera pixels see the panel`: 35-60% is typical.
- `usable bits`: 8 or 9 is fine, 7 is coarse but works, less means the camera does not resolve the stripes (closer, focus, exposure).
- `camera rotation ...` warns you if your rotation is majorly off.
- `correction: max |disp| ...`: the size of the correction, in uv and in panel pixels. Tens of pixels at the edge is plausible, hundreds is not.
- `fit residual rms`: how well the smooth lattice explains the samples

Options: `--bits 9` if the finest stripes are noisy, `--smooth 0.1` for a smoother map, `--taper 5` for a slower fade to identity outside coverage, `--dry` to look without applying. `--align` for the rotation readout only.

Then `python gxr_sweep.py --eye right` after moving the camera to the right lens (repeat Step 2 for that eye). The left eye's map is preserved. (OR USE THE MIRROR TOOL, SEE BELOW)

## Step 4: verify and refine (overlay)

`python gxr_overlay.py --eye left`, capture mode on (`c`). The driver draws the hue grid in content space, so the map warps it exactly like game content. Ground truth lines are drawn on top. Where they coincide, the correction should be right. Where the colored camera lines sit beside the ground truth, drag them onto it:

- Every grey dot is a lattice knot, drawn where its content currently sits. Hover near one and it highlights with the falloff circle. Press, drag: the dot sticks to the cursor, neighbours inside the circle follow with a falloff (mouse wheel or `-`/`=` for the radius). 

- Release, wait a second, the driver rebakes and the camera shows the new state. Hold Shift while dragging to constrain to vertical, Ctrl for horizontal, Alt to move in discrete 2-panel-pixel steps (combinable, e.g. Ctrl+Alt = horizontal in steps).
- `[` `]` camera-side ground truth opacity; `;` `'` headset-side grid brightness (`calib.patternBrightness`, also follows the general `streamFrame.brightness`); `k` toggles the dots.
- `g` flips gain 0/1: instant A/B between raw headset and corrected.
- `z` undo, `r` reset knots inside the circle at the cursor, `R` reset the eye.
- `f` / `F`: smoothing. `F` relaxe  the whole eye's lattice toward the smoothest map near the current one `f` does the same only inside the brush radius at the cursor, for polishing one region without touching the rest. 
- Both print the roughness before/after and the max change in px, both repeat for stronger effect, and `z` undoes. Typical use: hand-tune, `F`, check with `v`, repeat.
- `s` saves a profile file. `x` restores the eye to the session baseline (the map as it was when the overlay started, i.e. the swept result if you started right after a sweep). `e` switches eyes (also tells the driver; in capture mode the other eye is black).

Working from scratch by hand is the same with the sweep skipped: start with the raw headset (gain 1, empty map = identity), and drag lines onto their ground truth, coarse brush first, then fine.

Both eyes done: put the headset on, do the swim test (fixate, rotate your head). If it swims less at gain 1 than gain 0, it's good. The map is stored in `settings.json`; the saved profile file is the shareable/importable copy (GUI, Galaxy XR page, import).

### Mirroring one eye onto the other

If only one eye is calibrated, or the two measurements disagree more than the lenses plausibly do, `python gxr_mirror.py` copies the left eye's calibration onto the right, horizontally flipped (`--from right` for the other way, `--file` for a saved profile instead of `settings.json`, `--in-place` to overwrite). 

Every per-eye structure present is mirrored: lattice map, radial and per-axis curves, segments, center offsets.

### Free knots and the paint brush

Knots the sweep could not see (or that you mark yourself) are *free*: drawn hollow and dim, ignored by the brush falloff and by `f`/`F` smoothing. Grabbing a free knot directly still works and promotes it back to constrained. `p` toggles paint mode: stroke to mark knots free (for regions the camera cannot see), Shift+stroke to lock them back, mouse wheel for radius, `p` again to exit.

## Between sessions

Blackout (`streamFrame.calib.blackout`, `b` in the overlay, or Blackout Headset Screens under Advanced in the GUI) turns both panels fully black without stopping anything. Use it whenever the rig stays mounted and you are not actively looking through the camera.

## Troubleshooting

- Correction good in the middle, off in the outer third: recalibrate the camera with the ChArUco tools (see Step 1), then refit.
- Two writers: the GUI and the tools both write `settings.json`. Save in the GUI, then edit with the tools, or the other way round; do not drag in the overlay while dragging a slider in the GUI.

## Status and known limitations

- The mount is the hard part. The sweep itself does not need a precise rotation (the rotation is solved per run ), but it does need the camera *rigid for ~2 minutes* and roughly centered/aimed so the panel fills the view.
- Exposure must be pinned for sweeps. Auto exposure pumps between patterns and saturates the white reference.

## Files

- `tools/gxr_common.py`: shared code (paths, settings/diagnostic IO,
  camera, panel model, lattice fit, profile output).
- `tools/gxr_sweep.py`: automatic Gray-code measurement.
- `tools/gxr_overlay.py`: live view, alignment, manual editing.
- `tools/gxr_mirror.py`: mirror one eye's calibration onto the other.
- `tools/make_charuco_board.py`, `tools/charuco_capture.py`,
  `tools/charuco_calibrate.py`: ChArUco fisheye calibration
- `tools/capture_calibration_images.py`, `tools/camera_calibration.py`,
  `tools/board.svg`: chessboard calibration
