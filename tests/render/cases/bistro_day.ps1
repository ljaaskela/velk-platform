# The street corner by day, with a still camera.
@{
    Description = "Bistro day at the street corner: noise at rest, coverage"
    Exe = "velk_ui_bistro.exe"
    Args = @()
    Env = @{ VELK_BISTRO_VIEW = "312.300,-7.700,26.953" }
    Frames = 300
    Warm = 150
    Checks = @(
        @{ Metric = "flicker.mean"; Max = 0.02 }
        @{ Metric = "finest.reversals"; Max = 0 }
        @{ Metric = "coverage.missing_pct"; Max = 0.01 }
    )
}
