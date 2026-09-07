[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('check', 'build-standard', 'build-solar-router', 'native', 'all')]
    [string] $Mode = 'all',
    [string] $Distribution = 'Ubuntu-24.04'
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw 'WSL is required. Install the Ubuntu-24.04 distribution to run the GitHub-equivalent checks.'
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$linuxRoot = & wsl.exe --distribution $Distribution --exec wslpath -a -u $projectRoot
if ($LASTEXITCODE -ne 0 -or -not $linuxRoot) {
    throw "Cannot access this checkout through WSL distribution '$Distribution'."
}
$linuxRoot = ($linuxRoot -join "`n").Trim()

& wsl.exe --distribution $Distribution --exec bash -c 'docker context inspect heltec-ci >/dev/null 2>&1'
$runnerArguments = @('--distribution', $Distribution, '--cd', $linuxRoot, '--exec')
if ($LASTEXITCODE -eq 0) {
    $runnerArguments += @('env', 'DOCKER_CONTEXT=heltec-ci', 'HELTEC_DOCKER_NETWORK=host')
}
$runnerArguments += @('bash', 'bin/heltec-ci.sh', $Mode)

& wsl.exe @runnerArguments
exit $LASTEXITCODE
