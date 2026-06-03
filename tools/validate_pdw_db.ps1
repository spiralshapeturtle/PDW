<#
.SYNOPSIS
    Valideert de PDW SQLite-database (FIX [SqliteFeed]).

.DESCRIPTION
    Zoekt automatisch PDW.db naast PDW.exe (Desktop\PDW\ of Release\).
    Plaatst sqlite3.exe naast dit script (tools\) voor offline gebruik.
    Download: sqlite.org -> Precompiled Binaries for Windows -> sqlite-tools-win-x64-*.zip

.PARAMETER Table
    Tabelnaam. Default: alarmeringen.
#>
[CmdletBinding()]
param(
    [string]$Table = "alarmeringen"
)

$ErrorActionPreference = "Stop"
function Ok   ($m) { Write-Host "[OK]   $m" -ForegroundColor Green }
function Warn ($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Fail ($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }

# --- 1. DB zoeken ---------------------------------------------------------------
# Volgorde: naast PDW.exe op Desktop, dan Release-map naast dit script
$candidates = @(
    (Join-Path $env:USERPROFILE "Desktop\PDW\PDW.db"),
    (Join-Path $env:USERPROFILE "Desktop\PDW\pdw.db"),
    (Join-Path $PSScriptRoot   "..\Release\pdw.db"),
    (Join-Path $PSScriptRoot   "..\pdw.db")
)
$Path = $null
foreach ($c in $candidates) {
    if (Test-Path $c) { $Path = (Resolve-Path $c).Path; break }
}
if (-not $Path) {
    Fail "PDW.db niet gevonden op:`n  $($candidates -join "`n  ")`nStart PDW en enable de SQLite-feed, of geef het pad mee als eerste argument."
}

$size = (Get-Item $Path).Length
Write-Host "Database : $Path"
Write-Host "Grootte  : $([math]::Round($size/1KB,1)) KB`n"

# --- 2. SQLite-header check (geen externe tool nodig) ---------------------------
$fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
      [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
try { $hdr = New-Object byte[] 15; [void]$fs.Read($hdr, 0, 15) } finally { $fs.Close() }
$magic = [System.Text.Encoding]::ASCII.GetString($hdr)
if ($magic -ne "SQLite format 3") { Fail "Geen geldig SQLite-bestand (header: '$magic')" }
Ok "Geldig SQLite-header"

# --- 3. sqlite3.exe lokaliseren -------------------------------------------------
$toolDir = $PSScriptRoot   # tools\ naast dit script
$sqlite3  = $null
foreach ($d in @($toolDir, (Split-Path $Path), ".")) {
    $c = Join-Path $d "sqlite3.exe"
    if (Test-Path $c) { $sqlite3 = (Resolve-Path $c).Path; break }
}
if (-not $sqlite3) {
    $cmd = Get-Command sqlite3.exe -ErrorAction SilentlyContinue
    if ($cmd) { $sqlite3 = $cmd.Source }
}
if (-not $sqlite3) {
    Warn "sqlite3.exe niet gevonden."
    Write-Host "  Zet sqlite3.exe naast dit script: $toolDir"
    Write-Host "  Download: sqlite.org -> Precompiled Binaries for Windows -> sqlite-tools-win-x64-*.zip"
    Write-Host ""
    Write-Host "Alleen header-validatie uitgevoerd (OK)."
    exit 0
}
Ok "sqlite3.exe : $sqlite3"

function Q([string]$sql) {
    $out = & $sqlite3 $Path $sql 2>&1
    return $out
}

# --- 4. Integriteit -------------------------------------------------------------
Write-Host ""
$integ = (Q "PRAGMA integrity_check;") -join "; "
if ($integ -eq "ok") { Ok "integrity_check : ok" } else { Fail "integrity_check: $integ" }

$quick = (Q "PRAGMA quick_check;") -join "; "
if ($quick -eq "ok") { Ok "quick_check     : ok" } else { Warn "quick_check: $quick" }

$jmode = (Q "PRAGMA journal_mode;") -join ""
if ($jmode -eq "wal") { Ok "journal_mode    : wal (persistent in db-bestand)" }
else                  { Warn "journal_mode    : $jmode (verwacht: wal -- open PDW met feed enabled om te fixen)" }

$sync = (Q "PRAGMA synchronous;") -join ""
$synctxt = switch ($sync) { "0"{"OFF"}; "1"{"NORMAL"}; "2"{"FULL"}; default{$sync} }
Write-Host "  synchronous=$sync ($synctxt) -- per-connectie instelling, niet opgeslagen in db; PDW worker gebruikt NORMAL"

# --- 5. Schema-verificatie (Optimized) ------------------------------------------
Write-Host ""
$cols = Q "SELECT name FROM pragma_table_info('$Table');"
if (-not $cols) { Fail "Tabel '$Table' bestaat niet in $Path" }

$expected = @("id","ontvangen","capcode","mode","msg_type","bitrate",
              "message","label","subscribers","match_type","label_color")
$missing = $expected | Where-Object { $_ -notin $cols }
if ($missing) { Fail "Ontbrekende kolommen: $($missing -join ', ')" }
else          { Ok "Alle $($expected.Count) Optimized-kolommen aanwezig" }

$capType = (Q "SELECT type FROM pragma_table_info('$Table') WHERE name='capcode';") -join ""
if ($capType -match "TEXT") { Ok "capcode is TEXT  (ASTRID-voorloopnullen intact)" }
else                        { Fail "capcode is '$capType' - moet TEXT zijn" }

$idx = (Q "SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='$Table';") | Where-Object { $_ }
if ($idx) { Write-Host "  Indexen: $($idx -join ', ')" }
else      { Warn "Geen indexen gevonden op tabel '$Table'" }

# --- 6. Inhoud ------------------------------------------------------------------
Write-Host ""
$count = (Q "SELECT COUNT(*) FROM '$Table';") -join ""
Ok "Records: $count"

if ([int64]$count -gt 0) {
    Write-Host ""
    Write-Host "Laatste 5 records:" -ForegroundColor Cyan
    & $sqlite3 -header -column $Path `
        "SELECT ontvangen, capcode, msg_type, substr(message,1,45) AS message FROM '$Table' ORDER BY id DESC LIMIT 5;"

    # Subscribers JSON validatie
    $subRows = Q "SELECT subscribers FROM '$Table' WHERE subscribers != '';"
    if ($subRows) {
        $ok = 0; $fail = 0
        foreach ($r in $subRows) {
            try { $null = $r | ConvertFrom-Json; $ok++ }
            catch { $fail++ }
        }
        if ($fail -eq 0) { Ok "Subscribers JSON  : $ok GROUP-rijen, allemaal geldig" }
        else             { Warn "Subscribers JSON  : $fail van $($ok+$fail) rijen ongeldig" }
    }
}

Write-Host ""
Write-Host "Validatie voltooid - geen fouten gevonden." -ForegroundColor Green
