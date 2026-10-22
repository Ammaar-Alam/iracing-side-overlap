# Validation

All results were recorded live with `irinspect capture` and replayed causally with `sideeval`. Simulator build 2026.10.01.01. Hardware: Acer Predator PHN16S-71 (Core Ultra 9 275HX, RTX 5060 Laptop, 16 GB), 2560×1600 at 240 Hz, 150 % scaling, iRacing windowed and maximized, with RaceLab 8.4.2 running throughout.

## Data

| Set | Content | Driving ticks | Occupied ticks | Spotter transitions |
| --- | --- | --- | --- | --- |
| Tuning | Official MX-5 Cup practice, Lime Rock GP | 61,906 (17.2 min) | 1,369 (355 left, 1,014 right) | 8 |
| Evaluation | Two later race events at the same track, each with practice, lone qualifying and race (finished P10 and P5). The second race had the overlay running live from RaceLab. | 178,009 (49.4 min) | 438 (right) | 4 |

Both sets are online multiplayer at one track with one car model. The only parameter changed after looking at data, the pit lane prior, was chosen from the tuning set alone and then checked on the evaluation set. Three short excerpts with sanitized session data are kept in `tests/fixtures` and replayed by every build.

**This is a small sample.** Neither set contains three-wide racing or two cars on one side, so those states are verified only on synthetic frames. See [Untested](#untested).

## What each check proves

- **Agreement with `CarLeftRight`** validates signal handling only. The overlay shows a side as occupied exactly when the spotter does, by construction, and the fixture test asserts it for every tick.
- **Fore/aft position** is checked against the spotter's own geometry at the moments it flips. This is the one comparison where an independent measurement (the sim's internal car shapes) meets the lap-distance estimate.
- **Association** is checked against hindsight labels from a reverse-time pass over the same data. That is a consistency check, not ground truth.

## Fore/aft accuracy at the spotter edges

At every flip of `CarLeftRight` with exactly one car within 8 m, `sideeval` records the lap-distance offset between the two ticks around the flip. The model being tested: the spotter calls a car alongside when the bumpers touch (combined half lengths of 3.915 m for two MX-5s) and clear once it is 0.25 m past (`carLowHiPadding`).

| | n | Error p05 | Median | p95 |
| --- | --- | --- | --- | --- |
| Entering | 6 | +0.00 m | **+0.19 m** | +0.23 m |
| Leaving | 6 | −0.16 m | **+0.07 m** | +0.14 m |
| Tight radius ≤ 200 m (\|error\|) | 9 | 0.00 m | 0.09 m | 0.23 m |
| Straight (\|error\|) | 3 | 0.16 m | 0.19 m | 0.23 m |

All three events are pooled here, because each one produced only two to eight isolated flips. The two held-out race events gave entries of +0.18 and +0.14 m and exits of +0.04 and +0.14 m, in line with the tuning set.

- Lap distance and the spotter agree to within about a quarter of a metre at every observed edge.
- Exits fit the model within about 0.15 m (one outlier at −0.16 m).
- Entries are consistently about 0.19 m early. The spotter calls a car alongside while lap distance still shows about 0.19 m of gap. That shift is the same on straights and in corners, so it reads as a property of the spotter's entry threshold, not as error in lap distance. On screen, a car's bar appears with a gap of about 2 px at the default size before it touches your outline.
- No curvature effect is detectable at this sample size, so the corner term in the uncertainty model is untested by data. In these sessions it never moved a bar into fallback (imprecise fallback 0 %).

## Positioning and association

| Measure | Tuning | Evaluation |
| --- | --- | --- |
| Occupied time with a positioned bar | 100 % (99.49 % before the pit lane prior) | 100 % |
| Identity switches while positioned | 0 | 0 |
| Positioned ticks that disagree with hindsight | 0 of 1,369 | 0 of 438 |
| Occupied with no car within reach (unplaceable) | 0 | 0 |

- The only fallback ever seen was 7 ticks (0.12 s) on the main straight. Two cars parked in pit stalls beside the track sat level with us by lap distance and competed with the real car alongside.
- In the tuning set, a single overlapping car on pit road while we were on track (or the reverse) was called by the spotter in 0 of 260 ticks. That motivated a 0.02 prior for pit lane mismatches.
- The first held-out race event confirmed the prior: 0 of 200 such ticks were called.
- A similar rule for off-track cars was rejected. When we put two wheels off, the spotter still reported the car beside us in 65 % of ticks.

## Why the spotter must stay in charge

Car-ticks where lap distance showed a car overlapping by more than 0.5 m but the spotter said clear:

- **Lone qualifying:** 1,639 ticks. Other drivers' cars are reported by the SDK but are not physically on your track. One ran "alongside" at full speed for 14.5 s.
- **Race start:** cars level on the grid, while the spotter stayed silent with iRacing's default `carLowHiAtStart=0`.
- **Slow, stopped or spun cars** off the racing line, and pit stalls next to the straight.

An overlay that drew the nearest car by lap distance would have shown a car beside you in every one of these cases. This one showed nothing.

## Freshness and timing

| Measure | Result |
| --- | --- |
| Tick interval on the host | median 16.66 ms, 95 % within 14.9–18.4 ms |
| Missed or torn ticks | 0 in 247,982 rows (RaceLab reading at the same time) |
| Event wakes without a new tick | 0 |
| Nearby on-track cars whose position froze for a tick | 1 of 42,157 samples |
| Tick-to-tick change in a nearby car's step | median 0.001 m, p99.9 0.005 m, max 0.013 m |

Remote cars move smoothly every tick, and there is no noise to filter. Whatever network smoothing iRacing applies happens before the SDK, and the overlay matches what the local sim renders and collides with.

## Filtering and prediction

Measured over all 1,807 occupied side-ticks:

| Variant | \|bar − raw\| p95 | Error vs next tick p50 / p95 | Roughness p95 |
| --- | --- | --- | --- |
| Raw (chosen) | 0 | 1.8 / 6.9 cm | 2 mm |
| Alpha-beta (0.5, 0.1) | 0.7 cm | 1.9 / 6.8 cm | 2 mm |
| Kalman, constant velocity | 2.7 cm | 1.9 / 7.5 cm | 2 mm |
| Raw + one tick prediction | 6.9 cm (one tick ahead by design) | 0.0 / 0.2 cm | 2 mm |

- Filtering only adds lag.
- One tick of prediction would remove about 2 cm of median latency error. That is an order of magnitude below the edge agreement above, so the overlay draws raw positions and does not extrapolate.
- Both filters and prediction remain available in `sideeval` for re-evaluation on other data.

## Stale data, disconnects and session changes

- **Ticks stop for 0.3 s while driving:** the reference turns grey with a slash. Covered by a synthetic test.
- **Our own position becomes undefined:** this also shows as unavailable. Live, it happened on 15 single ticks (0.25 s in 66.6 min). Each was the tick where our own lap fraction reads −1 during a tow or reset to the pits, or the first tick after getting in the car.
- **Lap fractions just past 1 at the start/finish line:** treated as valid and wrapped.
- **No ticks for 10 s:** the sim is treated as gone. The window clears, the memory map is released and reopened when the sim returns.
- **Session number, session id, player car or replay jump changes:** the estimator resets all per-car history. Covered by a unit test.
- **A new event without a sim restart:** recordings now start a new file. One capture spanned two events before this was fixed.
- **RaceLab-style graceful stop:** `taskkill /PID` (no `/F`) closed the demo overlay in about 0.6 s, with settings saved.

## Performance

The figures below were recorded with `--stats` while the overlay animated its demo at 60 Hz and also read live telemetry from a running session. That is a heavier load than racing.

| Measure | Result |
| --- | --- |
| Startup to first frame on screen | 14.5 ms |
| CPU | 1.1 % of one core over 122 s |
| Peak working set / private memory | 18 MB / 2.4 MB |
| Telemetry decode and estimate per tick | median 9 µs, p99 33 µs |
| Draw and present per frame | median 0.24 ms, p99 1.1 ms |
| Executable | 0.59 MB, no DLLs beyond Windows |

**Not yet measured:**

- the iRacing frame-time comparison with and without the overlay
- live-mode telemetry-to-screen latency inside a driving session

## Untested

- three-wide and two cars on one side with real traffic (synthetic tests only)
- other tracks and other car models, so the cross-model reference point uncertainty (σ = 0.3 m) is an assumption
- reversed ovals, where the spotter's left and right may map to low and high
- pit entry and exit with traffic on the track alongside
- spins and reversing beside another car (unit tests only)
- `irsdkLogAllCars=1`
- replays and spectating as a reference car
- independent visual measurement from replay footage
