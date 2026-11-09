# A still camera in a still forest, so anything that changes between frames is noise.
@{
    Description = "Still forest: noise at rest, closeness to the path traced reference"
    Exe = "velk_ui_scale.exe"
    Args = @("250000", "0", "0", "nature", "nowind", "view=6338.860,-205.095,-6416.161,-72.894,-35.119,1.000")
    Frames = 400
    Warm = 200
    Checks = @(
        @{ Metric = "flicker.mean"; Max = 0.005 }
        @{ Metric = "flicker.over20"; Max = 0.003 }
        @{ Metric = "finest.reversals"; Max = 0 }
        @{ Metric = "image.reference_mean"; Reference = "reference_images\ref_forest1.png"; Max = 36 }
    )
}
