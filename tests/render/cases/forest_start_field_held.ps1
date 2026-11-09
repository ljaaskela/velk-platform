# The demo's opening with the camera held still, the control for forest_start_field.
@{
    Description = "Forest demo opening, camera held: patches of the probe field without motion"
    Exe = "velk_ui_scale.exe"
    Args = @("250000", "0", "0", "nature", "fixedstep", "tourhold=60")
    Env = @{ VELK_TEST_FLICKER_SOURCE = "field"; VELK_TEST_TILES_UP = "1" }
    Frames = 340
    Warm = 240
    Checks = @(
        @{ Metric = "flicker.patch_tiles"; Stat = "max"; Max = 1 }
    )
}
