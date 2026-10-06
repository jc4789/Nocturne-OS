# Build Nocturne OS from PowerShell using the bundled MSYS2 toolchain.
#   .\build.ps1          -> build images into .\build
#   .\build.ps1 run      -> build and boot in QEMU
#   .\build.ps1 test     -> build and run the in-OS test suite (also test-quick, test-full)
param([string]$Target = "all")
$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"
& "$PSScriptRoot\tools\msys64\usr\bin\bash.exe" -lc "make -j8 $Target"
