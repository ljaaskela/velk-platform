# The demo's opening, measuring the probe field alone on surfaces facing up or down.
@{
    Description = "Forest demo opening: patches of the probe field changing together"
    Exe = "velk_ui_scale.exe"
    Args = @("250000", "0", "0", "nature", "fixedstep")
    Env = @{ VELK_TEST_FLICKER_SOURCE = "field"; VELK_TEST_TILES_UP = "1" }
    Frames = 420
    Warm = 90
    Checks = @(
        @{ Metric = "flicker.patch_tiles"; Stat = "max"; Max = 1; Known = "patches of the probe field change together while the camera moves, none when it is held" }
    )
}
