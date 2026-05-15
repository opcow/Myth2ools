param(
    [string]$Reference = ".\out\le3e_unedited",
    [string]$Candidate = ".\out\le3e_rt"
)

function Get-RelativePath {
    param(
        [string]$Base,
        [string]$Path
    )

    $baseUri = New-Object System.Uri((Resolve-Path $Base).Path.TrimEnd('\') + '\')
    $pathUri = New-Object System.Uri((Resolve-Path $Path).Path)
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($pathUri).ToString().Replace('/', '\'))
}

function Get-FileMap {
    param(
        [string]$Root
    )

    $map = @{}
    Get-ChildItem -Path $Root -Recurse -File | ForEach-Object {
        $rel = Get-RelativePath -Base $Root -Path $_.FullName
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
        $map[$rel] = [pscustomobject]@{
            FullName = $_.FullName
            Length = $_.Length
            Hash = $hash
        }
    }
    return $map
}

if (-not (Test-Path $Reference)) {
    Write-Error "Reference folder not found: $Reference"
    exit 1
}

if (-not (Test-Path $Candidate)) {
    Write-Error "Candidate folder not found: $Candidate"
    exit 1
}

$refMap = Get-FileMap -Root $Reference
$candMap = Get-FileMap -Root $Candidate

$allPaths = @($refMap.Keys + $candMap.Keys | Sort-Object -Unique)
$onlyRef = @()
$onlyCand = @()
$diffs = @()

foreach ($rel in $allPaths) {
    $inRef = $refMap.ContainsKey($rel)
    $inCand = $candMap.ContainsKey($rel)

    if ($inRef -and -not $inCand) {
        $onlyRef += $rel
        continue
    }

    if ($inCand -and -not $inRef) {
        $onlyCand += $rel
        continue
    }

    $a = $refMap[$rel]
    $b = $candMap[$rel]
    if ($a.Length -ne $b.Length -or $a.Hash -ne $b.Hash) {
        $diffs += [pscustomobject]@{
            Path = $rel
            ReferenceLength = $a.Length
            CandidateLength = $b.Length
            SameLength = ($a.Length -eq $b.Length)
        }
    }
}

Write-Host ""
Write-Host "Reference: $((Resolve-Path $Reference).Path)"
Write-Host "Candidate: $((Resolve-Path $Candidate).Path)"
Write-Host ""
Write-Host ("Only in reference: {0}" -f $onlyRef.Count)
Write-Host ("Only in candidate: {0}" -f $onlyCand.Count)
Write-Host ("Different files:   {0}" -f $diffs.Count)

if ($onlyRef.Count -gt 0) {
    Write-Host ""
    Write-Host "Only in reference:"
    $onlyRef | Select-Object -First 50 | ForEach-Object { "  $_" }
}

if ($onlyCand.Count -gt 0) {
    Write-Host ""
    Write-Host "Only in candidate:"
    $onlyCand | Select-Object -First 50 | ForEach-Object { "  $_" }
}

if ($diffs.Count -gt 0) {
    Write-Host ""
    Write-Host "Different files:"
    $diffs | Select-Object -First 100 | Format-Table -AutoSize
}

if ($onlyRef.Count -eq 0 -and $onlyCand.Count -eq 0 -and $diffs.Count -eq 0) {
    Write-Host ""
    Write-Host "Folders match."
}
