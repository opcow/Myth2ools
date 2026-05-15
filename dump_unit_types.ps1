param(
    [string]$Reference = ".\out\le3e_unedited\raw\mesh_tag.bin",
    [string]$Candidate = ".\out\le3e_built\raw\mesh_tag.bin"
)

function Read-BE16 {
    param(
        [byte[]]$Bytes,
        [int]$Offset
    )

    return (([int]$Bytes[$Offset] -shl 8) -bor [int]$Bytes[$Offset + 1])
}

function Read-BE32 {
    param(
        [byte[]]$Bytes,
        [int]$Offset
    )

    return [uint32](
        (([uint32]$Bytes[$Offset] -shl 24) -bor
         ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
         ([uint32]$Bytes[$Offset + 2] -shl 8) -bor
         [uint32]$Bytes[$Offset + 3]) -band 0xFFFFFFFFL
    )
}

function Tag-ToString {
    param([uint32]$Tag)

    if ($Tag -eq 0xFFFFFFFF) { return "-1" }
    if ($Tag -eq 0) { return "0" }
    $chars = @(
        [char](($Tag -shr 24) -band 0xFF),
        [char](($Tag -shr 16) -band 0xFF),
        [char](($Tag -shr 8) -band 0xFF),
        [char]($Tag -band 0xFF)
    )
    return -join $chars
}

function Read-UnitTypes {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Missing mesh file: $Path"
    }

    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Path))
    $count = Read-BE32 $bytes 36
    $offset = 1024 + (Read-BE32 $bytes 40)
    $items = @()

    for ($i = 0; $i -lt $count; $i++) {
        $o = $offset + $i * 32
        $type = Read-BE16 $bytes ($o + 0)
        $flags = Read-BE16 $bytes ($o + 2)
        $tagRaw = Read-BE32 $bytes ($o + 4)
        $team = Read-BE16 $bytes ($o + 8)
        $netgame = [uint32](Read-BE32 $bytes ($o + 12))
        $instanceCount = Read-BE16 $bytes ($o + 28)
        $typeIndex = Read-BE16 $bytes ($o + 30)

        $items += [pscustomobject]@{
            Index = $i
            Type = $type
            Flags = ('0x{0:X4}' -f $flags)
            Tag = (Tag-ToString $tagRaw)
            TagRaw = ('0x{0:X8}' -f $tagRaw)
            Team = $team
            Netgame = ('0x{0:X8}' -f $netgame)
            InstanceCount = $instanceCount
            TypeIndex = $typeIndex
        }
    }

    return $items
}

$ref = Read-UnitTypes $Reference
$cand = Read-UnitTypes $Candidate
$max = [Math]::Max($ref.Count, $cand.Count)

Write-Host ""
Write-Host ("Reference: {0}" -f (Resolve-Path $Reference).Path)
Write-Host ("Candidate: {0}" -f (Resolve-Path $Candidate).Path)
Write-Host ""

$rows = @()
for ($i = 0; $i -lt $max; $i++) {
    $a = if ($i -lt $ref.Count) { $ref[$i] } else { $null }
    $b = if ($i -lt $cand.Count) { $cand[$i] } else { $null }

    $different = $false
    if ($null -eq $a -or $null -eq $b) {
        $different = $true
    } else {
        $different = (
            $a.Type -ne $b.Type -or
            $a.Flags -ne $b.Flags -or
            $a.TagRaw -ne $b.TagRaw -or
            $a.Team -ne $b.Team -or
            $a.Netgame -ne $b.Netgame -or
            $a.InstanceCount -ne $b.InstanceCount -or
            $a.TypeIndex -ne $b.TypeIndex
        )
    }

    if ($different) {
        $rows += [pscustomobject]@{
            Index = $i
            RefType = if ($a) { $a.Type } else { "-" }
            CandType = if ($b) { $b.Type } else { "-" }
            RefTag = if ($a) { $a.Tag } else { "-" }
            CandTag = if ($b) { $b.Tag } else { "-" }
            RefFlags = if ($a) { $a.Flags } else { "-" }
            CandFlags = if ($b) { $b.Flags } else { "-" }
            RefTeam = if ($a) { $a.Team } else { "-" }
            CandTeam = if ($b) { $b.Team } else { "-" }
            RefCount = if ($a) { $a.InstanceCount } else { "-" }
            CandCount = if ($b) { $b.InstanceCount } else { "-" }
            RefTypeIndex = if ($a) { $a.TypeIndex } else { "-" }
            CandTypeIndex = if ($b) { $b.TypeIndex } else { "-" }
        }
    }
}

if ($rows.Count -eq 0) {
    Write-Host "No unit type differences."
} else {
    $rows | Format-Table -AutoSize
}
