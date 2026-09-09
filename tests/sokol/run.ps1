param([int]$AudioFrames = 120)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '../..')
$root = (Get-Location).Path
$testDir = Join-Path $root ('build/tests/sokol-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDir | Out-Null
$sources = @(Get-ChildItem src/*.c | Where-Object Name -ne 'gui_main.c' | ForEach-Object FullName)
foreach ($name in @('frontend_integration','menu_mouse')) {
    & gcc -Wall -Wextra -std=c11 -O2 -Isrc "tests/sokol/$name.c" @sources -o "$testDir/$name.exe" -static -luser32 -lgdi32 -lwinmm -lole32 -lshell32 -ld3d11 -ldxgi
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
}
$core = @($sources | Where-Object { (Split-Path $_ -Leaf) -notin @('debugger.c','host_sokol.c') })
& gcc -Wall -Wextra -Werror -std=c11 -O2 -Isrc tests/sokol/rendering.c src/host_sokol.c -o "$testDir/rendering.exe" -static -luser32 -lgdi32 -lwinmm -lole32 -lshell32 -ld3d11 -ldxgi
if ($LASTEXITCODE -ne 0) { throw 'Rendering test compilation failed' }
& gcc -Wall -Wextra -Werror -std=c11 -O2 -Isrc tests/sokol/zapper_probe.c @core -o "$testDir/zapper_probe.exe" -lm
if ($LASTEXITCODE -ne 0) { throw 'Zapper probe compilation failed' }
& "$testDir/zapper_probe.exe"
if ($LASTEXITCODE -ne 0) { throw 'Zapper probe failed' }
$priorFrames = $env:NES_SOKOL_AUDIO_FRAMES
$priorDisable = $env:NES_DISABLE_AUDIO
try {
    $env:NES_SOKOL_AUDIO_FRAMES = "$AudioFrames"
    foreach ($mode in @('rendering','mouse','audio','muted','unavailable')) {
        $dir = Join-Path $testDir $mode
        New-Item -ItemType Directory -Path $dir | Out-Null
        $name = if ($mode -eq 'mouse') { 'menu_mouse' } elseif ($mode -eq 'rendering') { 'rendering' } else { 'frontend_integration' }
        Copy-Item -LiteralPath "$testDir/$name.exe" -Destination $dir
        $env:NES_DISABLE_AUDIO = if ($mode -eq 'unavailable') { '1' } else { $null }
        $process = Start-Process -FilePath "$dir/$name.exe" -ArgumentList $mode -WorkingDirectory $dir -WindowStyle Hidden -PassThru -RedirectStandardOutput "$dir/stdout.log" -RedirectStandardError "$dir/stderr.log"
        $null = $process.Handle
        if (-not $process.WaitForExit([Math]::Max(30000, $AudioFrames * 30))) {
            $process.Kill()
            throw "Timed out: $mode"
        }
        $process.WaitForExit()
        Get-Content "$dir/stdout.log"
        Get-Content "$dir/stderr.log"
        if ($process.ExitCode -ne 0) { throw "Failed: $mode (exit $($process.ExitCode))" }
        Write-Output "PASS: $mode"
    }
} finally {
    $env:NES_SOKOL_AUDIO_FRAMES = $priorFrames
    $env:NES_DISABLE_AUDIO = $priorDisable
}
Write-Output "Sokol frontend checks passed. Captures: $testDir"
