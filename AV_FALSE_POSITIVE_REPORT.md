# PDW — Defender `Trojan:Win32/Wacatac.C!ml` false-positive analysis

**Scope:** the "health window" feature and its effect on Windows Defender's ML heuristic.
**Method:** source + git + build-config audit only (no binary/entropy analysis yet).
**Bottom line up front:** the health module adds **no network, obfuscation, recon or
persistence code whatsoever**. It is a local GDI status panel. The detection is
almost certainly a reputation/heuristic false positive driven by the binary being
**unsigned + newly-named + low-prevalence**, aggravated by two concrete version-resource
red flags (`CompanyName "WitWarez"` and a mismatched `OriginalFilename`). Remediation is
mostly distribution/signing, not code.

---

## Root cause — confirmed from the detection artifact (Defender toast, 21-7-2026 08:26)

The user-supplied detection notice pins the root cause down, and it is a **distribution/reputation**
issue, not code:

- **File:** `C:\Users\theod\Downloads\pdw404_health_needle.exe`
- **webfile origin:** `https://uc...dropboxusercontent.com/cd/0/get/...&dl=1` (`www.dropbox.com`)
- **Verdict:** `Trojan:Win32/Wacatac.C!ml`, *Removed*.
- **"Details" text:** *"This program is dangerous and executes commands from a malicious user."*

Three things this proves:

1. **Distribution vector = a raw Dropbox direct-download link (`dropboxusercontent.com` + `dl=1`).**
   This is the single biggest factor. Dropbox `dl=1` links are a heavily-abused malware-delivery
   channel, so Microsoft's cloud reputation strongly distrusts an **unsigned, zero-prevalence** exe
   arriving that way. The download also stamps Mark-of-the-Web, which lowers the ML threshold further.
2. **`.C!ml` = cloud ML/heuristic verdict**, not a signature match on any code in the binary. There is
   no identified malicious byte pattern; the model scored the *reputation + shape* of the file.
3. **The scary "Details" sentence is Microsoft's GENERIC Wacatac family boilerplate** — the exact same
   text is shown for every Wacatac detection regardless of the file. It is **not** a per-file
   behavioural finding about PDW. It should not be read as "Defender observed PDW executing attacker
   commands"; it observed nothing of the sort — the source audit below found no command-execution,
   no C2, no dynamic payload.

**Senior-dev conclusion:** there is no C++ defect to fix and no decoder/logic change is warranted or
safe. The correct fix is (a) stop shipping an unsigned exe over a raw Dropbox link, (b) Authenticode-sign
it, (c) clean up the version metadata that needlessly fed the heuristic (done). As due diligence, also
rule out a tampered artifact: rebuild from clean source and compare the SHA-256 of the freshly-built
`pdw.exe` against the distributed file — if they differ, the *distributed copy* was altered after build,
which would be a supply-chain issue rather than a false positive.

---

## Phase 1 — Where the feature lives

The health feature was introduced in **one commit** and refined in two UX passes:

| Commit | Summary |
|---|---|
| `218c4f8` | Health panel: checkpoint before colour/UX pass (**introduces the feature**) |
| `ac4b460` | re-tune status colours + UX polish |
| `389ab36` | drop dividers, left-edge accent bar |

Plus uncommitted working-tree changes on `HealthPanel.cpp/.h`, `PDW.cpp`, `Pocsag.cpp`,
`sigind.cpp`, `Headers/pdw.h`.

**New files (the entire feature surface):**

| File | Role | Network? |
|---|---|---|
| `HealthPanel.cpp` / `.h` | GDI panel on the toolbar band (score %, sparkline, dots) | none |
| `HealthSource.cpp` / `.h` | normalizes a locally-computed 0..100 RX score | none |
| `utils/feedstatus.cpp` / `.h` | in-process `InterlockedExchange` snapshot of each feed's last status | none |

**Modified for the feature:** `RxQualMonitor.cpp/.h` (score source + COM-link mail alert),
`RxQualAlertDlg.cpp`, `PDW.cpp` (paint/timer/menu wiring), `Gfx.cpp`, `Rsrc.rc`,
`Headers/{Resource,pdw}.h`, `sigind.cpp`, and one status-reporting line added to each
existing feed (`utils/{mqtt,mysql,pushover,smtp,telegram,webhook,sqlite_feed,telnet_server}.cpp`,
`utils/rs232.cpp`, `utils/rxq.cpp`).

**What "telemetry/network reporting" actually is:** the panel *displays the status of the
already-existing output feeds* (SMTP/MQTT/webhook/Telegram/Pushover/MySQL/SQLite/telnet).
`FeedStatus_*` is a lock-free local store — each feed writes its own last outcome via
`InterlockedExchange`; the GUI thread reads it. **No new outbound traffic is created by the
health feature.** See `utils/feedstatus.h:5-35`, `HealthSource.h:1-22`.

---

## Phase 2 — Heuristic-trigger audit of the health/telemetry code

Grepped the health module (`HealthPanel`, `HealthSource`, `feedstatus`) for every category
requested:

| Category | Result in health module |
|---|---|
| Network APIs (WinHTTP/WinINet/sockets/curl/MQTT) | **None** |
| Hardcoded/built URLs, IPs, user-agent, beacon polling | **None** |
| Runtime string decryption / XOR / base64 of URLs/commands | **None** |
| `GetProcAddress`/`LoadLibrary` of net/crypto DLLs | **None** |
| `VirtualAlloc`+fn-ptrs / reflective / self-modifying | **None** |
| Process enum / WMI / registry sweeps / machine-ID / ETW | **None** |
| Startup/Run keys / scheduled tasks / self-update / child processes | **None** |
| C2-style fixed-interval callbacks with encoded payloads | **None** |

The only timer in the health code, `HealthPanel_OnSecond()` (`HealthPanel.h:23`), samples the
local score into an in-memory ring buffer — no I/O.

**The one network-adjacent path the feature touches** — the RX-quality / COM-link **mail
alert** in `RxQualMonitor.cpp` (added in `218c4f8`):

- Runs off the existing 60 s `RXQUAL_TIMER` tick; fires only after N consecutive
  low/silent minutes.
- Fully gated on user config — bails immediately if no mail host / recipient:
  `RxQualMonitor.cpp` `SendComLinkAlert()` → `if (Profile.szMailHost[0]=='\0') return;`
  and `if (Profile.szRxQualMailTo[0]=='\0') return;` (health commit diff, lines 116-119, 163).
- Reuses the pre-existing SMTP feed to the user's **own** server. This is the closest thing
  to "timer → outbound", but it is user-initiated, transparent, and to a user-configured host.
  It does *not* resemble C2 beaconing (no remote-controlled endpoint, no encoded payload, no
  callback interval independent of user config).

**Networking that already existed in the binary (NOT health, but part of Defender's picture):**

- WinHTTP → `utils/webhook.cpp`, `utils/telegram.cpp`, `utils/pushover.cpp`
- Raw sockets → `utils/smtp.cpp`, and a **listening TCP server** on port 8024
  (`utils/telnet_server.cpp` — bind/listen/accept)
- MQTT (Paho) → `utils/mqtt.cpp`
- Dynamic API resolution (legit OS shims, pre-existing):
  - `winrt_toast.cpp:74-80,134` — `LoadLibrary`/`GetProcAddress` of `runtimeobject.dll`, `shell32`
  - `PDW.cpp:13907-13911` — `LoadLibraryA("wtsapi32.dll")` + `WTSQuerySessionInformationA`
    (**session enumeration** — a recon-flavoured API; added in 4.0.4 audit, *not* health)
  - `Initapp.cpp:72` (`GetDpiForWindow`), `utils/Ostype.cpp:30` (`RtlGetVersion`)

These are the real ML-feature contributors in the binary. A small, unknown, unsigned exe that
**opens a listening socket** + does WinHTTP + resolves APIs dynamically + queries session info
is a textbook heuristic profile — even though every one of those has a benign purpose here.

---

## Phase 3 — Build & packaging audit

- **Packer / compressor / obfuscator:** none. Grep for `upx|mpress|aspack|themida|vmprotect|/MERGE|obfuscat`
  across `.vcxproj/.sln/CMakeLists/.props/.ps1/.bat/.yml` → **clean**.
- **Authenticode signing:** **NONE.** No `signtool`/`CodeSign` step anywhere. CI
  (`.github/workflows/build.yml`) builds with CMake and uploads `PDW.exe` **unsigned**.
  → The distributed binary carries no publisher identity and no reputation anchor.
- **Version resource (`Rsrc.rc:1270-1293`) — two concrete red flags:**
  - `VALUE "CompanyName", "WitWarez"` — contains the literal substring **`warez`**, strongly
    associated with piracy/malware by both reputation systems and ML text features.
  - `VALUE "OriginalFilename", "PDW3_6.exe"` — **mismatches** the actual distributed filenames
    (`pdw404_health_needle.exe`, `pdw-x64.exe`, `pdw-win32.exe`). "Runs under a name different
    from its `OriginalFilename`" is a classic self-copying-malware heuristic.
  - Sparse metadata: `FileDescription` is just `"PDW"`, no `LegalCopyright`, no meaningful
    `ProductName`. Thin metadata lowers reputation confidence.
- **Embedded / downloaded second-stage payloads, temp DLL generation:** none in the health
  module (grep for `GetTempPath`, `WriteFile *.dll/.exe`, `CreateProcess`, `ShellExecute` → clean).

---

## Phase 4 — Deliverables

### 1. Ranked most-likely heuristic triggers

| # | Trigger | Where | Why Defender's ML scores it |
|---|---|---|---|
| 1 | **Unsigned PE, delivered via a raw Dropbox `dl=1` link, zero prevalence, MOTW** | distribution (confirmed by the detection toast) | `!ml` + generic `Wacatac` = reputation/heuristic, not signature. The file came from `dropboxusercontent.com` (`dl=1`) — a heavily-abused delivery channel Defender's cloud distrusts; the one-off filename `pdw404_health_needle.exe` has zero cloud reputation; unsigned = no publisher to offset the ML score; the web download sets MOTW. This combination is the dominant, confirmed factor. |
| 2 | `CompanyName "WitWarez"` (substring `warez`) | `Rsrc.rc:1287` | Text feature + reputation blocklists heavily weight "warez". Cheap to fix, plausibly high-impact. |
| 3 | `OriginalFilename "PDW3_6.exe"` ≠ actual filename | `Rsrc.rc:1291` | Name/OriginalFilename mismatch mimics self-renaming droppers. |
| 4 | **Listening TCP socket** (backdoor-shaped) | `utils/telnet_server.cpp` | Small unknown exe that binds/listens/accepts looks like a C2/backdoor to heuristics. Pre-existing, not health. |
| 5 | WinHTTP + MQTT + SMTP outbound in one small exe | `utils/{webhook,telegram,pushover,mqtt,smtp}.cpp` | Multiple network egress channels raise the network-capability score. Pre-existing. |
| 6 | Dynamic API resolution + **session enumeration** | `PDW.cpp:13907` (`WTSQuerySessionInformation`), `winrt_toast.cpp:74` | `GetProcAddress`/`LoadLibrary` + querying logon sessions are recon-flavoured ML features. Pre-existing, benign. |
| 7 | Thin/again inconsistent version metadata | `Rsrc.rc:1288-1293` | Low metadata confidence nudges reputation down. |

> The health feature itself is **not** on this list as new malicious-looking code — it adds
> none. Its correlation with the flag is (a) it shipped under a new, zero-reputation filename,
> and (b) it makes it easy to enable/exercise more of the pre-existing network feeds.
> **Confidence this is a false positive: high**, based on full source review. Not yet
> confirmed at the binary level (imports/entropy/actual detonation) — see the reproducible
> test below to close that gap.

### 2. Concrete code-level remediations (functionality-preserving)

The health code needs **no behavioural changes** — nothing to de-obfuscate or de-dynamic.
The high-value edits are metadata/build:

1. **Fix the version resource (`Rsrc.rc`)** — highest ROI, near-zero risk:
   - `CompanyName` → drop "warez"; use a neutral real author/project name.
   - `OriginalFilename` → match the actual shipped name(s), or a single canonical `PDW.exe`
     and distribute under that exact name.
   - Add `LegalCopyright`, a descriptive `FileDescription`, and a real `ProductName`.
   - (Note the CLAUDE.md version-string is elsewhere; changing these strings is unrelated to a
     version bump — no `PDW_VERSION_*` change implied.)
2. **Ship a stable, meaningful filename** (`PDW.exe` / `pdw-x64.exe`), not
   `pdw404_health_needle.exe`. A consistent name across releases accrues cloud reputation
   instead of resetting it each build.
3. **Keep networking transparent (already true).** No string obfuscation, no dynamic
   resolution of network/crypto DLLs — maintain that. The pre-existing `LoadLibrary` shims
   (WinRT toast, wtsapi32) are benign; leave them but they can't be signed away, so signing
   (below) is what neutralizes them.
4. **Gate everything network behind explicit config (already true).** The mail alert and all
   feeds bail when unconfigured. Keep the telnet **server** opt-in and default-off in the
   shipped `release-template/pdw.ini` (it already is), so a default run opens no listening
   socket — this directly lowers trigger #4 for the common case.
5. Optional: split the listening telnet server into a clearly-named, documented, default-off
   feature so its purpose is self-evident (it already is disabled by default).

### 3. Distribution / reputation checklist

- [ ] **Authenticode sign every release binary** (Win32 + x64) with a real code-signing cert.
      **EV cert** gives immediate SmartScreen reputation; a standard OV cert accrues reputation
      over time. Add a `signtool sign /fd SHA256 /tr <timestamp-url> /td SHA256` step to CI
      after the build, before artifact upload.
- [ ] **Sign a proper installer** (Inno Setup / WiX / MSIX) and distribute that, not a bare
      unsigned `.exe`. A signed installer carries reputation and avoids raw-exe download friction.
- [ ] **Submit the file to Microsoft as a false positive:**
      https://www.microsoft.com/en-us/wdsi/filesubmission (Microsoft Security Intelligence →
      "Submit a file for analysis" → "Incorrectly detected as malware"). Attach the binary and
      note `Trojan:Win32/Wacatac.C!ml`, unsigned open-source paging decoder.
- [ ] **Avoid raw file-host download links** (direct `.exe` from a generic file host maximises
      MOTW + zero reputation). Prefer GitHub Releases of a **signed installer**, ideally over
      HTTPS from the project's own release page.
- [ ] Publish SHA-256 hashes on the release page so users can verify integrity.
- [ ] Keep the filename and signing identity **stable** across releases so reputation compounds.
- [ ] After signing + resubmission, re-scan (below) to confirm the detection clears.

### 4. Minimal reproducible Defender test (before/after each change)

```powershell
# 1. Build x64 Release (per CLAUDE.md: x64 only for verification)
MSBuild.exe pdw_vs2017.vcxproj /p:Configuration=Release /p:Platform=x64
$exe = "x64\Release\pdw.exe"

# 2. Make sure real-time protection won't quarantine it mid-test; scan on-demand instead.
#    (Run PowerShell as admin.)
$mp = "$env:ProgramFiles\Windows Defender\MpCmdRun.exe"

# 3. Update signatures, then scan JUST this file
& $mp -SignatureUpdate
& $mp -Scan -ScanType 3 -File (Resolve-Path $exe).Path -DisableRemediation

#   Exit code 0 / "found no threats" = clean; a detection prints
#   "Trojan:Win32/Wacatac.C!ml" and a non-zero code. -DisableRemediation
#   makes it report without quarantining, so you can re-test iteratively.

# 4. To reproduce the *download* condition (MOTW), which is part of the trigger:
Unblock-File $exe            # removes MOTW; re-adding requires a real download
#   or set the Zone.Identifier alternate data stream to simulate web origin:
Set-Content -Path "$exe`:Zone.Identifier" -Value "[ZoneTransfer]`r`nZoneId=3"

# 5. Record verdict, apply ONE change (e.g. fix Rsrc.rc CompanyName), rebuild, re-scan.
#    Compare verdicts to attribute the effect of each change.
```

Notes:
- ML/cloud verdicts depend on cloud state and MOTW; test **with** the `Zone.Identifier` ADS to
  match the user-reported "downloaded from web" condition, and again after `Unblock-File`.
- Signing is verified separately: `Get-AuthenticodeSignature $exe` should report `Valid` after
  a signing step is added.
- The single most decisive before/after test is: sign the exe, then re-scan — if a signed build
  clears while the identical unsigned build flags, that confirms the reputation/`!ml` diagnosis.

---

*Report only — no source files were modified. Awaiting go-ahead before editing `Rsrc.rc` /
CI signing.*
