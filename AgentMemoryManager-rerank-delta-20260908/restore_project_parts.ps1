param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$Destination
)

$ErrorActionPreference = 'Stop'
$packageRoot = (Resolve-Path -LiteralPath $PackageDirectory).Path
$destinationRoot = [System.IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null
$manifest = Get-Content -LiteralPath (Join-Path $packageRoot 'manifest.json') -Raw | ConvertFrom-Json

foreach ($package in $manifest.packages) {
    $packagePath = Join-Path $packageRoot $package.name
    $actual = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $package.sha256) { throw "Package checksum mismatch: $($package.name)" }
    Expand-Archive -LiteralPath $packagePath -DestinationPath $destinationRoot -Force
}

foreach ($file in $manifest.files | Where-Object { $_.chunks }) {
    $target = Join-Path $destinationRoot $file.path
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    $output = [System.IO.File]::Create($target)
    try {
        foreach ($chunk in $file.chunks) {
            $chunkPath = Join-Path $destinationRoot $chunk.entry
            $input = [System.IO.File]::OpenRead($chunkPath)
            try { $input.CopyTo($output) } finally { $input.Dispose() }
        }
    } finally { $output.Dispose() }
}

foreach ($file in $manifest.files) {
    $target = Join-Path $destinationRoot $file.path
    $actual = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $file.sha256) { throw "Restored file checksum mismatch: $($file.path)" }
}

$chunkRoot = Join-Path $destinationRoot '__chunks__'
if (Test-Path -LiteralPath $chunkRoot) { Remove-Item -LiteralPath $chunkRoot -Recurse -Force }
Write-Output "Restore verified: $destinationRoot"
