# The street corner at night, many coloured lights and a still camera.
@{
    Description = "Bistro night at the street corner: noise at rest with many lights"
    Exe = "velk_ui_bistro.exe"
    Args = @()
    Env = @{ VELK_BISTRO_VIEW = "312.300,-7.700,26.953"; VELK_BISTRO_NIGHT = "1" }
    Frames = 300
    Warm = 150
    Checks = @(
        @{ Metric = "flicker.mean"; Max = 0.033 }
        @{ Metric = "finest.reversals"; Max = 0 }
        @{ Metric = "coverage.missing_pct"; Max = 0.01 }
    )
}
