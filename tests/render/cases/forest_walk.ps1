# Across the ground among the trees, with wind, from a spot with leaves next to the camera.
@{
    Description = "Moving through trees: level steadiness, flicker, coverage"
    Exe = "velk_ui_scale.exe"
    Args = @("250000", "0", "0", "nature", "view=-6903.419,-996.160,10275.212,-79.604,-1.732,1.000",
             "path=100,500,150,0,-1", "groundpoints=-6880,10275;-6800,10275")
    Frames = 520
    Warm = 90
    Checks = @(
        @{ Metric = "finest.reversals"; Max = 0 }
        @{ Metric = "flicker.up_mean"; Max = 0.015; Known = "the ground changes more while the camera moves, see forest_start_held" }
        @{ Metric = "coverage.missing_pct"; Max = 0.01 }
    )
}
