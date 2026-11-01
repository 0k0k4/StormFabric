param(
    [ValidateSet('Check', 'Run', 'Restore')][string]$Mode = 'Check',
    [string]$GameDirectory = (Join-Path $PSScriptRoot '..\..\..\Stormworks'),
    [string]$NativeDataDirectory = (Join-Path $env:APPDATA 'Stormworks\data')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Copied next to manifest.json. Observation-only bundles never install fixtures.
# The separately labeled SMALL experiment creates ONLY new, hash-checked test
# files; existing mods/saves are never replaced. Changed test files are retained.
function Hash([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    }
    finally { if ($null -ne $stream) { $stream.Dispose() }; $algorithm.Dispose() }
}
function Require-Hash([string]$Path, [string]$Expected) {
    if ($Expected -notmatch '^[a-f0-9]{64}$' -or (Hash $Path) -ne $Expected) {
        throw "Hash mismatch: $Path. No file will be silently replaced."
    }
}
function Require-GameStopped {
    if (Get-Process -Name 'stormworks64' -ErrorAction SilentlyContinue) {
        throw 'Close Stormworks completely first. No running game will be killed.'
    }
    foreach ($running in @(Get-Process -Name 'Launcher' -ErrorAction SilentlyContinue)) {
        # An unreadable launcher identity is ambiguous: do not replace a DLL.
        if (!$running.Path -or $running.Path -eq $launcher) {
            throw 'Close this Stormworks Launcher first. No running launcher will be killed.'
        }
    }
}
function Write-Json([string]$Path, $Value) {
    $Value | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Path -Encoding UTF8
}

$bundle = [IO.Path]::GetFullPath($PSScriptRoot)
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$exe = Join-Path $game 'stormworks64.exe'
$launcher = Join-Path $game 'Launcher.exe'
$installed = Join-Path $game 'iphlpapi.dll'
$candidate = Join-Path $bundle 'iphlpapi.dll'
$manifest = Get-Content -LiteralPath (Join-Path $bundle 'manifest.json') -Raw | ConvertFrom-Json
$small = $manifest.kind -eq 'swml-custom-display-small-resolution-experiment-v1'
$vehicle = $manifest.kind -eq 'swml-custom-display-vehicle-ui-observation-v1'
if (!$small -and !$vehicle -and $manifest.kind -ne 'swml-custom-display-pass-through-observation-v1') {
    throw 'Not a recognized diagnostic bundle.'
}
$nativeData = [IO.Path]::GetFullPath($NativeDataDirectory)
$fixturePaths = @(
    'mods/swml_cd_resolution_test/mod.xml',
    'mods/swml_cd_resolution_test/data/components/26f8c812f42cd41a9173ae6030881eb9.bin',
    'vehicles/SWML_Resolution_Test_24x16.xml',
    'vehicles/SWML_Resolution_Test_24x16/26f8c812f42cd41a9173ae6030881eb9.bin'
)
$activePath = Join-Path $bundle 'active.json'
$lock = $null
try {
    # No two invocations may operate on this bundle's transaction at once.
    $lock = [IO.File]::Open((Join-Path $bundle 'transaction.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    Require-GameStopped
    Require-Hash $exe $manifest.executableSha256

    function Restore-Transaction {
        Require-GameStopped
        $active = Get-Content -LiteralPath $activePath -Raw | ConvertFrom-Json
        if ($active.gameDirectory -ne $game -or $active.originalSha256 -ne $manifest.originalLoaderSha256 -or
            $active.observerSha256 -ne $manifest.observerSha256 -or
            $active.run -notmatch '^run-[0-9]{8}-[0-9]{6}-[a-f0-9]{32}$') {
            throw 'Pending receipt does not match this game/bundle; refusing automatic restore.'
        }
        $runDirectory = Join-Path (Join-Path $bundle 'runs') $active.run
        $backup = Join-Path $runDirectory 'original-iphlpapi.dll'
        Require-Hash $backup $manifest.originalLoaderSha256
        $currentHash = Hash $installed
        if ($currentHash -ne $manifest.observerSha256 -and $currentHash -ne $manifest.originalLoaderSha256) {
            throw 'Installed DLL changed since the test. Refusing to overwrite another change; backup retained.'
        }
        $log = Join-Path $game 'StormworksModLoader.log'
        if (Test-Path -LiteralPath $log) {
            Copy-Item -LiteralPath $log -Destination (Join-Path $runDirectory 'observation.log') -Force
        }
        if ($currentHash -ne $manifest.originalLoaderSha256) {
            # Atomic, same-volume replacement; restore only the verified original.
            $temporary = Join-Path $game ('.swml-restore-' + [guid]::NewGuid().ToString('N') + '.dll')
            Copy-Item -LiteralPath $backup -Destination $temporary
            try { [IO.File]::Replace($temporary, $installed, [NullString]::Value) }
            finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
        }
        Require-Hash $installed $manifest.originalLoaderSha256
        $retainedFixtures = @()
        if ($small) {
            if ($active.nativeDataDirectory -ne $nativeData) { throw 'Fixture data directory differs; receipt retained.' }
            foreach ($fixture in @($active.createdFixtures)) {
                if ($fixture.path -notin $fixturePaths -or
                    $fixture.sha256 -ne $manifest.fixtures.($fixture.path)) {
                    throw 'Unrecognized fixture receipt; refusing cleanup.'
                }
            }
            foreach ($fixture in @($active.createdFixtures)) {
                $target = Join-Path $nativeData $fixture.path
                if (Test-Path -LiteralPath $target) {
                    if ((Hash $target) -eq $fixture.sha256) { Remove-Item -LiteralPath $target }
                    else { $retainedFixtures += $fixture.path }
                }
            }
            # Named directories only, nonrecursive removals, and only if empty.
            foreach ($relative in @('mods/swml_cd_resolution_test/data/components',
                'mods/swml_cd_resolution_test/data', 'mods/swml_cd_resolution_test',
                'vehicles/SWML_Resolution_Test_24x16')) {
                $directory = Join-Path $nativeData $relative
                if ((Test-Path -LiteralPath $directory -PathType Container) -and
                    !((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint) -and
                    !@(Get-ChildItem -LiteralPath $directory -Force).Count) {
                    Remove-Item -LiteralPath $directory
                }
            }
            if ($retainedFixtures.Count) { Write-Warning ('Modified test files retained: ' + ($retainedFixtures -join ', ')) }
        }
        Write-Json (Join-Path $runDirectory 'restored.json') @{
            utc = [DateTime]::UtcNow.ToString('o'); installedSha256 = Hash $installed
            retainedFixtures = $retainedFixtures
        }
        Remove-Item -LiteralPath $activePath
        Write-Host "Original loader restored and verified. Evidence: $runDirectory" -ForegroundColor Green
    }

    if ($Mode -eq 'Restore') {
        if (!(Test-Path -LiteralPath $activePath)) { throw 'No pending observation session to restore.' }
        Restore-Transaction
        return
    }
    Require-Hash $candidate $manifest.observerSha256
    Require-Hash $launcher $manifest.launcherSha256
    if (Test-Path -LiteralPath $activePath) {
        throw 'A prior session is pending. Close the game, then run RESTAURER.bat first.'
    }
    Require-Hash $installed $manifest.originalLoaderSha256
    if ($small) {
        if (!(Test-Path -LiteralPath $nativeData -PathType Container)) { throw 'Native data directory missing.' }
        if (@($manifest.fixtures.PSObject.Properties).Count -ne $fixturePaths.Count) { throw 'Incomplete experiment fixtures.' }
        foreach ($relative in $fixturePaths) {
            Require-Hash (Join-Path (Join-Path $bundle 'fixtures') $relative) $manifest.fixtures.$relative
            if (Test-Path -LiteralPath (Join-Path $nativeData $relative)) { throw "Test fixture already exists: $relative. No overwrite." }
        }
        foreach ($relative in @('mods/swml_cd_resolution_test', 'vehicles/SWML_Resolution_Test_24x16')) {
            if (Test-Path -LiteralPath (Join-Path $nativeData $relative)) { throw "Test directory already exists: $relative. No overwrite." }
        }
    }
    Write-Host 'Preflight passed: game stopped; executable, original loader and test DLL verified.'
    if ($Mode -eq 'Check') { return }

    $run = 'run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N')
    $runDirectory = Join-Path (Join-Path $bundle 'runs') $run
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $backup = Join-Path $runDirectory 'original-iphlpapi.dll'
    Copy-Item -LiteralPath $installed -Destination $backup
    Require-Hash $backup $manifest.originalLoaderSha256
    $priorLog = Join-Path $game 'StormworksModLoader.log'
    if (Test-Path -LiteralPath $priorLog) {
        Copy-Item -LiteralPath $priorLog -Destination (Join-Path $runDirectory 'previous.log')
    }
    $receipt = @{
        run = $run; gameDirectory = $game; originalSha256 = $manifest.originalLoaderSha256
        observerSha256 = $manifest.observerSha256; utc = [DateTime]::UtcNow.ToString('o')
        nativeDataDirectory = $nativeData; createdFixtures = @()
    }
    Write-Json $activePath $receipt
    $process = $null
    $launchAccepted = $false
    $gameObserved = $false
    try {
        Require-GameStopped
        Require-Hash $installed $manifest.originalLoaderSha256
        if ($small) {
            foreach ($relative in $fixturePaths) {
                $source = Join-Path (Join-Path $bundle 'fixtures') $relative
                Require-Hash $source $manifest.fixtures.$relative
                $target = Join-Path $nativeData $relative
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
                [IO.File]::Copy($source, $target, $false) # Create-new: never overwrite a user's file.
                $receipt.createdFixtures += @{ path = $relative; sha256 = $manifest.fixtures.$relative }
                Write-Json $activePath $receipt
            }
        }
        $temporary = Join-Path $game ('.swml-observe-' + [guid]::NewGuid().ToString('N') + '.dll')
        Copy-Item -LiteralPath $candidate -Destination $temporary
        try { [IO.File]::Replace($temporary, $installed, [NullString]::Value) }
        finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
        Require-Hash $installed $manifest.observerSha256
        $start = New-Object System.Diagnostics.ProcessStartInfo
        $start.FileName = $launcher; $start.WorkingDirectory = $game; $start.UseShellExecute = $false
        $start.EnvironmentVariables['SWML_CD_OBSERVE'] = '1'
        $start.EnvironmentVariables.Remove('SWML_CD_VEHICLE_OBSERVE')
        # Explicitly remove inherited experiment opt-in for an observation-only run.
        $start.EnvironmentVariables.Remove('SWML_CD_SMALL_RESOLUTION')
        if ($vehicle) {
            $start.EnvironmentVariables.Remove('SWML_CD_OBSERVE')
            $start.EnvironmentVariables['SWML_CD_VEHICLE_OBSERVE'] = '1'
            Write-Host 'VEHICLE UI OBSERVATION: enter vehicle editor, select a stock block in the full catalogue, open/cancel Load, leave and re-enter editor. Then RETURN TO MAIN MENU (keep the game running), reload the same save, enter editor and select a catalogue block again. Finally quit game/launcher. No custom dialog or resolution experiment is enabled. No save required.' -ForegroundColor Cyan
        } elseif ($small) {
            $start.EnvironmentVariables['SWML_CD_SMALL_RESOLUTION'] = '1'
            Write-Host 'SMALL RESOLUTION TEST: load SWML_Resolution_Test_24x16, spawn it, check camera and quality settings. Do NOT save over your own vehicle. Then quit game/launcher.' -ForegroundColor Cyan
        } else {
            Write-Host 'Observation only: load the witness vehicle, spawn it, change monitor quality, return to editor, then QUIT the game.' -ForegroundColor Cyan
        }
        Write-Host 'Starting via Launcher.exe; waiting for the actual game, not just the launcher.'
        $process = [Diagnostics.Process]::Start($start)
        $launchAccepted = $true
        $deadline = [DateTime]::UtcNow.AddSeconds(120)
        while ($true) {
            $runningGames = @(Get-Process -Name 'stormworks64' -ErrorAction SilentlyContinue)
            if ($runningGames.Count -gt 0) {
                $gameObserved = $true
                foreach ($running in $runningGames) { $running.WaitForExit(); $running.Dispose() }
            }
            if ($gameObserved -and $process.HasExited -and
                !(Get-Process -Name 'stormworks64' -ErrorAction SilentlyContinue)) { break }
            if (!$gameObserved -and [DateTime]::UtcNow -ge $deadline) {
                throw 'Game not observed within 120 seconds. Receipt retained: close launcher/game, then run RESTAURER.bat.'
            }
            Start-Sleep -Milliseconds 250
        }
    }
    finally {
        if ((Get-Process -Name 'stormworks64' -ErrorAction SilentlyContinue) -or
            ($null -ne $process -and !$process.HasExited) -or ($launchAccepted -and !$gameObserved)) {
            Write-Warning 'Game/launcher still running or game startup unconfirmed: no DLL touched. After quitting both, run RESTAURER.bat.'
        } else { Restore-Transaction }
        if ($null -ne $process) { $process.Dispose() }
    }
}
finally { if ($null -ne $lock) { $lock.Dispose() } }
