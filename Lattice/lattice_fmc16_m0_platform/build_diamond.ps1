$ErrorActionPreference = 'Stop'

$diamond = 'E:\Digital-EDA\Lattice Diamond\bin\nt64\pnmainc.exe'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path -LiteralPath $diamond)) {
    throw "Diamond command-line tool not found: $diamond"
}

Push-Location $root
try {
    & $diamond '.\build_diamond.tcl' 2>&1 | Tee-Object -FilePath '.\diamond_build.log'
    if ($LASTEXITCODE -ne 0) { throw "Diamond failed with exit code $LASTEXITCODE" }

    $jed = Get-ChildItem -LiteralPath '.\impl_fmc16_link_test' -Filter *.jed -File |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $jed) { throw 'Diamond completed but no JED was generated' }
    Write-Output ("JED=" + $jed.FullName)
}
finally {
    Pop-Location
}
