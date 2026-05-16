param(
    [string]$MeshPath = ".\out\le3e_unedited\raw\mesh_tag.bin"
)

function Read-BE16([byte[]]$b, [int]$o) {
    return (([int]$b[$o] -shl 8) -bor [int]$b[$o + 1])
}

function Read-BE32([byte[]]$b, [int]$o) {
    return (([int]$b[$o] -shl 24) -bor
            ([int]$b[$o + 1] -shl 16) -bor
            ([int]$b[$o + 2] -shl 8) -bor
            [int]$b[$o + 3])
}

function Tag4([byte[]]$b, [int]$o) {
    return -join ([char[]]$b[$o..($o + 3)])
}

if (-not (Test-Path $MeshPath)) {
    throw "Mesh not found: $MeshPath"
}

$resolved = Resolve-Path $MeshPath
$bytes = [System.IO.File]::ReadAllBytes($resolved)

$connectorTag = Tag4 $bytes 72
$connectorCount = Read-BE32 $bytes 0x11C
$connectorsOffset = Read-BE32 $bytes 0x120
$connectorsSize = Read-BE32 $bytes 0x124

"Mesh: $resolved"
"connector_tag: $connectorTag"
"connector_count: $connectorCount"
"connectors_offset: $connectorsOffset"
"connectors_size: $connectorsSize"

if ($connectorCount -le 0 -or $connectorsSize -le 0) {
    return
}

$dataStart = 1024 + $connectorsOffset
$recordSize = if ($connectorCount -gt 0 -and ($connectorsSize % $connectorCount) -eq 0) {
    [int]($connectorsSize / $connectorCount)
} else {
    0
}

"record_size_guess: $recordSize"
""

for ($i = 0; $i -lt $connectorCount; $i++) {
    $off = $dataStart + ($i * $recordSize)
    if ($off + $recordSize -gt $bytes.Length) { break }

    "Connector $i @ 0x{0:X}" -f $off

    $hex = @()
    for ($j = 0; $j -lt $recordSize; $j++) {
        $hex += $bytes[$off + $j].ToString("X2")
    }
    "  raw: " + ($hex -join " ")

    if (($recordSize % 2) -eq 0) {
        $words = @()
        for ($j = 0; $j -lt $recordSize; $j += 2) {
            $words += ("{0:X4}" -f (Read-BE16 $bytes ($off + $j)))
        }
        "  be16: " + ($words -join " ")
    }

    ""
}
