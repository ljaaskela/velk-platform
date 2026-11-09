# Render tests

Repeatable render runs that pass or fail. Build first, then from the repository root:

    powershell -ExecutionPolicy Bypass -File tests\render\run.ps1

To run some of them, name them. Each is a file in `cases/`:

    powershell -ExecutionPolicy Bypass -File tests\render\run.ps1 forest_start forest_descent

| Option | Effect |
|---|---|
| `-List` | Lists the tests |
| `-Visible` | Shows the window. Otherwise windows are hidden |
| `-Strict` | Known failures also fail the run |
| `-Out dir` | Where logs and captures go. Default `build\render_tests` |

Each check prints `PASS`, `FAIL` or `KNOWN`, with the measured value and the limit, and `summary.txt` in the output directory keeps the result. The exit code is 1 if anything failed. `KNOWN` means the check is failing for a reason named in its case; it does not fail the run.

## Adding a test

Add a file to `cases/`. Its name is the test's name. It returns:

    @{
        Description = "What this checks"
        Exe = "velk_ui_scale.exe"
        Args = @("250000", "0", "0", "nature", "fixedstep")
        Env = @{ NAME = "value" }
        Frames = 420
        Warm = 90
        Checks = @(
            @{ Metric = "flicker.up_mean"; Max = 0.02 }
            @{ Metric = "finest.reversals"; Stat = "max"; Max = 0; Known = "why it is failing" }
        )
    }

`Frames` is how many frames the run lasts; `Warm` is how many at the start are ignored. A check has `Max`, `Min` or both, an optional `Stat`, and `Known` if it is expected to fail for now. `Env` is optional. A case can instead have `Script` for a script whose output is parsed, like `gi_scenes.ps1`.

Motion has to repeat exactly from run to run, so paths count frames, not time. In the scale sample:

| Argument | Effect |
|---|---|
| `fixedstep` | The tour moves by a fixed step per frame |
| `tourhold=N` | The tour stops at frame N |
| `view=x,y,z,yaw,pitch,distance` | Starts the camera there |
| `path=from,to,dx,dz,above` | With `view=`: moves the camera by dx, dz between frames from and to, ending `above` units over the ground, or at its starting height if negative |
| `groundpoints=x,z;x,z` | Logs GI at those points on the ground, up to 8 |

Measurements come from the log. A new one is a line the renderer logs when `VELK_TEST_DIAGNOSTICS` is set, parsed in `Read-RunLog` and looked up in `Get-Metric`, both in `lib/metrics.ps1`.

## Metrics

| Metric | Meaning | Default stat |
|---|---|---|
| `flicker.mean`, `flicker.over5`, `flicker.over20` | Change in lighting per frame, or the share of pixels changing by more than 5% or 20%. Followed from frame to frame so camera motion does not count; surfaces that moved are skipped | p95 |
| `flicker.up_mean`, `flicker.up_over5`, `flicker.up_over20` | The same for surfaces facing up or down, such as the ground | p95 |
| `flicker.tile_max` | The worst of 16x9 screen tiles | p95 |
| `flicker.patch_tiles` | The largest connected group of those tiles changing by more than 1%, so a patch cannot hide in an average | p95 |
| `finest.reversals` | Finest GI level steps that undo the step before within 30 frames | |
| `finest.changes` | Finest GI level steps | |
| `field.step_pct` | Largest single-frame change of the GI field at the points in `groundpoints=`, in percent | |
| `field.level_ratio` | Brightest GI level over darkest at those points, at the end of the run | |
| `coverage.missing_pct` | Share of visible surface that no probe can light | max |
| `image.reference_mean` | Mean difference from a `Reference` image, out of 255 | |
| `gpu.<pass>` | Median GPU time of a pass, in ms | |
| `gitest.<row>.leak`, `.unevenness`, `.mean`, `.p99` | Rows printed by `samples/scale/gi_test.ps1` | |

A stat is one of `p50`, `p95`, `max`, `min`, `mean`, `last`.

Limits are set from measurements and guard against getting worse. They do not say a result is correct.
