param(
    [string]$ReferencePlugin = ".\out\le3e_plugin",
    [string]$CandidatePlugin = ".\out\le3e_plugin_probe3",
    [string]$MeshTag = "le3e"
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

function Tag-FromString {
    param([string]$Text)

    if ($Text.Length -ne 4) {
        throw "Tag must be exactly 4 characters: $Text"
    }

    return [uint32](
        (([uint32][byte][char]$Text[0] -shl 24) -bor
         ([uint32][byte][char]$Text[1] -shl 16) -bor
         ([uint32][byte][char]$Text[2] -shl 8) -bor
         [uint32][byte][char]$Text[3]) -band 0xFFFFFFFFL
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

function Get-MeshBytesFromPlugin {
    param(
        [string]$PluginPath,
        [string]$MeshTag
    )

    if (-not (Test-Path $PluginPath)) {
        throw "Missing plugin: $PluginPath"
    }

    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $PluginPath))
    if ($bytes.Length -lt 128) {
        throw "Plugin too small: $PluginPath"
    }

    $entryPointCount = Read-BE16 $bytes 100
    $tagCount = Read-BE16 $bytes 102
    $tagTableOffset = 128 + $entryPointCount * 112
    $wantedGroup = Tag-FromString "mesh"
    $wantedSubgroup = Tag-FromString $MeshTag

    for ($i = 0; $i -lt $tagCount; $i++) {
        $o = $tagTableOffset + $i * 64
        $group = Read-BE32 $bytes ($o + 36)
        $subgroup = Read-BE32 $bytes ($o + 40)
        if ($group -ne $wantedGroup -or $subgroup -ne $wantedSubgroup) {
            continue
        }

        $offset = [int](Read-BE32 $bytes ($o + 44))
        $size = [int](Read-BE32 $bytes ($o + 48))
        $nameBytes = [byte[]]::new(32)
        [Array]::Copy($bytes, $o + 4, $nameBytes, 0, 32)
        $zero = [Array]::IndexOf($nameBytes, [byte]0)
        if ($zero -lt 0) { $zero = 32 }
        $name = [System.Text.Encoding]::ASCII.GetString($nameBytes, 0, $zero)

        $mesh = [byte[]]::new($size)
        [Array]::Copy($bytes, $offset, $mesh, 0, $size)
        return [pscustomobject]@{
            Path = (Resolve-Path $PluginPath).Path
            Name = $name
            Offset = $offset
            Size = $size
            Bytes = $mesh
        }
    }

    throw "mesh/$MeshTag not found in $PluginPath"
}

function Compare-CellFlagsAndLinks {
    param(
        [byte[]]$A,
        [byte[]]$B
    )

    $cellW = 192
    $cellH = 192
    $gridStart = 1024
    $rows = @()
    $flagDiff = 0
    $firstDiff = 0

    for ($i = 0; $i -lt ($cellW * $cellH); $i++) {
        $o = $gridStart + $i * 12
        $fA = Read-BE16 $A ($o + 4)
        $fB = Read-BE16 $B ($o + 4)
        $lA = Read-BE16 $A ($o + 6)
        $lB = Read-BE16 $B ($o + 6)

        if ($fA -ne $fB) {
            $flagDiff++
            if ($rows.Count -lt 12) {
                $rows += [pscustomobject]@{
                    Kind = "flags"
                    Cell = $i
                    Row = [int]($i / $cellW)
                    Col = $i % $cellW
                    Ref = ('0x{0:X4}' -f $fA)
                    Cand = ('0x{0:X4}' -f $fB)
                }
            }
        }
        if ($lA -ne $lB) {
            $firstDiff++
            if ($rows.Count -lt 12) {
                $rows += [pscustomobject]@{
                    Kind = "first_object_index"
                    Cell = $i
                    Row = [int]($i / $cellW)
                    Col = $i % $cellW
                    Ref = ('0x{0:X4}' -f $lA)
                    Cand = ('0x{0:X4}' -f $lB)
                }
            }
        }
    }

    Write-Host ""
    Write-Host "Cell suspects"
    Write-Host ("  flags diffs: {0}" -f $flagDiff)
    Write-Host ("  first_object_index diffs: {0}" -f $firstDiff)
    if ($rows.Count -gt 0) {
        $rows | Format-Table -AutoSize
    } else {
        Write-Host "  No cell flag/link diffs."
    }
}

function Compare-UnitTypes {
    param(
        [byte[]]$A,
        [byte[]]$B
    )

    $count = [Math]::Min((Read-BE32 $A 36), (Read-BE32 $B 36))
    $offA = 1024 + (Read-BE32 $A 40)
    $offB = 1024 + (Read-BE32 $B 40)
    $rows = @()

    for ($i = 0; $i -lt $count; $i++) {
        $aBase = $offA + $i * 32
        $bBase = $offB + $i * 32
        $aType = Read-BE16 $A ($aBase + 0)
        $bType = Read-BE16 $B ($bBase + 0)
        $interesting = ($aType -in 1, 6) -or ($bType -in 1, 6)
        if (-not $interesting) { continue }

        $aFlags = Read-BE16 $A ($aBase + 2)
        $bFlags = Read-BE16 $B ($bBase + 2)
        $aTag = Read-BE32 $A ($aBase + 4)
        $bTag = Read-BE32 $B ($bBase + 4)
        $aTeam = Read-BE16 $A ($aBase + 8)
        $bTeam = Read-BE16 $B ($bBase + 8)
        $aNet = Read-BE32 $A ($aBase + 12)
        $bNet = Read-BE32 $B ($bBase + 12)
        $aCount = Read-BE16 $A ($aBase + 28)
        $bCount = Read-BE16 $B ($bBase + 28)
        $aRel = Read-BE16 $A ($aBase + 30)
        $bRel = Read-BE16 $B ($bBase + 30)

        if ($aType -ne $bType -or $aFlags -ne $bFlags -or $aTag -ne $bTag -or
            $aTeam -ne $bTeam -or $aNet -ne $bNet -or $aCount -ne $bCount -or $aRel -ne $bRel) {
            $rows += [pscustomobject]@{
                Index = $i
                RefType = $aType
                CandType = $bType
                RefTag = Tag-ToString $aTag
                CandTag = Tag-ToString $bTag
                RefFlags = ('0x{0:X4}' -f $aFlags)
                CandFlags = ('0x{0:X4}' -f $bFlags)
                RefTeam = $aTeam
                CandTeam = $bTeam
                RefNet = ('0x{0:X8}' -f $aNet)
                CandNet = ('0x{0:X8}' -f $bNet)
                RefCount = $aCount
                CandCount = $bCount
                RefRel = $aRel
                CandRel = $bRel
            }
        }
    }

    Write-Host ""
    Write-Host "Type-1 / Type-6 palette entries"
    if ($rows.Count -gt 0) {
        $rows | Format-Table -AutoSize
    } else {
        Write-Host "  No type-1/type-6 diffs."
    }
}

function Compare-InstanceLinks {
    param(
        [byte[]]$A,
        [byte[]]$B
    )

    $count = [Math]::Min((Read-BE32 $A 52), (Read-BE32 $B 52))
    $offA = 1024 + (Read-BE32 $A 56)
    $offB = 1024 + (Read-BE32 $B 56)
    $linkDiff = 0
    $rows = @()

    for ($i = 0; $i -lt $count; $i++) {
        $aBase = $offA + $i * 64
        $bBase = $offB + $i * 64
        $aType = Read-BE16 $A ($aBase + 4)
        $bType = Read-BE16 $B ($bBase + 4)
        $aPal = Read-BE16 $A ($aBase + 6)
        $bPal = Read-BE16 $B ($bBase + 6)
        $aIdent = Read-BE16 $A ($aBase + 8)
        $bIdent = Read-BE16 $B ($bBase + 8)
        $aPitch = Read-BE16 $A ($aBase + 34)
        $bPitch = Read-BE16 $B ($bBase + 34)
        $aPerm = [int]$A[$aBase + 37]
        $bPerm = [int]$B[$bBase + 37]
        $aLink = Read-BE16 $A ($aBase + 60)
        $bLink = Read-BE16 $B ($bBase + 60)

        if ($aLink -ne $bLink) {
            $linkDiff++
        }

        if (($aType -in 1, 6, 11) -or ($bType -in 1, 6, 11)) {
            if ($aType -ne $bType -or $aPal -ne $bPal -or $aIdent -ne $bIdent -or
                $aPitch -ne $bPitch -or $aPerm -ne $bPerm -or $aLink -ne $bLink) {
                if ($rows.Count -lt 24) {
                    $rows += [pscustomobject]@{
                        Index = $i
                        RefType = $aType
                        CandType = $bType
                        RefPal = $aPal
                        CandPal = $bPal
                        RefIdent = $aIdent
                        CandIdent = $bIdent
                        RefPitch = ('0x{0:X4}' -f $aPitch)
                        CandPitch = ('0x{0:X4}' -f $bPitch)
                        RefPerm = $aPerm
                        CandPerm = $bPerm
                        RefLink = ('0x{0:X4}' -f $aLink)
                        CandLink = ('0x{0:X4}' -f $bLink)
                    }
                }
            }
        }
    }

    Write-Host ""
    Write-Host "Instance +60 link field"
    Write-Host ("  total link diffs: {0}" -f $linkDiff)
    if ($rows.Count -gt 0) {
        $rows | Format-Table -AutoSize
    } else {
        Write-Host "  No interesting instance diffs."
    }
}

$ref = Get-MeshBytesFromPlugin -PluginPath $ReferencePlugin -MeshTag $MeshTag
$cand = Get-MeshBytesFromPlugin -PluginPath $CandidatePlugin -MeshTag $MeshTag

Write-Host ""
Write-Host ("Reference mesh: {0} ({1} bytes)" -f $ref.Path, $ref.Size)
Write-Host ("Candidate mesh: {0} ({1} bytes)" -f $cand.Path, $cand.Size)

Compare-CellFlagsAndLinks -A $ref.Bytes -B $cand.Bytes
Compare-UnitTypes -A $ref.Bytes -B $cand.Bytes
Compare-InstanceLinks -A $ref.Bytes -B $cand.Bytes
