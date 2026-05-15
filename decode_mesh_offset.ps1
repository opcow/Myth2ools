param(
    [Parameter(Mandatory = $true)]
    [string]$MeshPath,

    [Parameter(Mandatory = $true)]
    [string]$Offset
)

function Read-BE16 {
    param(
        [byte[]]$Bytes,
        [int]$At
    )

    return (([int]$Bytes[$At] -shl 8) -bor [int]$Bytes[$At + 1])
}

function Read-BE32 {
    param(
        [byte[]]$Bytes,
        [int]$At
    )

    return (([int]$Bytes[$At] -shl 24) -bor
            ([int]$Bytes[$At + 1] -shl 16) -bor
            ([int]$Bytes[$At + 2] -shl 8) -bor
            [int]$Bytes[$At + 3])
}

function Parse-Offset {
    param([string]$Text)

    $t = $Text.Trim()
    if ($t.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        return [Convert]::ToInt64($t.Substring(2), 16)
    }
    return [Convert]::ToInt64($t, 10)
}

if (-not (Test-Path $MeshPath)) {
    Write-Error "Mesh file not found: $MeshPath"
    exit 1
}

$resolved = (Resolve-Path $MeshPath).Path
$bytes = [System.IO.File]::ReadAllBytes($resolved)
$fileOffset = Parse-Offset $Offset

if ($fileOffset -lt 0 -or $fileOffset -ge $bytes.Length) {
    Write-Error ("Offset 0x{0:X} is outside file length 0x{1:X}" -f $fileOffset, $bytes.Length)
    exit 1
}

$headerSize = 1024
$dataOffset = $fileOffset - $headerSize

$unitTypeCount = Read-BE32 $bytes 36
$unitTypeOffset = Read-BE32 $bytes 40
$unitTypeSize = Read-BE32 $bytes 44

$instanceCount = Read-BE32 $bytes 52
$instanceOffset = Read-BE32 $bytes 56
$instanceSize = Read-BE32 $bytes 60

$actionCount = Read-BE32 $bytes 128
$actionOffset = Read-BE32 $bytes 132
$actionSize = Read-BE32 $bytes 136

$cellDataSize = Read-BE32 $bytes 16
$cellDataOffset = Read-BE32 $bytes 12

$cellStart = $headerSize + $cellDataOffset
$cellEnd = $cellStart + $cellDataSize
$unitTypeStart = $headerSize + $unitTypeOffset
$unitTypeEnd = $unitTypeStart + $unitTypeSize
$instanceStart = $headerSize + $instanceOffset
$instanceEnd = $instanceStart + $instanceSize
$actionStart = $headerSize + $actionOffset
$actionEnd = $actionStart + $actionSize

Write-Host ("File: {0}" -f $resolved)
Write-Host ("Offset: 0x{0:X}" -f $fileOffset)
Write-Host ("Data-relative: 0x{0:X}" -f $dataOffset)
Write-Host ""

Write-Host ("Cell grid:   0x{0:X} .. 0x{1:X}" -f $cellStart, ($cellEnd - 1))
Write-Host ("Unit types:  0x{0:X} .. 0x{1:X}" -f $unitTypeStart, ($unitTypeEnd - 1))
Write-Host ("Instances:   0x{0:X} .. 0x{1:X}" -f $instanceStart, ($instanceEnd - 1))
Write-Host ("Actions:     0x{0:X} .. 0x{1:X}" -f $actionStart, ($actionEnd - 1))
Write-Host ""

if ($fileOffset -lt $headerSize) {
    Write-Host ("Region: header (byte 0x{0:X} of 0x400)" -f $fileOffset)
}
elseif ($fileOffset -ge $cellStart -and $fileOffset -lt $cellEnd) {
    $cellSize = 12
    $rel = $fileOffset - $cellStart
    $cellIndex = [math]::Floor($rel / $cellSize)
    $cellByte = $rel % $cellSize
    $cellW = [int](Read-BE16 $bytes 8) * 32
    $row = [math]::Floor($cellIndex / $cellW)
    $col = $cellIndex % $cellW
    Write-Host ("Region: cell grid")
    Write-Host ("Cell index: {0}" -f $cellIndex)
    Write-Host ("Cell byte:  0x{0:X}" -f $cellByte)
    Write-Host ("Cell row/col: {0}, {1}" -f $row, $col)
}
elseif ($fileOffset -ge $unitTypeStart -and $fileOffset -lt $unitTypeEnd) {
    $recordSize = 32
    $rel = $fileOffset - $unitTypeStart
    $index = [math]::Floor($rel / $recordSize)
    $recordByte = $rel % $recordSize
    Write-Host ("Region: unit type table")
    Write-Host ("Record index: {0} of {1}" -f $index, $unitTypeCount)
    Write-Host ("Record byte:  0x{0:X}" -f $recordByte)
}
elseif ($fileOffset -ge $instanceStart -and $fileOffset -lt $instanceEnd) {
    $recordSize = 64
    $rel = $fileOffset - $instanceStart
    $index = [math]::Floor($rel / $recordSize)
    $recordByte = $rel % $recordSize
    Write-Host ("Region: instance table")
    Write-Host ("Record index: {0} of {1}" -f $index, $instanceCount)
    Write-Host ("Record byte:  0x{0:X}" -f $recordByte)
}
elseif ($fileOffset -ge $actionStart -and $fileOffset -lt $actionEnd) {
    $rel = $fileOffset - $actionStart
    Write-Host ("Region: action buffer")
    Write-Host ("Action-relative byte: 0x{0:X}" -f $rel)
    Write-Host ("Action count: {0}" -f $actionCount)
}
else {
    Write-Host "Region: outside the primary decoded sections"
}
