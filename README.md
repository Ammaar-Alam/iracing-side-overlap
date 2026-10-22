# iRacing Side Overlap

A small always-on-top overlay for iRacing that shows how far a car alongside you overlaps your car, front to back. It is meant for single-screen setups where the cockpit view and virtual mirror do not show the car beside you.

One native Windows executable, about 0.6 MB, with no runtime dependencies. It reads iRacing's telemetry SDK, never steals focus, and lets every click pass through to the game.

## What it shows

```
   ▮   ┌─┐        a faint outline in the middle is your car, front at the top
   ▮   │ │   ▮    a solid bar beside it is a car on that side, drawn at its length
   ▮   │ │   ▮    the bar moves up as that car gains on you and down as it falls back
       └─┘   ▮
```

| State | Display |
| --- | --- |
| Side clear | nothing on that side |
| Car alongside, position known | solid bar spanning that car's front and rear |
| Two cars on that side | the same bar split in two, spanning both cars |
| Car alongside, position not trustworthy | whole lane outlined and dimly filled, no position |
| Telemetry lost or stale while driving | your car outline turns grey with a slash through it |
| Not driving (garage, menus, replays) | nothing at all |

Whether a side is occupied always comes from iRacing's own spotter. The bar's fore/aft position comes from each car's distance around the lap, and it is shown only when the overlay can tell which car the spotter means and the estimate is precise enough. Otherwise the lane lights up without a position. The accuracy notes are in [docs/validation.md](docs/validation.md).

## Install

Requirements: Windows 10 or 11 (x64) and iRacing in windowed or borderless mode. Exclusive fullscreen hides every overlay, including RaceLab's.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Install
```

The first build downloads a pinned [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) toolchain (about 190 MB, checksum verified) into `.tools`. The script builds everything into `bin`, runs the tests, and copies the overlay to:

```
%LOCALAPPDATA%\Programs\iRacingSideOverlap\side-overlap.exe
```

Settings are saved in `side-overlap.ini` next to the executable.

## Start it with RaceLab

RaceLab's **Tools Manager** launches and closes the overlay with iRacing. Tested with RaceLab 8.4.2.

Open RaceLab, go to **Settings > Tools Manager**, press **+ Add** in the Apps section, and fill in:

| Field | Value |
| --- | --- |
| Name | Side Overlap |
| Executable Path | `C:\Users\<you>\AppData\Local\Programs\iRacingSideOverlap\side-overlap.exe` |
| Arguments | *(leave empty)* |
| Working Directory | *(leave empty)* |
| Auto Start | With Sim |
| Auto Stop | With Sim |
| Sim Filter | iRacing |
| Termination Mode | Graceful |
| Send close signal twice | off |

The overlay handles RaceLab's graceful close (`WM_CLOSE`), saves its settings and exits. A second copy started by accident exits immediately.

## Use

The overlay puts an icon in the notification area. Right-click it for:

- **Edit layout**: makes the overlay draggable and resizable, with a demo animation so you can see the result. Choose it again to lock it.
- **Demo**: plays scripted traffic through the real estimator. A `DEMO` label marks every non-live mode.
- **Reset position**: moves the overlay back under the mirror on the primary monitor. This also happens automatically if a saved position ends up off screen.
- **Opacity**, **Color** and **Refresh rate** (30 or 60 Hz).
- **Open settings file**.

You can also start it from the command line:

```
side-overlap.exe [--edit] [--demo] [--playback CAPTURE.ibt [--from ROW]] [--record DIR] [--stats]
```

- `--playback` drives the overlay from a recorded capture at real speed.
- `--record` saves the telemetry the overlay sees to `.ibt` files, written from a background thread.
- `--stats` appends timing, CPU and memory figures to `side-overlap-stats.txt` when the overlay exits.

The `[cars]` section of the settings file sets car lengths in metres by iRacing car path, for example `mx5 mx52016=3.915`. Cars without an entry use a short built-in table, or 4.5 m.

## Tools

| Tool | Purpose |
| --- | --- |
| `irinspect inventory` | Lists every telemetry variable the running sim exposes, with sample values, change rates, sentinel counts and tick timing. |
| `irinspect capture` | Records live telemetry to `.ibt` with host timestamps, until Ctrl+C. |
| `irinspect ibt FILE` | The same inventory for a recorded file. |
| `irinspect clip IN OUT --from A --to B` | Cuts a compact excerpt for test fixtures, keeping only the variables the overlay reads. Its session data holds no names or account ids. |
| `sideeval FILES...` | Replays captures through the estimator and reports occupancy, positioning, association, spotter-edge agreement, freshness and timing. |

## Documentation

- [docs/telemetry.md](docs/telemetry.md): what iRacing exposes, verified on the installed build.
- [docs/research.md](docs/research.md): estimator design, the methods it borrows, and the experiments behind each decision.
- [docs/validation.md](docs/validation.md): accuracy and performance results, and what remains untested.

## Layout

```
src/irsdk.h            telemetry layout shared by live memory and .ibt files
src/sdk_live.*         coherent snapshots from the memory map
src/ibt.*, recorder.*  .ibt reading, writing and bounded background recording
src/frame.*            the variables the estimator reads
src/session_info.*     track length and car list from the session string
src/estimator.*        occupancy, association and fore/aft estimate
src/overlay.cpp        the window, drawing, tray menu and main loop
src/render.*           antialiased drawing into the layered window
src/inspect.cpp        irinspect
src/sideeval.cpp       sideeval
tests/                 estimator tests and recorded fixtures
```
