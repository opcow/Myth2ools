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

    return (([int]$Bytes[$Offset] -shl 24) -bor
            ([int]$Bytes[$Offset + 1] -shl 16) -bor
            ([int]$Bytes[$Offset + 2] -shl 8) -bor
            [int]$Bytes[$Offset + 3])
}

function Compare-CellGrid {
    param(
        [byte[]]$A,
        [byte[]]$B,
        [string]$LabelA,
        [string]$LabelB,
        [int]$CellW = 192,
        [int]$CellH = 192
    )

    $gridStart = 1024
    $cellSize = 12
    $cellCount = $CellW * $CellH

    $diffHeight = 0
    $diffNormal = 0
    $diffFlags = 0
    $diffFirst = 0
    $diffMedia = 0
    $diffRender = 0
    $examples = @()

    for ($i = 0; $i -lt $cellCount; $i++) {
        $off = $gridStart + $i * $cellSize
        $row = [int]($i / $CellW)
        $col = $i % $CellW

        $hA = Read-BE16 $A $off
        $hB = Read-BE16 $B $off
        $nA = Read-BE16 $A ($off + 2)
        $nB = Read-BE16 $B ($off + 2)
        $fA = Read-BE16 $A ($off + 4)
        $fB = Read-BE16 $B ($off + 4)
        $oA = Read-BE16 $A ($off + 6)
        $oB = Read-BE16 $B ($off + 6)
        $mA = Read-BE16 $A ($off + 8)
        $mB = Read-BE16 $B ($off + 8)
        $rA = Read-BE16 $A ($off + 10)
        $rB = Read-BE16 $B ($off + 10)

        $changed = $false
        if ($hA -ne $hB) { $diffHeight++; $changed = $true }
        if ($nA -ne $nB) { $diffNormal++; $changed = $true }
        if ($fA -ne $fB) { $diffFlags++; $changed = $true }
        if ($oA -ne $oB) { $diffFirst++; $changed = $true }
        if ($mA -ne $mB) { $diffMedia++; $changed = $true }
        if ($rA -ne $rB) { $diffRender++; $changed = $true }

        if ($changed -and $examples.Count -lt 12) {
            $examples += [pscustomobject]@{
                Offset = ('0x{0:X}' -f $off)
                Row = $row
                Col = $col
                Height = ('{0:X4}/{1:X4}' -f $hA, $hB)
                Normal = ('{0:X4}/{1:X4}' -f $nA, $nB)
                Flags = ('{0:X4}/{1:X4}' -f $fA, $fB)
                First = ('{0:X4}/{1:X4}' -f $oA, $oB)
                Media = ('{0:X4}/{1:X4}' -f $mA, $mB)
                Render = ('{0:X4}/{1:X4}' -f $rA, $rB)
            }
        }
    }

    Write-Host ""
    Write-Host "Cell grid: $LabelA vs $LabelB"
    Write-Host ("  height={0} normal={1} flags={2} first={3} media={4} render={5}" -f `
        $diffHeight, $diffNormal, $diffFlags, $diffFirst, $diffMedia, $diffRender)
    if ($examples.Count -gt 0) {
        $examples | Format-Table -AutoSize
    } else {
        Write-Host "  No cell-grid diffs."
    }
}

function Compare-UnitTypes {
    param(
        [byte[]]$A,
        [byte[]]$B,
        [string]$LabelA,
        [string]$LabelB
    )

    $offA = 1024 + (Read-BE32 $A 40)
    $offB = 1024 + (Read-BE32 $B 40)
    $countA = Read-BE32 $A 36
    $countB = Read-BE32 $B 36
    $count = [Math]::Min($countA, $countB)
    $diffs = @()

    for ($i = 0; $i -lt $count; $i++) {
        $aType = Read-BE16 $A ($offA + $i * 32 + 0)
        $bType = Read-BE16 $B ($offB + $i * 32 + 0)
        $aTag = Read-BE32 $A ($offA + $i * 32 + 4)
        $bTag = Read-BE32 $B ($offB + $i * 32 + 4)
        $aCount = Read-BE16 $A ($offA + $i * 32 + 28)
        $bCount = Read-BE16 $B ($offB + $i * 32 + 28)
        $aRel = Read-BE16 $A ($offA + $i * 32 + 30)
        $bRel = Read-BE16 $B ($offB + $i * 32 + 30)

        if ($aType -ne $bType -or $aTag -ne $bTag -or $aCount -ne $bCount -or $aRel -ne $bRel) {
            if ($diffs.Count -lt 12) {
                $diffs += [pscustomobject]@{
                    Index = $i
                    Type = ('{0:X4}/{1:X4}' -f $aType, $bType)
                    Tag = ('0x{0:X8}/0x{1:X8}' -f $aTag, $bTag)
                    Count = ('{0}/{1}' -f $aCount, $bCount)
                    Rel = ('{0}/{1}' -f $aRel, $bRel)
                }
            }
        }
    }

    Write-Host ""
    Write-Host "Unit types: $LabelA vs $LabelB"
    Write-Host ("  countA={0} countB={1}" -f $countA, $countB)
    if ($diffs.Count -gt 0) {
        $diffs | Format-Table -AutoSize
    } else {
        Write-Host "  No unit-type diffs in shared range."
    }
}

function Compare-Instances {
    param(
        [byte[]]$A,
        [byte[]]$B,
        [string]$LabelA,
        [string]$LabelB
    )

    $offA = 1024 + (Read-BE32 $A 56)
    $offB = 1024 + (Read-BE32 $B 56)
    $countA = Read-BE32 $A 52
    $countB = Read-BE32 $B 52
    $count = [Math]::Min($countA, $countB)

    $diffType = 0
    $diffPal = 0
    $diffIdent = 0
    $diffPos = 0
    $diffYaw = 0
    $diffPitch = 0
    $diffPerm = 0
    $diffLink = 0
    $examples = @()

    for ($i = 0; $i -lt $count; $i++) {
        $aBase = $offA + $i * 64
        $bBase = $offB + $i * 64
        $aType = Read-BE16 $A ($aBase + 4)
        $bType = Read-BE16 $B ($bBase + 4)
        $aPal = Read-BE16 $A ($aBase + 6)
        $bPal = Read-BE16 $B ($bBase + 6)
        $aIdent = Read-BE16 $A ($aBase + 8)
        $bIdent = Read-BE16 $B ($bBase + 8)
        $aX = Read-BE32 $A ($aBase + 12)
        $bX = Read-BE32 $B ($bBase + 12)
        $aY = Read-BE32 $A ($aBase + 16)
        $bY = Read-BE32 $B ($bBase + 16)
        $aZ = Read-BE32 $A ($aBase + 20)
        $bZ = Read-BE32 $B ($bBase + 20)
        $aYaw = Read-BE16 $A ($aBase + 32)
        $bYaw = Read-BE16 $B ($bBase + 32)
        $aPitch = Read-BE16 $A ($aBase + 34)
        $bPitch = Read-BE16 $B ($bBase + 34)
        $aPerm = [int]$A[$aBase + 37]
        $bPerm = [int]$B[$bBase + 37]
        $aLink = Read-BE16 $A ($aBase + 60)
        $bLink = Read-BE16 $B ($bBase + 60)

        $changed = $false
        if ($aType -ne $bType) { $diffType++; $changed = $true }
        if ($aPal -ne $bPal) { $diffPal++; $changed = $true }
        if ($aIdent -ne $bIdent) { $diffIdent++; $changed = $true }
        if ($aX -ne $bX -or $aY -ne $bY -or $aZ -ne $bZ) { $diffPos++; $changed = $true }
        if ($aYaw -ne $bYaw) { $diffYaw++; $changed = $true }
        if ($aPitch -ne $bPitch) { $diffPitch++; $changed = $true }
        if ($aPerm -ne $bPerm) { $diffPerm++; $changed = $true }
        if ($aLink -ne $bLink) { $diffLink++; $changed = $true }

        if ($changed -and $examples.Count -lt 12) {
            $examples += [pscustomobject]@{
                Index = $i
                Type = ('{0}/{1}' -f $aType, $bType)
                Pal = ('{0}/{1}' -f $aPal, $bPal)
                Ident = ('{0}/{1}' -f $aIdent, $bIdent)
                Perm = ('{0}/{1}' -f $aPerm, $bPerm)
                Link = ('{0:X4}/{1:X4}' -f $aLink, $bLink)
            }
        }
    }

    Write-Host ""
    Write-Host "Instances: $LabelA vs $LabelB"
    Write-Host ("  type={0} pal={1} ident={2} pos={3} yaw={4} pitch={5} perm={6} link={7}" -f `
        $diffType, $diffPal, $diffIdent, $diffPos, $diffYaw, $diffPitch, $diffPerm, $diffLink)
    if ($examples.Count -gt 0) {
        $examples | Format-Table -AutoSize
    } else {
        Write-Host "  No instance diffs in shared range."
    }
}

$files = @(
    @{ Name = "stock"; Path = ".\out\le3e_unedited\raw\mesh_tag.bin" },
    @{ Name = "built"; Path = ".\out\le3e_built\raw\mesh_tag.bin" },
    @{ Name = "roundtrip"; Path = ".\out\le3e_rt\raw\mesh_tag.bin" }
)

$loaded = @{}
foreach ($entry in $files) {
    if (-not (Test-Path $entry.Path)) {
        Write-Host "Missing: $($entry.Path)"
        exit 1
    }
    $loaded[$entry.Name] = [System.IO.File]::ReadAllBytes((Resolve-Path $entry.Path))
}

Compare-CellGrid -A $loaded["stock"] -B $loaded["built"] -LabelA "stock" -LabelB "built"
Compare-CellGrid -A $loaded["stock"] -B $loaded["roundtrip"] -LabelA "stock" -LabelB "roundtrip"

Compare-UnitTypes -A $loaded["stock"] -B $loaded["built"] -LabelA "stock" -LabelB "built"
Compare-UnitTypes -A $loaded["stock"] -B $loaded["roundtrip"] -LabelA "stock" -LabelB "roundtrip"

Compare-Instances -A $loaded["stock"] -B $loaded["built"] -LabelA "stock" -LabelB "built"
Compare-Instances -A $loaded["stock"] -B $loaded["roundtrip"] -LabelA "stock" -LabelB "roundtrip"
