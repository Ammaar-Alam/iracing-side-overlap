# What iRacing exposes

Verified on simulator build 2026.10.01.01 (2026 Season 4 Patch 1) in an official online MX-5 Cup practice and race at Lime Rock Park. The sources are:

- `irinspect inventory` against the live memory map: 327 variables, 8,596-byte rows, 60 Hz
- `irinspect ibt` against the captures and against iRacing's own disk logs from build 2026.06.24.02
- the session string
- `app.ini`

Readings are labelled **observed** (read from the interface) or **estimated** (computed by the overlay). Anything the interface does not provide is listed as unavailable.

## Capability table

| Signal | Verified meaning | Freshness | Limitations | Use |
| --- | --- | --- | --- | --- |
| `CarLeftRight` (int, player only) | The spotter's side occupancy: 0 off, 1 clear, 2 left, 3 right, 4 both sides, 5 two left, 6 two right. Declared `int` on this build. | New value every 60 Hz tick. Changes at the overlap edges (see below). | Categorical. Says nothing about which car, where it is, or how far away sideways. Off appears briefly when getting into the car. | Sole authority for occupancy. |
| `CarIdxLapDistPct[64]` (float) | Each car's fraction of the lap along iRacing's track reference, −1 when the car is not in the world. | Every tick for every car. Remote cars on track advance smoothly (see freshness). | A position along the track, not along our car's axis. The reference point on the car is undocumented. | Fore/aft offset and candidates. |
| `CarIdxTrackSurface[64]` (int) | −1 not in world, 0 off track, 1 in pit stall, 2 approaching pits, 3 on track. | Every tick. | Coarse. | Drops cars that are not in the world. Classifies diagnostics. |
| `CarIdxOnPitRoad[64]` (bool) | Car is on pit road. | Every tick. | — | Diagnostics. |
| `PlayerCarIdx`, `CamCarIdx` | Our car index and the camera's car. | Every tick. | — | Reference car. |
| `IsOnTrack`, `IsReplayPlaying` | Driving with physics running, or replay playing. | Every tick. | — | Shows the overlay only while driving. |
| `SessionTime`, `SessionNum`, `SessionUniqueID`, `SessionTick` | Sim clock (stops when paused), session identity. | Every tick. | Jumps in replays. | Filter steps, resets. |
| `Speed`, `VelocityX/Y`, `YawRate` (player only) | Our speed, body-frame velocity, yaw rate. `_ST` versions carry 6 sub-samples per tick (360 Hz). | Every tick. | Not published for other cars. | Curvature and slip in the geometry uncertainty. |
| `CarDistAhead`, `CarDistBehind` (player only) | Distance to the nearest car ahead or behind, in metres. 500000 when there is none. | Every tick. | No side and no identity. Can be derived from lap distance. | Not used. |
| `CarIdxEstTime`, `CarIdxSteer`, `CarIdxRPM`, `CarIdxGear` | Per-car timing estimate and driver inputs. | Every tick. | Not geometry. | Not used. |
| Session `WeekendInfo.TrackLength` | Length of the lap reference, 4 decimals in km (2.3360 km at Lime Rock GP). | Per session. | — | Converts lap fraction to metres. |
| Session `WeekendInfo.TrackDirection` | `neutral` on road courses. | Per session. | An iRacing developer note says left and right map to low and high on ovals. Reversed ovals are untested. | Diagnostics. |
| Session `DriverInfo.Drivers[]` | `CarIdx`, `CarPath`, `CarIsPaceCar`, `IsSpectator`. | When the string updates. | No car dimensions. | Car model per index, for lengths. |
| `app.ini [SPCC] carLowHiPadding` | "How much clearance, front and back in meters, to give a car before reporting it as clear." 0.25 by default. | Read at startup. | — | Overlap padding. |

### Unavailable

Neither the live map nor the disk logs carry any of the following:

- other cars' world position, heading, lateral offset or speed
- any car's dimensions
- the reference point behind `CarIdxLapDistPct`
- the spotter's lateral range

The player's own `Lat`, `Lon` and `Alt` exist only in disk logs, not in the live map. Several open-source radars ask for `CarIdxPosX/Y/Z` or `CarIdxHeading`. Those variables do not exist.

### Where each signal exists

- **Live memory while driving:** everything above.
- **iRacing disk logs (`.ibt`):** player-only. 278 variables with no `CarIdx*` arrays and no `CarLeftRight`. The overlay cannot be evaluated from iRacing's own disk logs, which is why `irinspect capture` records the memory map instead.
- **Replays:** the memory map keeps publishing `CarIdx*` arrays, and `IsReplayPlaying`, `ReplaySessionTime` and `ReplayFrameNum` describe the replay.
  - The player-only variables (`Speed`, `Velocity*`, `YawRate`) describe our own parked car, not the car being replayed.
  - `CarLeftRight` read 1 on all 4,790 captured replay ticks. No car overlapped in those frames, so whether the spotter runs during replays is unverified.
  - The overlay draws nothing while a replay plays.
- **Array length:** with `irsdkLogAllCars=0` (the default) the `CarIdx*` arrays have 64 entries. The 2026 Season 3 Patch 1 notes say setting it to 1 lets them "grow to the actual size of the entries table." The overlay reads array lengths from the variable headers, so either setting works. Larger arrays add entries, not geometry.

## Official statements

- iRacing's native Visual Spotter (2026 Season 2): "This feature is not a vehicle radar - it uses the same data that powers the regular audio and text Spotter system." No SDK variables were added for it.
- 2026 Season 3 Patch 1: "[Misc] irsdkLogAllCars=0. When set to 1, all the CarIdxXXX arrays will be allowed to grow to the actual size of the entries table."
- 2026 Season 4: the spotter no longer reports cars from other run groups.

## Update behaviour

- Rows arrive on iRacing's data-valid event at 60 Hz. Every event wake carried a new tick, and no rows were torn or missed in about 75 minutes of capture with RaceLab reading at the same time.
- The host interval between ticks has a median of 16.66 ms, with 95 % between 14.7 and 18.6 ms. `SessionTime` advances in exact 1/60 s steps.
- The variable layout was identical across practice and race. The overlay still rebinds whenever the header changes.
