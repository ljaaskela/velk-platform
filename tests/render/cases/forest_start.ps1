# The forest demo's opening, near the treeline, with the tour on a fixed step so runs repeat.
@{
    Description = "Forest demo opening: flicker, level steadiness, coverage"
    Exe = "velk_ui_scale.exe"
    Args = @("250000", "0", "0", "nature", "fixedstep", "groundpoints=8417,0;8350,300;8300,700")
    Frames = 420
    Warm = 90
    Checks = @(
        @{ Metric = "flicker.up_mean"; Max = 0.007; Known = "the ground changes about 3 times as much moving as held, see forest_start_held" }
        @{ Metric = "flicker.up_over20"; Max = 0.012; Known = "the ground changes about 3 times as much moving as held, see forest_start_held" }
        @{ Metric = "flicker.tile_max"; Max = 0.03; Known = "the ground changes about 3 times as much moving as held, see forest_start_held" }
        @{ Metric = "finest.reversals"; Max = 0 }
        @{ Metric = "coverage.missing_pct"; Max = 0.01 }
    )
}
