# Build Nocturne OS from PowerShell using the bundled MSYS2 toolchain.
#   .\build.ps1          -> build images into .\build
#   .\build.ps1 run      -> build and boot in QEMU
#   .\build.ps1 test     -> build and run the in-OS test suite (also test-quick, test-full)
param([string]$Target = "all")
$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"
# Keep paths and the requested target out of shell syntax. The caller's working
# directory must not decide which project gets built.
$env:NOCTURNE_BUILD_ROOT = $PSScriptRoot
$env:NOCTURNE_BUILD_TARGET = $Target
& "$PSScriptRoot\tools\msys64\usr\bin\bash.exe" -lc 'cd "$NOCTURNE_BUILD_ROOT" && make -j8 "$NOCTURNE_BUILD_TARGET"'
exit $LASTEXITCODE
