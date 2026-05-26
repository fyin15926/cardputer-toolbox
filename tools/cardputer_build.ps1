param(
  [switch]$Flash,
  [string]$Port = "COM3",
  [switch]$SkipCompile
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Source = Join-Path $RepoRoot "toolbox\toolbox.ino"
$Cli = Join-Path $RepoRoot "tools\arduino-cli\arduino-cli.exe"
$Sketch = "C:\cardputer\sketches\toolbox"
$SketchFile = Join-Path $Sketch "toolbox.ino"
$Build = "C:\cardputer\build_fresh\out"
$Fqbn = "esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB"

if (!(Test-Path $Cli)) {
  throw "arduino-cli not found: $Cli"
}
if (!(Test-Path $Source)) {
  throw "source file not found: $Source"
}

New-Item -ItemType Directory -Force $Sketch | Out-Null
New-Item -ItemType Directory -Force $Build | Out-Null

Copy-Item $Source $SketchFile -Force
Write-Host "Synced source to $SketchFile"

if (!$SkipCompile) {
  Write-Host "Compiling with fixed Cardputer-Adv FQBN..."
  & $Cli compile --jobs 1 --fqbn $Fqbn --build-path $Build $Sketch
  if ($LASTEXITCODE -ne 0) {
    throw "compile failed with exit code $LASTEXITCODE"
  }
}

if ($Flash) {
  Write-Host "Uploading to $Port..."
  & $Cli upload --fqbn $Fqbn --port $Port --input-dir $Build
  if ($LASTEXITCODE -ne 0) {
    throw "upload failed with exit code $LASTEXITCODE"
  }
}

Write-Host "Done."
