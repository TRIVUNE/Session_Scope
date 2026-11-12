# Takes proof.rpp (made by REAPER with the same ten plug-ins) and replaces each plug-in's saved
# state with the state Ableton Live saved (as the bench wrote it: live\bench\state\N.component /
# N.controller), keeping REAPER's own header (pins) and preset-name part. Writes proof-live.rpp.
param([string]$Rpp, [string]$StateDir, [string]$Out)

# REAPER's order of the plug-ins in proof.rpp -> their number in the Live set.
$liveIndex = @(3, 1, 4, 2, 5, 6, 7, 8, 9, 10)

function U32([int64]$v) { return [BitConverter]::GetBytes([uint32]$v) }
function Lines([byte[]]$bytes, [string]$indent) {
  $b64 = [Convert]::ToBase64String($bytes)
  $out = @()
  for ($i = 0; $i -lt $b64.Length; $i += 128) { $out += $indent + $b64.Substring($i, [Math]::Min(128, $b64.Length - $i)) }
  return $out
}

$lines = Get-Content $Rpp
$result = New-Object System.Collections.Generic.List[string]
$block = -1; $inside = $false; $b64 = @(); $indent = ''
foreach ($line in $lines) {
  if (-not $inside -and $line.TrimStart().StartsWith('<VST ')) {
    $inside = $true; $block++; $b64 = @(); $result.Add($line); continue
  }
  if ($inside) {
    if ($line.Trim() -eq '>') {
      # Decode REAPER's block line by line, read its header, swap the state.
      $bytes = New-Object System.Collections.Generic.List[byte]
      foreach ($l in $b64) { $bytes.AddRange([Convert]::FromBase64String($l.Trim())) }
      $all = $bytes.ToArray()
      $at = 8
      $nIn = [BitConverter]::ToUInt32($all, $at); $at += 4 + 8 * $nIn
      $nOut = [BitConverter]::ToUInt32($all, $at); $at += 4 + 8 * $nOut
      $oldSize = [BitConverter]::ToUInt32($all, $at)
      $headerEnd = $at + 12
      $nameBytes = $all[($headerEnd + $oldSize)..($all.Length - 1)]

      $n = $liveIndex[$block]
      $comp = [IO.File]::ReadAllBytes("$StateDir\$n.component")
      $ctrlFile = "$StateDir\$n.controller"
      $ctrl = if (Test-Path $ctrlFile) { [IO.File]::ReadAllBytes($ctrlFile) } else { [byte[]]@() }
      $state = [byte[]]((U32 $comp.Length) + (U32 1) + $comp + (U32 $ctrl.Length) + (U32 1) + $ctrl)
      $header = [byte[]]($all[0..($at - 1)] + (U32 $state.Length) + $all[($at + 4)..($headerEnd - 1)])
      $indent = ($b64[0] -replace '^(\s*).*$', '$1')
      foreach ($l in (Lines $header $indent)) { $result.Add($l) }
      foreach ($l in (Lines $state $indent)) { $result.Add($l) }
      foreach ($l in (Lines ([byte[]]$nameBytes) $indent)) { $result.Add($l) }
      $result.Add($line); $inside = $false; continue
    }
    $b64 += $line; continue
  }
  $result.Add($line)
}
[IO.File]::WriteAllLines($Out, $result)
"blocks rewritten: $($block + 1)"
