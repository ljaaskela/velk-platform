# The camera drops to just over the ground, so the finest GI level steps down several times.
@{
    Description = "Descent to the ground: level steadiness, field stability, level agreement"
    Exe = "velk_ui_scale.exe"
    Args = @("250000", "0", "0", "nature", "nowind", "view=6338.860,-205.095,-6416.161,-72.894,-35.119,1.000",
             "path=200,900,0,0,3", "groundpoints=6378.86,-6416.161;6300,-6450")
    Frames = 1000
    Warm = 60
    Checks = @(
        @{ Metric = "finest.reversals"; Max = 0 }
        @{ Metric = "field.step_pct"; Max = 3; Known = "GI levels disagree, so a change of finest level jumps" }
        @{ Metric = "field.level_ratio"; Max = 1.25; Known = "coarse GI levels are brighter than fine ones" }
    )
}
