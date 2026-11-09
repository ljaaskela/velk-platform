# The GI test scenes in samples/scale/gi_test.cpp, scored by samples/scale/gi_test.ps1.
@{
    Description = "GI test scenes against their references"
    Script = "samples\scale\gi_test.ps1"
    Checks = @(
        @{ Metric = "gitest.leak_light.leak"; Max = 0.05 }
        @{ Metric = "gitest.leak_sky.leak"; Max = 0.3 }
        @{ Metric = "gitest.blotch.unevenness"; Max = 1.5 }
        @{ Metric = "gitest.size_10.mean"; Max = 0.5 }
        @{ Metric = "gitest.size_100.mean"; Max = 0.5 }
        @{ Metric = "gitest.outlier.mean"; Max = 0.5 }
        @{ Metric = "gitest.shapes_mesh.mean"; Max = 0.5 }
        @{ Metric = "gitest.move.mean"; Max = 1 }
        @{ Metric = "gitest.deform.mean"; Max = 1 }
        @{ Metric = "gitest.lights_64.mean"; Max = 4 }
        @{ Metric = "gitest.dense.mean"; Max = 3.5 }
        @{ Metric = "gitest.origin_1e5.mean"; Max = 1 }
        @{ Metric = "gitest.origin_1e6.mean"; Max = 1 }
        @{ Metric = "gitest.origin_1e7.mean"; Max = 1 }
    )
}
