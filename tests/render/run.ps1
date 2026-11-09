# Runs the render tests. See README.md.
#
#   powershell -ExecutionPolicy Bypass -File tests\render\run.ps1 [names...] [-Visible] [-Strict] [-List] [-Out dir]

param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Names = @(),
    [switch]$Visible,
    [switch]$Strict,
    [switch]$List,
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$bin = Join-Path $root "build\bin"
if (-not $Out) { $Out = Join-Path $root "build\render_tests" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
. (Join-Path $PSScriptRoot "lib\metrics.ps1")

$files = Get-ChildItem (Join-Path $PSScriptRoot "cases") -Filter *.ps1 | Sort-Object Name
if ($List) {
    foreach ($f in $files) { "{0,-22} {1}" -f $f.BaseName, (& $f.FullName).Description }
    exit 0
}
if ($Names.Count -gt 0) {
    $unknown = $Names | Where-Object { $files.BaseName -notcontains $_ }
    if ($unknown) { "Unknown test: $($unknown -join ', ')"; exit 2 }
    $files = $files | Where-Object { $Names -contains $_.BaseName }
}

function Invoke-Run([string]$name, [hashtable]$case, [string]$log, [string]$capture) {
    $variables = @{}
    if (-not $Visible) { $variables.VELK_HIDE_WINDOW = "1" }
    if ($case.Script) {
        $script = Join-Path $root $case.Script
        $line = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$script`" -Out `"$(Join-Path $Out $name)`" > `"$log`" 2>&1"
    }
    else {
        $variables.VELK_TEST_DIAGNOSTICS = "1"
        $variables.VELK_GI_PRINT_RATE = "1"
        $variables.VELK_AUTO_GPU_STATS = "1"
        $variables.VELK_AUTO_EXIT_FRAMES = "$($case.Frames)"
        if ($capture) { $variables.VELK_AUTO_CAPTURE = $capture }
        if ($case.Env) { foreach ($k in $case.Env.Keys) { $variables[$k] = "$($case.Env[$k])" } }
        $arguments = ($case.Args | ForEach-Object { "`"$_`"" }) -join " "
        $line = "`"$(Join-Path $bin $case.Exe)`" $arguments > `"$log`" 2>&1"
    }

    $saved = @{}
    foreach ($k in $variables.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k)
        [Environment]::SetEnvironmentVariable($k, $variables[$k])
    }
    Push-Location $bin
    try {
        cmd /s /c "`"$line`""
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    return $code
}

$failed = 0
$known = 0
$passed = 0
$report = New-Object System.Collections.ArrayList

function Write-Result([string]$status, [string]$test, [string]$what, [string]$detail) {
    $text = "{0,-6} {1,-20} {2,-34} {3}" -f $status, $test, $what, $detail
    Write-Output $text
    [void]$script:report.Add($text)
}

foreach ($file in $files) {
    $name = $file.BaseName
    $case = & $file.FullName
    $log = Join-Path $Out "$name.log"
    $needs_capture = @($case.Checks | Where-Object { $_.Reference }).Count -gt 0
    $capture = if ($needs_capture) { Join-Path $Out "$name.png" } else { "" }
    $started = Get-Date
    $code = Invoke-Run $name $case $log $capture
    $seconds = ((Get-Date) - $started).TotalSeconds
    "ran    {0,-20} {1:F0} s" -f $name, $seconds

    if ($case.Script) { $metrics = Read-GiTestLog $log }
    else { $metrics = Read-RunLog $log $(if ($case.Warm) { $case.Warm } else { 60 }) }

    if ($code -ne 0) {
        Write-Result "FAIL" $name "process exit code" "$code (see $log)"
        ++$failed
    }
    if ($metrics.errors.Count -gt 0) {
        Write-Result "FAIL" $name "errors in the log" "$($metrics.errors.Count), first: $(($metrics.errors[0]).Substring(0, [Math]::Min(100, $metrics.errors[0].Length)))"
        ++$failed
    }

    foreach ($check in $case.Checks) {
        $value = Get-Metric $metrics $check $capture $root
        $what = $check.Metric
        if ($check.Stat) { $what = "$what $($check.Stat)" }
        if ($null -eq $value -or [double]::IsNaN([double]$value)) {
            Write-Result "FAIL" $name $what "no data"
            ++$failed
            continue
        }
        $limit = @()
        $ok = $true
        if ($null -ne $check.Max) { $limit += "at most $($check.Max)"; $ok = $ok -and ($value -le $check.Max) }
        if ($null -ne $check.Min) { $limit += "at least $($check.Min)"; $ok = $ok -and ($value -ge $check.Min) }
        $detail = "{0:F4}, limit {1}" -f [double]$value, ($limit -join ", ")
        if ($ok) {
            if ($check.Known) { Write-Result "PASS" $name $what "$detail (was a known failure: $($check.Known))" }
            else { Write-Result "PASS" $name $what $detail }
            ++$passed
        }
        elseif ($check.Known -and -not $Strict) {
            Write-Result "KNOWN" $name $what "$detail ($($check.Known))"
            ++$known
        }
        else {
            Write-Result "FAIL" $name $what $detail
            ++$failed
        }
    }
}

$summary = "{0} passed, {1} known failures, {2} failed" -f $passed, $known, $failed
$summary
$report + $summary | Set-Content (Join-Path $Out "summary.txt")
if ($failed -gt 0) { exit 1 }
exit 0
