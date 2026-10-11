# Copies dist\KenshiKin into <Kenshi>\mods\KenshiKin for the Game Editor's Workshop upload, keeping
# the files the editor owns there: KenshiKin.mod (it stores the item's info in it), _KenshiKin.info
# (Workshop item id, title, tags, visibility) and _KenshiKin.img (preview). Removes package files that
# no longer exist in dist. Refuses while the game is running.
param([string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi")
$ErrorActionPreference = "Stop"
$dist = (Resolve-Path "$PSScriptRoot\..\dist\KenshiKin").Path
$dst = Join-Path $Game "mods\KenshiKin"
if (Get-Process Kenshi_x64 -ErrorAction SilentlyContinue) { Write-Host "Kenshi is running: close it first."; exit 1 }
$keep = 'KenshiKin.mod', '_KenshiKin.info', '_KenshiKin.img'
New-Item -ItemType Directory -Force $dst | Out-Null
# stale package files (not in dist, not owned by the editor)
Get-ChildItem $dst -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($dst.Length + 1)
    if ($keep -notcontains $rel -and -not (Test-Path (Join-Path $dist $rel))) { Remove-Item $_.FullName -Force; Write-Host "removed $rel" }
}
Get-ChildItem $dist -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($dist.Length + 1)
    if ($keep -contains $rel -and (Test-Path (Join-Path $dst $rel))) { return }   # the editor's copy stays
    $t = Join-Path $dst $rel
    New-Item -ItemType Directory -Force (Split-Path $t) | Out-Null
    Copy-Item $_.FullName $t -Force
}
Write-Host "installed in $dst (kept: $((($keep | Where-Object { Test-Path (Join-Path $dst $_) }) -join ', ')))"
