param(
    [string]$Reference = ".\out\le3e_plugin",
    [string]$Candidate = ".\out\le3e_plugin_built",
    [switch]$ListOnly
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

function Read-CString {
    param(
        [byte[]]$Bytes,
        [int]$Offset,
        [int]$Length
    )

    $slice = [byte[]]::new($Length)
    [Array]::Copy($Bytes, $Offset, $slice, 0, $Length)
    $zero = [Array]::IndexOf($slice, [byte]0)
    if ($zero -ge 0) {
        $Length = $zero
    }

    return [System.Text.Encoding]::ASCII.GetString($slice, 0, $Length)
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

function Get-HashHex {
    param([byte[]]$Bytes)

    if ($null -eq $Bytes) {
        return ""
    }

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash($Bytes)
        return ([BitConverter]::ToString($hash)).Replace("-", "").ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Read-PluginHeader {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Missing file: $Path"
    }

    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Path))
    if ($bytes.Length -lt 128) {
        throw "File too small to contain Myth header: $Path"
    }

    $signature = Read-BE32 $bytes 124
    $isLocal = $false
    $entryPointCount = Read-BE16 $bytes 100
    $tagCount = Read-BE16 $bytes 102

    if ($signature -eq 0x646E6732) {
        $isLocal = $false
    } elseif ((Read-BE32 $bytes 60) -eq 0x6D746832) {
        $isLocal = $true
        $signature = 0x6D746832
        $entryPointCount = 0
        $tagCount = 1
    } else {
        throw "Unrecognized Myth tag container: $Path"
    }

    return [pscustomobject]@{
        Path = (Resolve-Path $Path).Path
        Bytes = $bytes
        Type = Read-BE16 $bytes 0
        Version = Read-BE16 $bytes 2
        Name = Read-CString $bytes 4 32
        Url = Read-CString $bytes 36 64
        EntryPointCount = $entryPointCount
        TagCount = $tagCount
        Flags = Read-BE32 $bytes 108
        Signature = $signature
        SignatureText = (Tag-ToString $signature)
        IsLocal = $isLocal
        Length = $bytes.Length
    }
}

function Read-PluginTags {
    param([string]$Path)

    $header = Read-PluginHeader $Path
    $bytes = $header.Bytes
    $start = if ($header.IsLocal) { 0 } else { 128 + $header.EntryPointCount * 112 }
    $rows = @()

    for ($i = 0; $i -lt $header.TagCount; $i++) {
        $o = $start + $i * 64
        if ($o + 64 -gt $bytes.Length) {
            throw "Tag table for $Path runs past end of file at entry $i."
        }

        $groupRaw = Read-BE32 $bytes ($o + 36)
        $subgroupRaw = Read-BE32 $bytes ($o + 40)
        $offset = Read-BE32 $bytes ($o + 44)
        $size = Read-BE32 $bytes ($o + 48)
        if ($header.IsLocal) {
            $size = [uint32][Math]::Max(0, $bytes.Length - 64)
        }
        $hash = ""
        if (($offset + $size) -le $bytes.Length) {
            $payload = [byte[]]::new([int]$size)
            if ($size -gt 0) {
                [Array]::Copy($bytes, [int]$offset, $payload, 0, [int]$size)
            }
            $hash = Get-HashHex $payload
        }

        $rows += [pscustomobject]@{
            Index = $i
            Name = Read-CString $bytes ($o + 4) 32
            Group = Tag-ToString $groupRaw
            GroupRaw = ('0x{0:X8}' -f $groupRaw)
            Subgroup = Tag-ToString $subgroupRaw
            SubgroupRaw = ('0x{0:X8}' -f $subgroupRaw)
            Offset = [uint32]$offset
            Size = [uint32]$size
            Version = Read-BE16 $bytes ($o + 56)
            Hash = $hash
            Key = ('{0}:{1}' -f ('0x{0:X8}' -f $groupRaw), ('0x{0:X8}' -f $subgroupRaw))
        }
    }

    return [pscustomobject]@{
        Header = $header
        Tags = $rows
    }
}

function Show-PluginSummary {
    param(
        [string]$Label,
        [pscustomobject]$Plugin
    )

    Write-Host ""
    Write-Host ("== {0} ==" -f $Label)
    Write-Host ("Path:       {0}" -f $Plugin.Header.Path)
    Write-Host ("Name:       {0}" -f $Plugin.Header.Name)
    Write-Host ("Type:       0x{0:X4}" -f $Plugin.Header.Type)
    Write-Host ("Version:    0x{0:X4}" -f $Plugin.Header.Version)
    Write-Host ("Signature:  {0} ({1})" -f $Plugin.Header.SignatureText, ('0x{0:X8}' -f $Plugin.Header.Signature))
    Write-Host ("Flags:      0x{0:X8}" -f $Plugin.Header.Flags)
    Write-Host ("Entries:    {0}" -f $Plugin.Header.EntryPointCount)
    Write-Host ("Tags:       {0}" -f $Plugin.Header.TagCount)
    Write-Host ("Length:     {0}" -f $Plugin.Header.Length)
}

function Show-Duplicates {
    param(
        [string]$Label,
        [object[]]$Tags
    )

    $dupes = $Tags |
        Group-Object Key |
        Where-Object { $_.Count -gt 1 } |
        Sort-Object Name

    Write-Host ""
    Write-Host ("Duplicate tag ids in {0}:" -f $Label)
    if ($dupes.Count -eq 0) {
        Write-Host "  none"
        return
    }

    foreach ($group in $dupes) {
        $items = $group.Group
        $first = $items[0]
        Write-Host ("  {0}/{1} count={2}" -f $first.Group, $first.Subgroup, $group.Count)
        $items |
            Select-Object Index, Name, Group, Subgroup, Version, Offset, Size |
            Format-Table -AutoSize
    }
}

function Show-TagList {
    param(
        [string]$Label,
        [object[]]$Tags
    )

    Write-Host ""
    Write-Host ("Tag list for {0}:" -f $Label)
    $Tags |
        Sort-Object Group, Subgroup, Index |
        Select-Object Index, Name, Group, Subgroup, Version, Offset, Size |
        Format-Table -AutoSize
}

function Group-TagsByKey {
    param([object[]]$Tags)

    $map = @{}
    foreach ($tag in $Tags) {
        if (-not $map.ContainsKey($tag.Key)) {
            $map[$tag.Key] = New-Object System.Collections.ArrayList
        }
        [void]$map[$tag.Key].Add($tag)
    }
    return $map
}

function Show-Comparison {
    param(
        [pscustomobject]$ReferencePlugin,
        [pscustomobject]$CandidatePlugin
    )

    $refGroups = Group-TagsByKey $ReferencePlugin.Tags
    $candGroups = Group-TagsByKey $CandidatePlugin.Tags

    $allKeys = @($refGroups.Keys + $candGroups.Keys | Sort-Object -Unique)
    $onlyRef = @()
    $onlyCand = @()
    $changed = @()

    foreach ($key in $allKeys) {
        $a = @($refGroups[$key])
        $b = @($candGroups[$key])

        if ($a.Count -eq 0) {
            foreach ($item in $b) {
                $onlyCand += [pscustomobject]@{
                    Group = $item.Group
                    Subgroup = $item.Subgroup
                    Name = $item.Name
                    Version = $item.Version
                    Offset = $item.Offset
                    Size = $item.Size
                }
            }
            continue
        }

        if ($b.Count -eq 0) {
            foreach ($item in $a) {
                $onlyRef += [pscustomobject]@{
                    Group = $item.Group
                    Subgroup = $item.Subgroup
                    Name = $item.Name
                    Version = $item.Version
                    Offset = $item.Offset
                    Size = $item.Size
                }
            }
            continue
        }

        if ($a.Count -ne $b.Count) {
            $changed += [pscustomobject]@{
                Group = $a[0].Group
                Subgroup = $a[0].Subgroup
                Issue = "duplicate-count"
                Reference = $a.Count
                Candidate = $b.Count
            }
            continue
        }

        $aSorted = $a | Sort-Object Name, Version, Size, Offset
        $bSorted = $b | Sort-Object Name, Version, Size, Offset
        for ($i = 0; $i -lt $aSorted.Count; $i++) {
            $left = $aSorted[$i]
            $right = $bSorted[$i]
            if ($left.Name -ne $right.Name -or
                $left.Version -ne $right.Version -or
                $left.Size -ne $right.Size -or
                $left.Offset -ne $right.Offset) {
                $changed += [pscustomobject]@{
                    Group = $left.Group
                    Subgroup = $left.Subgroup
                    Issue = "metadata"
                    Reference = ("{0} v{1} off={2} size={3}" -f $left.Name, $left.Version, $left.Offset, $left.Size)
                    Candidate = ("{0} v{1} off={2} size={3}" -f $right.Name, $right.Version, $right.Offset, $right.Size)
                }
            }
            if ($left.Hash -ne $right.Hash) {
                $changed += [pscustomobject]@{
                    Group = $left.Group
                    Subgroup = $left.Subgroup
                    Issue = "payload-hash"
                    Reference = $left.Hash
                    Candidate = $right.Hash
                }
            }
        }
    }

    Write-Host ""
    Write-Host "Tags only in reference:"
    if ($onlyRef.Count -eq 0) {
        Write-Host "  none"
    } else {
        $onlyRef |
            Sort-Object Group, Subgroup, Name |
            Format-Table -AutoSize
    }

    Write-Host ""
    Write-Host "Tags only in candidate:"
    if ($onlyCand.Count -eq 0) {
        Write-Host "  none"
    } else {
        $onlyCand |
            Sort-Object Group, Subgroup, Name |
            Format-Table -AutoSize
    }

    Write-Host ""
    Write-Host "Shared tag ids with metadata differences:"
    if ($changed.Count -eq 0) {
        Write-Host "  none"
    } else {
        $changed |
            Sort-Object Group, Subgroup, Issue |
            Format-Table -AutoSize
    }
}

$refPlugin = Read-PluginTags $Reference
Show-PluginSummary -Label "Reference" -Plugin $refPlugin
Show-Duplicates -Label "reference" -Tags $refPlugin.Tags

if ($ListOnly -or [string]::IsNullOrWhiteSpace($Candidate)) {
    Show-TagList -Label "reference" -Tags $refPlugin.Tags
    return
}

$candPlugin = Read-PluginTags $Candidate
Show-PluginSummary -Label "Candidate" -Plugin $candPlugin
Show-Duplicates -Label "candidate" -Tags $candPlugin.Tags
Show-Comparison -ReferencePlugin $refPlugin -CandidatePlugin $candPlugin
