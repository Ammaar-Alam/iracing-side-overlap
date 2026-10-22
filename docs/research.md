# Estimator design and research notes

Each entry gives the problem, the method borrowed, the assumptions, the experiment and the decision. Measured numbers come from `sideeval` runs on live captures and are collected in [validation.md](validation.md). The facts about iRacing's interface are in [telemetry.md](telemetry.md).

## 1. Occupancy and position are separate questions

**Problem.** iRacing tells the player which sides are occupied (`CarLeftRight`) and where every car is around the lap (`CarIdxLapDistPct`). Nothing says which car the spotter means, or where any car sits sideways.

**Design.** The overlay answers three questions separately:

1. Is a side occupied? Only `CarLeftRight` answers this. A clear spotter hides the bars even when lap distance suggests an overlap, and an occupied spotter lights the lane even when no car can be matched.
2. Which car accounts for it? This is an association between candidate cars and the spotter's counts (section 3).
3. Where is that car, front to back? Lap distance answers this, with an uncertainty that grows in situations where lap distance stops describing the car's frame (section 2). The bar is drawn only when the association is confident and the uncertainty is small. Otherwise the lane is lit without a position.

**Evidence.** Several open-source radars draw whichever car is nearest by lap distance, or keep drawing a remembered car after the spotter says clear. Both put a confident-looking bar where the data does not support one.

## 2. Lap distance against car-frame geometry (Frenet frame)

**Reference.** M. Werling, J. Ziegler, S. Kammel, S. Thrun, "Optimal Trajectory Generation for Dynamic Street Scenarios in a Frenét Frame," ICRA 2010. Positions are written as arc length *s* along a reference path and lateral offset *d* from it.

**Problem.** `CarIdxLapDistPct × TrackLength` is the *s* coordinate of each car along iRacing's track reference. The overlay wants the other car's offset along *our* car's axis. Those differ in three ways:

- **Curvature.** At lateral offset *d* from a path with curvature κ, a step of *ds* covers (1 − κd)·*ds* of actual distance. Two cars side by side at the same *s* are level, but a gap Δ*s* between cars on different lines in a 30 m hairpin can be off by more than 10 %.
- **Heading.** If our car points at angle ψ to the path, a car beside us at lateral distance *w* shifts along our axis by about *w*·sin ψ. At *w* ≈ 2.2 m that is 0.2 m at 5° and 0.6 m at 15°.
- **Reference point.** Which point on each car `CarIdxLapDistPct` refers to is not documented. For two cars of the same model the offsets cancel. Between models they do not.

**What is observable.** *d* for opponents is not published, so these effects cannot be corrected, only bounded. Our own heading and path curvature are observable: curvature is about `YawRate / Speed` and slip is `atan2(VelocityY, VelocityX)`.

**Method.** Each candidate gets a geometric standard deviation

σ_geom² = (|Δs|·κ·w)² + (w·sin β)² + σ_model²

- *w* = 2.2 m is a nominal side-by-side spacing.
- β is our slip angle.
- σ_model = 0.3 m applies only between different car models.

Reversing, slip beyond about 29°, and a neighbour crawling or going backwards along the track all mark the position as unplaceable. A bar is drawn only while its total σ is at most 0.6 m.

**Experiment.** The spotter computes overlap from the sim's own car geometry, which this overlay cannot see. Every isolated flip of `CarLeftRight` is therefore an independent check of lap distance: at the flip, |Δs| should equal the combined half lengths, plus the padding on the way out. The errors are split by curvature to test whether corners degrade the estimate as modelled.

## 3. Which car is beside us (probabilistic data association)

**Reference.** T. Fortmann, Y. Bar-Shalom, M. Scheffe, "Sonar Tracking of Multiple Targets Using Joint Probabilistic Data Association," IEEE Journal of Oceanic Engineering 8(3), 1983. JPDA enumerates joint assignments of measurements to targets, excludes infeasible ones, and turns the remaining likelihoods into association probabilities.

**Adaptation.** Here the measurement is not a position. It is the spotter's count of cars per side. Each joint hypothesis assigns every candidate within reach (at most 4, so at most 81 hypotheses) to left, right or neither. A hypothesis is feasible only if its counts match `CarLeftRight`, where "both sides" means at least one per side and "two cars left" means at least two on the left. The likelihood terms are:

- **Overlap.** A logistic function of how far inside the overlap span the car sits by lap distance. Its softness widens with that car's σ.
- **Out of range.** A small probability (0.03) that an overlapping car is outside the spotter's lateral reach, for example in the grass or on a parallel pit lane.
- **Phantom.** A small probability (0.02) of an occupied side with no matching candidate, playing the role of JPDA's missed detection and clutter terms.
- **Side persistence.** A car that has stayed alongside cannot have crossed to the other side, since it would have had to pass through us (probability 10⁻⁴). A car that dropped out of overlap may come back on either side, with its remembered side preferred (0.2 for a switch).

Unlike JPDA, the overlay never mixes candidates into an averaged position. A blend of two cars would be a precise-looking bar describing neither. It takes the most probable set of cars per side and draws it only when that set has probability of at least 0.9 and no phantom.

**Why not a full multi-target tracker.** Candidates are few, the hypothesis space is tiny, and the only per-car state that matters is a remembered side. Full JPDA machinery such as gating ellipses and measurement covariance mixing would add code without adding information.

**Experiment.** Live association can only be checked against what the same data reveals later. `sideeval` runs the estimator a second time backwards in time. Where that hindsight pass is confident about a side, it labels the true car. Disagreements between the live pass and the hindsight labels are counted, and so are fallback ticks hindsight could have resolved. Both passes use the same signals, so this measures consistency. A replay review with camera evidence is the independent check.

## 4. Filtering and prediction (Kalman 1960, alpha-beta)

**Reference.** R. E. Kalman, "A New Approach to Linear Filtering and Prediction Problems," Journal of Basic Engineering 82(1), 1960.

**Candidates.** Three filters are run on each car's relative position:

- **Raw**: the position as measured, with velocity from successive differences.
- **Alpha-beta** (α = 0.5, β = 0.1).
- **Constant-velocity Kalman**: white relative acceleration with σ = 8 m/s², measurement σ = 0.05 m. It carries an explicit covariance.

All three reset when the innovation exceeds 3 m, which catches tows, resets and network corrections, and when sim time jumps or runs backwards. Optional prediction extrapolates up to 0.5 m.

**Experiment.** The useful questions are whether there is noise to remove and how much latency there is to hide. `sideeval` measures four things:

- the tick-to-tick change in each nearby car's step, which is the noise floor
- how far the drawn bar sits from that tick's raw offset, which is lag
- how far it sits from the next tick's raw offset, which is the error after one tick of latency
- the second difference of the drawn position, which is roughness

## 5. Timing

- Reads are event-driven on iRacing's data-valid event, and every row is copied coherently: the tick count is checked after the copy.
- Host time comes from `QueryPerformanceCounter`. Sim time comes from `SessionTime`, which stops when paused and jumps in replays.
- Staleness is judged on host time. Without a new tick for 0.3 s the display switches to the unavailable state. After 10 s without ticks the overlay treats the sim as gone and draws nothing.
- Filter steps use sim time, so a pause neither integrates velocity nor counts as stale data.
