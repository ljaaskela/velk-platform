# Runs every GI test scene from gi_test.cpp and scores the captures. Build first.
#
#   powershell -File samples\scale\gi_test.ps1 [-Frames 240] [-Out dir]
#
# leak rows: brightness of a scene that should be black, as a fraction of its lit
# counterpart. 0 is correct.
# Other rows: difference from the reference, out of 255: mean, 99th percentile, and the
# percentage of pixels off by more than 8. noise is the same scene a few frames later, so
# differences near it say nothing.

param(
    [int]$Frames = 240,
    [string]$Out = (Join-Path $env:TEMP "velk_gitest")
)

$bin = Join-Path $PSScriptRoot "..\..\build\bin"
$exe = Join-Path $bin "velk_ui_scale.exe"
New-Item -ItemType Directory -Force $Out | Out-Null

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class GiScore {
    static byte[] Load(string path, out int w, out int h, out int stride) {
        using (var bmp = new Bitmap(path)) {
            w = bmp.Width; h = bmp.Height;
            var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            stride = data.Stride;
            var bytes = new byte[stride * h];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            bmp.UnlockBits(data);
            return bytes;
        }
    }
    public static double Mean(string path) {
        int w, h, s; var p = Load(path, out w, out h, out s);
        double sum = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int i = y * s + x * 3;
            sum += 0.0722 * p[i] + 0.7152 * p[i + 1] + 0.2126 * p[i + 2];
        }
        return sum / (w * h);
    }
    // Mean difference from the surrounding 11x11 average, inside a disc. Smooth shading
    // scores low, blotches high.
    public static double Unevenness(string path, int cx, int cy, int radius) {
        int w, h, s; var p = Load(path, out w, out h, out s);
        Func<int, int, double> lum = (x, y) => { int i = y * s + x * 3; return 0.0722 * p[i] + 0.7152 * p[i + 1] + 0.2126 * p[i + 2]; };
        double sum = 0; int count = 0;
        for (int y = cy - radius; y <= cy + radius; ++y) for (int x = cx - radius; x <= cx + radius; ++x) {
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > radius * radius) continue;
            double avg = 0;
            for (int j = -5; j <= 5; ++j) for (int k = -5; k <= 5; ++k) avg += lum(x + k, y + j);
            sum += Math.Abs(lum(x, y) - avg / 121.0); ++count;
        }
        return sum / count;
    }
    public static double[] Diff(string a, string b) {
        int w, h, s, w2, h2, s2;
        var p = Load(a, out w, out h, out s);
        var q = Load(b, out w2, out h2, out s2);
        var counts = new long[256];
        double sum = 0; long over = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int i = y * s + x * 3;
            int d = Math.Max(Math.Max(Math.Abs(p[i] - q[i]), Math.Abs(p[i + 1] - q[i + 1])), Math.Abs(p[i + 2] - q[i + 2]));
            counts[d]++; sum += d; if (d > 8) over++;
        }
        long total = (long)w * h, seen = 0; int p99 = 255;
        for (int d = 0; d < 256; ++d) { seen += counts[d]; if (seen >= total * 0.99) { p99 = d; break; } }
        return new double[] { sum / total, p99, 100.0 * over / total };
    }
}
"@

$env:VELK_AUTO_GPU_STATS = "1"
Remove-Item Env:\VELK_VK_VALIDATION -ErrorAction SilentlyContinue
$timings = @{}
$problems = @()

function Run-Test([string]$test, [string]$file, [int]$frames) {
    $env:VELK_AUTO_EXIT_FRAMES = "$frames"
    $env:VELK_AUTO_CAPTURE = (Join-Path $Out "$file.png")
    Push-Location $bin
    $log = cmd /c "`"$exe`" gitest $test $Frames 2>&1"
    Pop-Location
    $probes = $log | Select-String "gpu pass gi\.(trace|shade|maps)\s+med=\s*([0-9.]+)"
    $probe_ms = 0.0
    foreach ($m in $probes) { $probe_ms += [double]$m.Matches[0].Groups[2].Value }
    $light = ($log | Select-String "gpu pass deferred\.lighting\s+med=\s*([0-9.]+)" | Select-Object -First 1)
    $script:timings[$file] = @(
        $(if ($probes) { $probe_ms.ToString("0.000", [Globalization.CultureInfo]::InvariantCulture) } else { "?" }),
        $(if ($light) { $light.Matches[0].Groups[1].Value } else { "?" })
    )
    $bad = $log | Select-String "Validation Error|device lost|\[ERROR"
    if ($bad) { $script:problems += "${file}: $($bad[0])" }
}

$tests = "leak_light", "leak_lit", "leak_sky", "open_sky", "size_1", "size_10", "size_100", "outlier",
         "shapes_mesh", "move", "move_ref", "deform", "deform_ref", "lights_1", "lights_64", "dense", "dense_ref", "origin_1e5", "origin_1e6", "origin_1e7"
foreach ($t in $tests) { Run-Test $t $t $Frames }
Run-Test "size_1" "noise" ($Frames + 7)

function Png([string]$n) { Join-Path $Out "$n.png" }
function Time([string]$n) { "$($timings[$n][0]) / $($timings[$n][1])" }

"{0,-14} {1,-34} {2}" -f "test", "score", "probes / lighting ms"
foreach ($pair in @(@("leak_light", "leak_lit"), @("leak_sky", "open_sky"))) {
    $ratio = [GiScore]::Mean((Png $pair[0])) / [Math]::Max([GiScore]::Mean((Png $pair[1])), 0.001)
    "{0,-14} {1,-34} {2}" -f $pair[0], ("leak {0:F4}" -f $ratio), (Time $pair[0])
}
"{0,-14} {1,-34} {2}" -f "blotch", ("unevenness {0:F2}" -f [GiScore]::Unevenness((Png "size_1"), 545, 357, 70)), (Time "size_1")
$pairs = @(@("noise", "size_1"), @("size_10", "size_1"), @("size_100", "size_1"), @("outlier", "size_1"),
           @("shapes_mesh", "size_1"), @("move", "move_ref"), @("deform", "deform_ref"), @("lights_64", "lights_1"),
           @("dense", "dense_ref"), @("origin_1e5", "size_1"), @("origin_1e6", "size_1"), @("origin_1e7", "size_1"))
foreach ($pair in $pairs) {
    $d = [GiScore]::Diff((Png $pair[0]), (Png $pair[1]))
    "{0,-14} {1,-34} {2}" -f $pair[0], ("mean {0:F2}  p99 {1,3}  over8 {2:F2}%" -f $d[0], $d[1], $d[2]), (Time $pair[0])
}
"{0,-14} {1,-34} {2}" -f "dense_ref", "reference for dense", (Time "dense_ref")
"captures: $Out"
if ($problems.Count -gt 0) { "PROBLEMS:"; $problems } else { "logs clean" }
