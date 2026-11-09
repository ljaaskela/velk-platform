# Leaves next to the camera and a mountain far away, which once left the mountain unlit.
@{
    Description = "Near leaves, far mountain: the field reaches what is seen"
    Exe = "velk_ui_scale.exe"
    Args = @("250000", "0", "0", "nature", "view=-6903.419,-996.160,10275.212,-79.604,-1.732,1.000")
    Frames = 300
    Warm = 120
    Checks = @(
        @{ Metric = "coverage.missing_pct"; Max = 0.01 }
        @{ Metric = "finest.reversals"; Max = 0 }
    )
}
