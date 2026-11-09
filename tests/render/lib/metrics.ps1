# Reads a test run's log into metrics, which the checks in a case then compare with limits.

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class RenderTestImage {
    static byte[] Bits(Bitmap b, out int stride) {
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        stride = d.Stride; var p = new byte[stride * b.Height];
        Marshal.Copy(d.Scan0, p, 0, p.Length); b.UnlockBits(d); return p;
    }
    // Mean per channel difference, out of 255.
    public static double MeanDifference(string a, string b) {
        using (var x = new Bitmap(a)) using (var y = new Bitmap(b)) {
            if (x.Width != y.Width || x.Height != y.Height) return double.NaN;
            int s1, s2; var p = Bits(x, out s1); var q = Bits(y, out s2);
            double sum = 0;
            for (int j = 0; j < x.Height; j++) for (int i = 0; i < x.Width; i++) {
                int u = j * s1 + i * 3, v = j * s2 + i * 3;
                sum += (Math.Abs(p[u] - q[v]) + Math.Abs(p[u + 1] - q[v + 1]) + Math.Abs(p[u + 2] - q[v + 2])) / 3.0;
            }
            return sum / ((double)x.Width * x.Height);
        }
    }
}
"@

function Get-Stat([double[]]$values, [string]$stat) {
    if (-not $values -or $values.Count -eq 0) { return $null }
    $sorted = $values | Sort-Object
    switch ($stat) {
        "max"  { return $sorted[-1] }
        "min"  { return $sorted[0] }
        "last" { return $values[-1] }
        "mean" { return ($values | Measure-Object -Average).Average }
        "p50"  { return $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.5)] }
        "p95"  { return $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.95)] }
        default { throw "unknown stat $stat" }
    }
}

# Per frame series from test_frame lines, after the frames a case skips to warm up.
function Read-RunLog([string]$path, [int]$warm) {
    $m = @{ series = @{}; finest = New-Object System.Collections.ArrayList; points = @{}; gpu = @{}; errors = @() }
    foreach ($name in "mean", "over5", "over20", "up_mean", "up_over5", "up_over20", "tile_max", "patch_tiles", "missing_pct") {
        $m.series[$name] = New-Object System.Collections.Generic.List[double]
    }
    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -match "\[ERROR|Validation Error|device lost|compilation failed") {
            $m.errors += $line
        }
        if ($line -match "test_frame frame=(\d+) valid=(\d+) mean=([0-9.]+) over5=([0-9.]+) over20=([0-9.]+) up_valid=(\d+) up_mean=([0-9.]+) up_over5=([0-9.]+) up_over20=([0-9.]+) tile_max=([0-9.]+) tested=(\d+) missing=(\d+) patch=(\d+)") {
            if ([int]$Matches[1] -lt $warm -or [int]$Matches[2] -eq 0) { continue }
            $m.series.mean.Add([double]$Matches[3])
            $m.series.over5.Add([double]$Matches[4])
            $m.series.over20.Add([double]$Matches[5])
            if ([int]$Matches[6] -gt 0) {
                $m.series.up_mean.Add([double]$Matches[7])
                $m.series.up_over5.Add([double]$Matches[8])
                $m.series.up_over20.Add([double]$Matches[9])
            }
            $m.series.tile_max.Add([double]$Matches[10])
            $m.series.patch_tiles.Add([double]$Matches[13])
            if ([int]$Matches[11] -gt 0) {
                $m.series.missing_pct.Add(100.0 * [double]$Matches[12] / [double]$Matches[11])
            }
        }
        elseif ($line -match "test_finest frame=(\d+) level=(-?\d+) known=1") {
            if ([int]$Matches[1] -ge $warm) { [void]$m.finest.Add(@([int]$Matches[1], [int]$Matches[2])) }
        }
        elseif ($line -match "gi_print frame=(\d+) point=(\d+) cell=\S+ total=([^,]+),([^,]+),(\S+)(.*)") {
            $frame = [int]$Matches[1]
            if ($frame -lt $warm) { continue }
            $point = $Matches[2]
            if (-not $m.points.ContainsKey($point)) { $m.points[$point] = New-Object System.Collections.ArrayList }
            $levels = @()
            foreach ($l in [regex]::Matches($Matches[6], "L\d\[w=([0-9.]+) ([0-9.]+)\]")) {
                $levels += ,@([double]$l.Groups[1].Value, [double]$l.Groups[2].Value)
            }
            [void]$m.points[$point].Add(@{ frame = $frame; green = [double]$Matches[4]; levels = $levels })
        }
        elseif ($line -match "gpu pass (\S+)\s+med=\s*([0-9.]+)") {
            $m.gpu[$Matches[1]] = [double]$Matches[2]
        }
    }
    return $m
}

# Reads what gi_test.ps1 prints into metrics such as gitest.size_10.mean.
function Read-GiTestLog([string]$path) {
    $values = @{}
    $errors = @()
    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -match "^(\S+)\s+leak ([0-9.]+)") { $values["gitest.$($Matches[1]).leak"] = [double]$Matches[2] }
        elseif ($line -match "^(\S+)\s+unevenness ([0-9.]+)") { $values["gitest.$($Matches[1]).unevenness"] = [double]$Matches[2] }
        elseif ($line -match "^(\S+)\s+mean ([0-9.]+)\s+p99\s+(\d+)\s+over8 ([0-9.]+)%") {
            $values["gitest.$($Matches[1]).mean"] = [double]$Matches[2]
            $values["gitest.$($Matches[1]).p99"] = [double]$Matches[3]
        }
        elseif ($line -match "PROBLEMS") { $errors += $line }
    }
    return @{ values = $values; errors = $errors }
}

function Get-FinestSteps($m) {
    $changes = New-Object System.Collections.ArrayList
    for ($i = 1; $i -lt $m.finest.Count; ++$i) {
        if ($m.finest[$i][1] -ne $m.finest[$i - 1][1]) {
            [void]$changes.Add(@($m.finest[$i][0], $m.finest[$i - 1][1], $m.finest[$i][1]))
        }
    }
    return ,$changes
}

# The value of one check, or $null if the run has no data for it.
function Get-Metric($m, [hashtable]$check, [string]$capture, [string]$root) {
    $name = $check.Metric
    if ($name.StartsWith("gitest.")) { return $m.values[$name] }
    if ($name.StartsWith("flicker.") -or $name -eq "coverage.missing_pct") {
        $stat = if ($check.Stat) { $check.Stat } elseif ($name -eq "coverage.missing_pct") { "max" } else { "p95" }
        $key = if ($name -eq "coverage.missing_pct") { "missing_pct" } else { $name.Substring(8) }
        return Get-Stat ($m.series[$key].ToArray()) $stat
    }
    switch ($name) {
        "finest.changes" { if ($m.finest.Count -eq 0) { return $null }; return (Get-FinestSteps $m).Count }
        "finest.reversals" {
            if ($m.finest.Count -eq 0) { return $null }
            # A step that undoes the one before it within 30 frames.
            $steps = Get-FinestSteps $m
            $count = 0
            for ($i = 1; $i -lt $steps.Count; ++$i) {
                if ($steps[$i][2] -eq $steps[$i - 1][1] -and ($steps[$i][0] - $steps[$i - 1][0]) -lt 30) { ++$count }
            }
            return $count
        }
        "field.step_pct" {
            $largest = $null
            foreach ($samples in $m.points.Values) {
                for ($i = 1; $i -lt $samples.Count; ++$i) {
                    $before = $samples[$i - 1].green
                    if ($before -le 0.001) { continue }
                    $step = 100.0 * [Math]::Abs($samples[$i].green - $before) / $before
                    if ($null -eq $largest -or $step -gt $largest) { $largest = $step }
                }
            }
            return $largest
        }
        "field.level_ratio" {
            # At the end of the run, the brightest level over the darkest at each point.
            $largest = $null
            foreach ($samples in $m.points.Values) {
                if ($samples.Count -eq 0) { continue }
                $seen = @($samples[-1].levels | Where-Object { $_[0] -gt 0.25 -and $_[1] -gt 0.001 } | ForEach-Object { $_[1] })
                if ($seen.Count -lt 2) { continue }
                $sorted = $seen | Sort-Object
                $ratio = [double]$sorted[-1] / [double]$sorted[0]
                if ($null -eq $largest -or $ratio -gt $largest) { $largest = $ratio }
            }
            return $largest
        }
        "image.reference_mean" {
            if (-not $capture -or -not (Test-Path $capture)) { return $null }
            return [RenderTestImage]::MeanDifference($capture, (Join-Path $root $check.Reference))
        }
        default {
            if ($name.StartsWith("gpu.")) { return $m.gpu[$name.Substring(4)] }
            throw "unknown metric $name"
        }
    }
}
