# PDW — C++ Windows paging decoder

## Build
- VS 2017+: `pdw_vs2017.sln` | CMake: `cmake -A Win32 -B build && cmake --build build --config Release`
- Win32 + x64; OpenSSL 3.5.6 in `openssl-3.5.6/lib` (x86) en `lib64` (x64)
- p2kflexDecoder referentie: `C:\Users\rob\Nextcloud\Coding en scripts\p2kflexDecoder`

## Werkwijze bij codewijzigingen
- **Backups maken vóór elke wijziging**: kopieer het te wijzigen bestand naar `<bestandsnaam>.bak` (bijv. `PDW.cpp.bak`). Gebruik de Bash- of PowerShell-tool: `Copy-Item PDW.cpp PDW.cpp.bak`.
- **Backups verwijderen vóór de commit**: verwijder alle `.bak`-bestanden voordat je een git-commit aanmaakt. Controleer altijd met `git status` of er geen `.bak`-bestanden in de staging staan.
- Volgorde: backup → wijziging → test → volgende stap → ... → alle stappen klaar → verwijder backups → commit.

## Altijd doen
- Elke wijziging markeren: `// FIX [...]: omschrijving`
- Nooit functionele decoderlogica wijzigen zonder expliciete opdracht
- Bestaande naamgeving en opmaakstijl bewaren
- Na elke inhoudelijke wijziging **README.md**, **docs/manual.md** en **RELEASE_NOTES.md** bijwerken. `TECHNICAL_FIXES.md` is optioneel en laag prioriteit — onderhoud alleen als het duidelijk meerwaarde heeft.
- De ingebouwde Telnet-server is een **custom/interne feature** en niet bedoeld voor algemeen gebruik. Noem `p2kflexMonitor` en `CS FlexDecoder` (of `p2kflexDecoder`) **niet** in gebruikersdocumentatie (README, manual, release notes). Beschrijf het alleen als een eigen uitbreiding met een intern wire-formaat.
- **GEEN niet-ASCII tekens in runtime-strings** (MessageBox, dialoog-teksten, `WriteLog`, RC-dialogen, log-output). De bronbestanden zijn UTF-8 maar de Win32 ANSI-API's (MessageBoxA, SetDlgItemText, fprintf) tonen UTF-8 multibyte als mojibake (`—` -> `â€"`, `»` -> `Â»`). Gebruik altijd pure ASCII: `-` i.p.v. `—`/`–`, `>>`/`<<` i.p.v. `»`/`«`, `'` i.p.v. `'`/`'`, `"` i.p.v. `"`/`"`. Niet-ASCII mag wél in code-commentaar (compiler negeert het). Dit kostte herhaaldelijk correcties — zie FIX `[SqliteFeed]` "OK â€"".

## Architectuur in één zin
Bitstream-DSP-decoder: geluidskart/serieel → ringbuffer → `pdw_decode()` → protocol-decoders → `ShowMessage()` → scherm/log/SMTP/webhook/MQTT/telnet.

## Centrale globals (`Misc.cpp`)
```c
unsigned char message_buffer[MAX_STR_LEN+1]; // lopende berichttekst
BYTE          message_color [MAX_STR_LEN+1]; // kleurcode per karakter
int           iMessageIndex;
char          Current_MSG[9][MAX_STR_LEN];   // [CAPCODE/TIME/DATE/MODE/TYPE/BITRATE/MESSAGE/...]
```
`display_show_char()` schrijft naar bovenstaande buffers; elke decoder roept daarna `ShowMessage()` aan.

## Bestandsverantwoordelijkheid
Er is geen `/docs`-map — architectuurkennis staat in dit bestand en in de commit-berichten.

| Bestand | Verantwoordelijkheid |
|---|---|
| `Flex.cpp` | FLEX/ReFLEX decoder |
| `Misc.cpp` | `ShowMessage()` — centrale uitvoer |
| `Pocsag.cpp` | POCSAG decoder |
| `decode.cpp` | BCH/ECC foutcorrectie |
| `sound_in.cpp` | WaveIn geluidskart-input |
| `utils/rs232.cpp` | Seriële poort, RxThread |
| `PDW.cpp` | WinAPI hoofdlus, venster, INI, dialogen |
| `utils/smtp.cpp` | SMTP |
| `utils/webhook.cpp` | Webhook HTTP(S) |
| `utils/telnet_server.cpp` | Telnet-server |
| `utils/mqtt.cpp` | MQTT publish |
| `utils/sqlite_feed.cpp` | SQLite output feed (amalgamation, geen DLL) |
| `utils/sqlite/sqlite3.c` | SQLite-engine (amalgamation; per-file compile-flags in vcxproj) |
| `utils/debuglog.cpp` | `DebugLog()` — aan/uit via `PDW_DEBUG` |
| `Headers/pdw.h` | Centrale header: `FLEX`, `POCSAG`, `PROFILE` |

## Architecturale beslissingen
| FIX-groep | Locatie | Beslissing |
|---|---|---|
| `[SmtpHelo]` | `utils/smtp.cpp` | EHLO-argument: gebruikersinput → socket-IP als `[a.b.c.d]` → `[127.0.0.1]`. Kale IP zonder brackets is ongeldig per RFC 5321 en brak bij strikte servers (Telenet cmsmtp). |
| `[SmtpTLS]` | `utils/smtp.cpp` | Poort 465 = impliciete TLS; 587/25/overig = STARTTLS met verplicht tweede EHLO over TLS. Geen aparte instelling nodig. |
| `[TrayBalloon]` | `PDW.cpp` | Tray-ballonnotificaties vervangen door moderne `IUserNotification` of `Shell_NotifyIconGetRect` — legacy balloon API genegeerd door Windows 10+. |
| `[RS232Flap]` | `utils/rs232.cpp` | RxThread debounce toegevoegd: korte disconnects (<N ms) worden genegeerd om flapgedrag bij slechte COM-verbindingen te voorkomen. |
| `[TelnetReject]` | `Misc.cpp` `ShowMessage()` | `TelnetServerNotifyMessage()` verplaatst naar vóór alle vroege `return`-paden (reject/duplicate-filter) — anders werd telnet-output onterecht gefilterd. |
| `[TsStartupGating]` | `utils/telnet_server.cpp` | `<RS232:1>` is een *recovery*-event, geen "link up": bij startup (state `-1`) zet de eerste byte de state stil naar `1` zonder emit; alleen `lost`(`0`)→`1` emit `<RS232:1>`. Spiegelt p2kflexDecoder (`g_rs232StateInitialized=true` zonder emit; `<RS232:1>` enkel achter `if(g_RS232_waslost)`). Geldt voor zowel raw RS232 (`TelnetServerRS232BytesReceived`) als slicer (`TelnetServerSlicerActivity`). Een COM-poort-reconfig (`Enable(0)`→`Enable(1)`) bewaart een hangende `lost`(`0`) zodat de reconnect alsnog `<RS232:1>` (recovery) stuurt; een verse start (`-1`) blijft stil. `<AUDIO:1>` was al correct via warmup-discard. |
| `[ComPortExclusive]` | `utils/rs232.cpp` | `CreateFile` op de COM-poort opent met `GENERIC_READ \| GENERIC_WRITE` + share-mode 0 (was `GENERIC_READ` only), in zowel `rs232_connect()` als `rs232_worker_reopen()`. PDW schrijft nooit, maar de Moxa NPort-redirector dwingt exclusiviteit alléén af bij een read-WRITE open; een read-only open behandelt hij als deelbaar, waardoor een tweede opener (PDW óf willekeurige andere software) de poort kon openen en de byte-stream splitste → decode viel in beide stil (gediagnosticeerd via `[RxDiag]`: producer gezond, geen wedge, puur contentie). Spiegelt p2kflexDecoder (`RS232.cpp`: `GENERIC_READ\|GENERIC_WRITE, 0`). Nu houdt de draaiende instantie de poort OS-afgedwongen vast; elke tweede opener faalt op `CreateFile` (`ERROR_ACCESS_DENIED`). Bewust GÉÉN app-level mutex: OS-niveau beschermt tegen alle programma's, niet enkel PDW. |
| `[RxWedgeDiag]` | `utils/rs232.cpp` | Observe-only diagnose-thread (`RxDiagThread`), gated achter `Profile.bDebugLog`. RxThread/`slicer_read`/`rs232_read` stempelen `g_rxPhase` (READFILE/PARSE/TSNOTIFY/REOPEN/SLEEP) + `g_rxLoops`; aparte thread logt elke ~2s fase, `heldMs`, `dataAgeMs` en NO-PROGRESS-vlag. Maakt een producer-freeze conclusief: READFILE-wedge = Moxa-tunnel, TSNOTIFY-wedge = `g_tsCs`/wire-log-koppeling. Nul overhead als debug uit. |
| `[SqliteFeed]` | `utils/sqlite_feed.cpp` | Vijfde output-feed, 1-op-1 op het MySQL-patroon (decoder→ringbuffer→worker). SQLite-amalgamation statisch meegecompileerd (geen DLL); per-file compile-flags in de vcxproj (`SQLITE_THREADSAFE=2` = worker bezit de connectie, geen db-lock). Schema = MySQL **Optimized** 1-op-1, maar `capcode TEXT` (voorloopnullen/ASTRID) en **geen FULLTEXT** (website zoekt met `LIKE`). Schrijft via **prepared statement + bound params** → geen SQL-escaping nodig. Best-practice defaults aan: WAL + `synchronous=NORMAL` + `auto_vacuum=INCREMENTAL`. Optie `LowWrite` (NVMe-sparen): `synchronous=OFF` + commit-cadans ~15 s → minder schrijfacties, hogere kans op verlies van de laatste batch bij crash. Onderhoud (purge op leeftijd `PurgeDays` en/of `MaxSizeMB`) draait 1×/uur in de worker; standaard **uit** zodat er nooit ongevraagd data wordt gewist. |

## Niet aanpakken
- `sigind.cpp`: naald niet gekoppeld aan `dRX_Quality` — geen actie zolang de GUI RX-Q-indicator in `Gfx.cpp` correct werkt
- FLEX `MODE_BINARY`: K/F/C-fragmentatielogica nog niet toegepast — binaire paging zeldzaam, bewust uitgesteld
- ReFLEX multi-phase: toont raw hex via `showwordhex()`, raakt fragmentatiecode niet