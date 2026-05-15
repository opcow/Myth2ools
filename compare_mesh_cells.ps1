function Read-BE16 {
    param(
        [byte[]]$Bytes,
        [int]$Offset
    )

    return (([int]$Bytes[$Offset] -shl 8) -bor [int]$Bytes[$Offset + 1])
}

$paths = @(
    @{ Name = "stock"; Path = ".\out\le3e_unedited\raw\mesh_tag.bin" },
    @{ Name = "built"; Path = ".\out\le3e_built\raw\mesh_tag.bin" },
    @{ Name = "roundtrip"; Path = ".\out\le3e_rt\raw\mesh_tag.bin" }
)

$offsets = @(0x9B70, 0x9B7C, 0x9B88, 0x9B94)

foreach ($entry in $paths) {
    if (-not (Test-Path $entry.Path)) {
        Write-Host "Missing: $($entry.Path)"
        continue
    }

    $resolved = Resolve-Path $entry.Path
    $bytes = [System.IO.File]::ReadAllBytes($resolved)

    Write-Host ""
    Write-Host "== $($entry.Name) =="

    foreach ($off in $offsets) {
        $height = Read-BE16 -Bytes $bytes -Offset $off
        $normal = Read-BE16 -Bytes $bytes -Offset ($off + 2)
        $flags = Read-BE16 -Bytes $bytes -Offset ($off + 4)
        $firstObject = Read-BE16 -Bytes $bytes -Offset ($off + 6)
        $mediaHeight = Read-BE16 -Bytes $bytes -Offset ($off + 8)
        $renderHeight = Read-BE16 -Bytes $bytes -Offset ($off + 10)

        "{0}: h={1:X4} n={2:X4} f={3:X4} first={4:X4} media={5:X4} render={6:X4}" -f `
            ("0x{0:X}" -f $off), $height, $normal, $flags, $firstObject, $mediaHeight, $renderHeight
    }
}
