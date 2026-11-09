# The demo's opening with the camera held still, the control for forest_start.
@{
    Description = "Forest demo opening, camera held: what the ground changes by without motion"
    Exe = "velk_ui_scale.exe"
    Args = @("250000", "0", "0", "nature", "fixedstep", "tourhold=60")
    Frames = 240
    Warm = 120
    Checks = @(
        @{ Metric = "flicker.up_mean"; Max = 0.006 }
        @{ Metric = "flicker.up_over20"; Max = 0.01 }
        @{ Metric = "flicker.tile_max"; Max = 0.025 }
        @{ Metric = "finest.reversals"; Max = 0 }
    )
}
