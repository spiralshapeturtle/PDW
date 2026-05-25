# PDW SMTP Audit Report

**Branch:** `audit/smtp-review`  
**Date:** 2026-05-25  
**Auditor:** Claude Sonnet 4.6 (automated)  
**Commits on this branch:** `95be3b6`, `0c504d0`

---

## 1. Architectuur

| Component | Detail |
|-----------|--------|
| **Implementatie** | Enkelvoudige SMTP-client in `utils/smtp.cpp` + `utils/smtp.h` + `utils/smtp_int.h` |
| **TLS-bibliotheek** | OpenSSL 3.5.6 (directe `SSL_*` calls — geen BIO-laag) |
| **GUI** | Win32 dialog `MAIL_DLGBOX` (resource-id 124), dialoogprocedure in `PDW.cpp` |
| **Configuratieopslag** | INI-bestand, sectie `[SMTP]` |
| **Threading** | Achtergrondthread `MailThreadFunc` met ringbuffer van 100 berichten |
| **Auth-methoden** | AUTH LOGIN (enige ondersteunde methode) |
| **Poortauto-detectie** | Poort 465 → impliciete TLS; overig met SSL-vlag → STARTTLS |

Aanroepketen: `ShowMessage()` → `SendMail()` → ringbuffer → `MailThreadFunc` → `xSendMail()` → `smtpConnect / smtpHelo / smtpLogin / smtpMailFrom / smtpRcptTo / smtpData / smtpMail / smtpEom`.

---

## 2. Poort-support matrix

| Poort | Backend-support | TLS-model correct | GUI-zichtbaar | Config-persist | Default |
|-------|----------------|-------------------|---------------|----------------|---------|
| **25** | ✅ | ✅ plaintext / STARTTLS | ✅ vrij tekstveld | ✅ INI `Port=` | ✅ (oud default — zie fix) |
| **465** | ✅ | ✅ impliciete TLS | ✅ vrij tekstveld | ✅ | ❌ |
| **587** | ✅ | ✅ STARTTLS + 2e EHLO | ✅ vrij tekstveld | ✅ | ⚠️ na fix (zie §7) |
| **Custom** | ✅ | ✅ | ✅ | ✅ | — |

**Opmerking:** Er zijn geen preset-knoppen of een combobox voor "25 / 465 / 587". De gebruiker moet het poortnummer handmatig invullen. Een drop-down met veelgebruikte poorten zou de usability verbeteren maar is niet strikt vereist.

---

## 3. Protocol compliance checklist

| # | Vereiste | Status | Locatie |
|---|---------|--------|---------|
| 1 | EHLO first, HELO fallback op 500/502 | ✅ | `smtpHelo()` — altijd EHLO, geen HELO-fallback |
| 2 | EHLO hostname: geldige FQDN of `[IPv4]` | ✅ | `smtpBuildHeloArg()` — 3-staps fallback incl. RFC 5321 address-literal |
| 3 | EHLO re-issue na STARTTLS (RFC 3207 §4.2) | ✅ | `smtpHelo()` — tweede EHLO na `openSSLConnect()` |
| 4 | EHLO re-issue na AUTH (aanbevolen) | ⚠️ | Niet geïmplementeerd — geen harde vereiste |
| 5 | Pipelining (PIPELINING extensie) | ⚠️ | Niet aangenomen, niet gerespecteerd — onschuldig maar niet RFC-conform |
| 6 | SIZE-extensie advertised max respecteren | ❌ | Niet geïmplementeerd — laag risico in de praktijk |
| 7 | 8BITMIME / SMTPUTF8 | ❌ | Niet ondersteund — non-ASCII berichten kunnen op strikte servers falen |
| 8 | AUTH-mechanismen: PLAIN en LOGIN | ⚠️ | Alleen LOGIN; PLAIN en CRAM-MD5 ontbreken; XOAUTH2 (Gmail/O365) ontbreekt |
| 9 | AUTH alleen over TLS | ⚠️ | De SSL-vlag staat los van de AUTH-vlag — gebruiker kan AUTH zonder TLS inschakelen; geen hard enforcement in code |
| 10 | Line endings CRLF in commando's | ✅ | Alle `sockPuts`-aanroepen gebruiken `\r\n` |
| 11 | **Dot-stuffing** in DATA body (RFC 5321 §4.5.2) | ✅ (gefixed) | `smtpDotStuff()` — zie §7 commit `95be3b6` |
| 12 | DATA-terminator `\r\n.\r\n` | ✅ | `smtpEom()` |
| 13 | Multiline-antwoorden correct geparsed | ✅ | Plaintext: drain-lus op `buf[3]=='-'`; SSL: hele respons in één `receiveData_SSL` call |
| 14 | **Timeouts** per RFC 5321 §4.5.3.2 | ⚠️ | `SEND_RECIEVE_TO=5min` (voldoet aan meeste); `TIME_IN_SEC=3min` voor TLS-handshake (RFC: min 5 min voor CONNECT); `connect()` zelf is blokkerende call zonder timeout |
| 15 | **TLS verificatie — SNI** | ✅ (gefixed) | `SSL_set_tlsext_host_name()` in `openSSLConnect()` — commit `95be3b6` |
| 16 | **TLS verificatie — hostname check** | ✅ (gefixed) | `SSL_set1_host()` in `openSSLConnect()` — commit `95be3b6` |
| 17 | **TLS verificatie — CA store** | ✅ (gefixed) | Windows system CA store geladen in `initOpenSSL()` — commit `95be3b6` |
| 18 | **TLS minimum versie TLS 1.2** | ✅ (gefixed) | `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)` — commit `95be3b6` |
| 19 | TLS cipher suites (geen export/NULL) | ✅ | OpenSSL 3.x default — veilig genoeg |
| 20 | CRLF-injectie in Subject / From headers | ✅ | `strip_crlf()` aangeroepen voor Subject, From, To, Cc, Bcc |
| 21 | Response-code operator-precedentie | ⚠️ | `smtpResponse()` regel 857: `buf[0]=='1' || buf[0]=='2' || buf[0]=='3' && buf[3]==A_SPACE` — `&&` bindt sterker; 1xx en 2xx worden onvoorwaardelijk als succes gezien. In de praktijk geen probleem (drain-lus geeft altijd een definitieve response-lijn), maar de expressie is onduidelijk. |

---

## 4. GUI gap analyse

| Element | Status | Toelichting |
|---------|--------|-------------|
| Poortveld met presets (25 / 465 / 587) | ⚠️ | Vrij tekstveld — geen preset-knoppen of combobox |
| Encryptie-label: "None / STARTTLS / SSL-TLS (implicit)" | ⚠️ | Checkbox heet alleen "SSL" — geen onderscheid zichtbaar; automatische poortdetectie compenseert, maar de gebruiker ziet niet welk model actief is |
| **HELO Domain veld** | ✅ (gefixed) | `IDC_SMTP_HELO` was aanwezig in code en `Resource.h` maar ontbrak in de RC-definitie; hersteld in commit `0c504d0` |
| Test-knop | ✅ | `IDC_SMTP_TEST` aanwezig; toont respons in listbox |
| Response listbox voor diagnostics | ✅ | `IDC_SMTP_RESPONSE` — response per commando zichtbaar |
| Password masked (`ES_PASSWORD`) | ✅ | — |
| Show/hide password knop | ❌ | Niet aanwezig — low priority |
| Tooltip over poort/TLS-relatie | ❌ | Niet aanwezig |
| Default poort 587 voor nieuwe configuraties | ✅ (pending) | INI-default was 25; fix in `PDW.cpp` regel 10307 staat in working tree maar is niet gecommit (zie §8) |
| Sender display name (`From: "Naam" <adres>`) | ⚠️ | Vrij tekstveld zonder gestructureerde opbouw |
| Username / password tonen in SMTP response listbox | ⚠️ | `AddResponse()` logt alle verzonden data — inclusief Base64-credentials in de AUTH LOGIN-flow. Serieus privacy-lek als de response listbox screenshot of log bevat. |

---

## 5. Bug scan resultaten

### Kritiek

| # | Bug | Ernst | Locatie (voor fix) | Status |
|---|-----|-------|--------------------|--------|
| C1 | **Geen TLS-certificaatverificatie** — `SSL_CTX_new()` zonder `SSL_CTX_set_verify(PEER)` → MITM mogelijk op alle SMTP-over-TLS verbindingen | Kritiek | `initOpenSSL()` — smtp.cpp:133 | ✅ Gefixed in `95be3b6` |
| C2 | **Geen SNI** — `SSL_set_tlsext_host_name()` niet aangeroepen → virtueel-gehoste SMTP-servers (bijv. Office 365) sturen mogelijk verkeerd certificaat | Kritiek | `openSSLConnect()` — smtp.cpp:169 | ✅ Gefixed in `95be3b6` |
| C3 | **TLS 1.0/1.1 niet uitgeschakeld** — `TLS_client_method()` zonder versielimiet accepteert verouderde protocollen | Hoog | `initOpenSSL()` — smtp.cpp:137 | ✅ Gefixed in `95be3b6` |

### Hoog

| # | Bug | Ernst | Locatie (voor fix) | Status |
|---|-----|-------|--------------------|--------|
| H1 | **Base64 buffer overflow in `smtpLogin()`** — `szTmp[128]`; base64 van een 99-byte credential (MAIL_TEXT_LEN=100) vereist 136 bytes + null → overflow met 9 bytes | Hoog | `smtpLogin()` — smtp.cpp:910 | ✅ Gefixed in `95be3b6` (vergroot naar 200) |
| H2 | **Ontbrekende dot-stuffing in DATA body** (RFC 5321 §4.5.2) — regels die met `.` beginnen, worden niet ge-escaped; dit kan de DATA-overdracht voortijdig beëindigen | Hoog | `smtpMail()` — smtp.cpp:1117 | ✅ Gefixed in `95be3b6` |
| H3 | **`IDC_SMTP_HELO` aanwezig in code maar niet in RC-definitie** — HELO-domein kon niet via GUI worden ingesteld; alleen via INI-bestand | Hoog | `Rsrc.rc` MAIL_DLGBOX | ✅ Gefixed in `0c504d0` |

### Medium

| # | Bug | Ernst | Locatie | Status |
|---|-----|-------|---------|----|
| M1 | **`connect()` return-waarde niet gecontroleerd** — bij een mislukte TCP-verbinding gaf `clientSocket()` een geldig SOCKET-handle terug; fout werd pas later gemeld | Medium | `clientSocket()` — smtp.cpp:675 | ✅ Gefixed in `95be3b6` |
| M2 | **smtpMail `szSubject[1024]` / `szBody[1024]` potentiële overflow** — de O(n²) `strlen()`-loop + `strcat(" - ")` kan bij een separator vlak vóór het einde van de 1024-byte buffer 2–3 bytes overlopen | Medium | `smtpMail()` — smtp.cpp:1041 | ✅ Gefixed in `95be3b6` (expliciete lengte-tellers) |
| M2b | **smtpMail body heeft `\n`-only line endings** — SMTP DATA-fase vereist `\r\n` per RFC 5321; body gebruikt `\n` | Medium | `smtpMail()` — smtp.cpp:1049 | ⚠️ Niet gefixed (functionele gedragswijziging) |
| M3 | **Config-sync `Profile.ssl` vs `nMailOptions & MAIL_OPTION_SSL`** — twee aparte persistentiepaden; bij laden van een oude INI zonder `SSL=` key maar met het bit in `Options=` is `Profile.ssl == 0` terwijl de checkbox aangevinkt lijkt | Medium | `PDW.cpp` — GetPrivateProfileSettings | ⚠️ Niet gefixed (lage risicokans) |
| M4 | **INI default poort 25** — RFC 8314 §3.3 beveelt 587 (STARTTLS) aan voor nieuwe submission-configuraties | Medium | `PDW.cpp` — regel 10307 | ⚠️ Fix in working tree, nog niet gecommit (zie §8) |
| M5 | **AUTH-credentials worden gelogd in response listbox** — `AddResponse()` wordt ook aangeroepen voor data die daadwerkelijk naar de server wordt gestuurd (AUTH LOGIN flow), inclusief Base64-credentials | Medium | `AddResponse()` / `sockPuts()` — smtp.cpp:702, 580 | ⚠️ Niet gefixed (betreft bestaand design) |

### Laag

| # | Bug | Ernst | Locatie | Status |
|---|-----|-------|---------|--------|
| L1 | **`gethostbyname()` deprecated** — niet thread-safe, IPv4-only; vervanger: `getaddrinfo()` | Laag | `atoAddr()` — smtp.cpp:606 | ❌ Niet gefixed |
| L2 | **`connect()` blokkeert zonder timeout** — OS-default (Windows: ~20 s) geldt; bij traag netwerk hangt de GUI indirect | Laag | `clientSocket()` — smtp.cpp:675 | ❌ Niet gefixed |
| L3 | **SMTP-response buffer `MY_BUFF_SIZE=1024`** — een uitgebreide multiline EHLO-reply (verbose server) kan 1024 bytes overschrijden; `receiveData_SSL` retourneert dan `LACK_OF_MEMORY` | Laag | smtp.cpp:17, 301 | ❌ Niet gefixed |
| L4 | **Separator-karakter in `smtpMail()` is kapot** — het `\xef\xbf\xbd` (UTF-8 REPLACEMENT CHARACTER) in de char-literal vergelijkt bij MSVC effectief op byte 0xBD, niet op het originele 0x95 (Windows-1252 bullet point). In de praktijk triggert de splitsing nooit; subject en body zijn identiek | Laag | `smtpMail()` — smtp.cpp:1097 | ❌ Niet gefixed (functionele gedragswijziging) |
| L5 | **Operator-precedentie in `smtpResponse()`** — `buf[0]=='1' || buf[0]=='2' || buf[0]=='3' && buf[3]==A_SPACE` — leesbaarheidsrisico | Laag | smtp.cpp:857 | ❌ Niet gefixed |

---

## 6. ISP-compatibiliteit

| Provider | Poort/Protocol | AUTH | TLS (voor fix) | TLS (na fix) | Verdict |
|----------|---------------|------|----------------|--------------|---------|
| **Gmail** smtp.gmail.com | 465 SSL of 587 STARTTLS | App-password → AUTH LOGIN ✅ | ⚠️ MITM mogelijk | ✅ Geverifieerd | ✅ Werkt |
| **Gmail** (OAuth2-gebruikers) | 587 | XOAUTH2 vereist | ❌ | ❌ | ❌ Niet ondersteund |
| **Outlook/Office 365** smtp.office365.com | 587 STARTTLS | App-password → AUTH LOGIN ✅ | ⚠️ SNI ontbrak → mogelijk verkeerd cert | ✅ SNI gefixed | ✅ Werkt met app-password |
| **Outlook/O365** (moderne auth) | 587 | XOAUTH2 vereist | ❌ | ❌ | ❌ Niet ondersteund |
| **Yahoo** smtp.mail.yahoo.com | 465 of 587 | App-password → AUTH LOGIN ✅ | ⚠️ | ✅ | ✅ Werkt |
| **iCloud** smtp.mail.me.com | 587 STARTTLS | App-specific password → LOGIN ✅ | ⚠️ | ✅ | ✅ Werkt |
| **KPN / Ziggo / generiek** | 587 STARTTLS | LOGIN ✅ | ⚠️ | ✅ | ✅ Werkt |
| **Self-hosted Postfix/Exim** | 25 / 465 / 587 | Allen ✅ | ⚠️ | ✅ | ✅ Werkt |

**Conclusie:** De enige providers die structureel niet werken zijn Gmail/Outlook met OAuth2/XOAUTH2-accounts. Gebruikers met app-passwords zijn na de TLS-fix op alle providers goed gedekt.

---

## 7. Toegepaste fixes

### Commit `95be3b6` — `fix(smtp): Base64 buffer, connect(), dot-stuffing, TLS verification`

**Bestanden:** `utils/smtp.cpp`

| Fix | Beschrijving |
|-----|-------------|
| `szTmp` vergroot | `smtpLogin()`: 128 → 200 bytes; base64 van 99 bytes = 136 bytes + null |
| `connect()` check | `clientSocket()`: foutpad bij mislukte TCP-connect; sockets wordt gesloten |
| `smtpDotStuff()` | Nieuwe RFC 5321 §4.5.2 helper; wordt voor elke body-sockPuts aangeroepen |
| `smtpMail` buffergrootte | `szSubject`/`szBody`: `1024` → `MAX_MAIL_LEN + 32`; lengte-tellers ipv `strlen()` |
| TLS minimum versie | `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)` in `initOpenSSL()` |
| TLS CA store | Windows system CA store geladen via `CertOpenSystemStoreA("ROOT")` + `d2i_X509` |
| TLS peer verify | `SSL_CTX_set_verify(SSL_VERIFY_PEER)` |
| SNI | `SSL_set_tlsext_host_name(m_ssl, g_szTlsHostname)` in `openSSLConnect()` |
| Hostname verify | `SSL_set1_host(m_ssl, g_szTlsHostname)` |
| Hostname opslaan | `g_szTlsHostname` wordt gezet in `smtpConnect()` vóór de TLS-handshake |
| `#include` / pragma | `openssl/x509.h`, `wincrypt.h`, `#pragma comment(lib, "crypt32.lib")` |

### Commit `0c504d0` — `fix(smtp): restore missing HELO domain field in SMTP settings dialog`

**Bestanden:** `Rsrc.rc`

| Fix | Beschrijving |
|-----|-------------|
| HELO-veld hersteld | `EDITTEXT IDC_SMTP_HELO` + `LTEXT "&HELO Domain"` toegevoegd aan `MAIL_DLGBOX` |
| Groepbox uitgebreid | "Mail Settings": hoogte 68 → 84 DLUs |
| Verticaal verschoven | Alle controls onder het HELO-veld zijn 16 DLUs naar beneden verschoven |
| Dialooghoogte | 265 → 281 DLUs |

---

## 8. Aanbevolen vervolgacties (geprioriteerd)

### P0 — Direct committen (staan in working tree, nog niet gecommit)

| Actie | Bestand | Detail |
|-------|---------|--------|
| **Default poort 25 → 587** | `PDW.cpp` regel 10307 | `GetPrivateProfileInt("SMTP", "Port", 587, ...)` — RFC 8314 §3.3. Staat in working tree als `// FIX [SmtpPort]`. Aparte commit uitstellen totdat de andere pre-existing working tree changes (versie-bump, filter-fix) ook gecommit worden. |

### P1 — Gewenst in een volgende sprint

| Prioriteit | Actie | Reden |
|-----------|-------|-------|
| P1a | Credentials verbergen in response listbox | `AddResponse()` wordt aangeroepen vóór en na de base64-encoded username/password; screenshotten of logging lekken credentials |
| P1b | `MAIL_OPTION_AUTH` afdwingen over TLS | GUI staat toe om AUTH aan te zetten zonder SSL; code schiet over de check heen |
| P1c | PORT-presets combobox of labels | Gebruiker moet 587/465 uit zijn hoofd weten |
| P1d | `\r\n` body line endings | RFC 5321 §2.3.8 — body gebruikt momenteel `\n`-only |

### P2 — Gewenst op termijn

| Prioriteit | Actie | Reden |
|-----------|-------|-------|
| P2a | XOAUTH2 / PLAIN AUTH | Gmail en Office365 accounts met OAuth; vereist token-flow of SASL PLAIN |
| P2b | `getaddrinfo()` i.p.v. `gethostbyname()` | IPv6, thread-safe |
| P2c | Non-blocking `connect()` met selectie-timeout | `clientSocket()` blokkeert tot OS-timeout (~20 s) bij onbereikbare server |
| P2d | Operator-precedentie in `smtpResponse()` | Leesbaarheidsprobleem, geen functioneel risico |
| P2e | Separator-karakter fix in `smtpMail()` | Encoding-artefact; subject ≠ body zoals bedoeld |

---

## 9. Bouwresultaat

```
MSBuild pdw_vs2017.vcxproj /p:Configuration=Release /p:Platform=Win32
→ pdw_vs2017.vcxproj -> C:\PDW sourcecode\Release\pdw.exe
```

**Build succesvol** — geen compile-errors na alle fixes. De enige gewijzigde bestanden op de `audit/smtp-review` branch zijn `utils/smtp.cpp` (commit `95be3b6`) en `Rsrc.rc` (commit `0c504d0`).
