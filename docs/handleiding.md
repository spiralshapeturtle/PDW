# PDW Gebruikershandleiding

**Windows 7-11** | FLEX / ReFLEX / POCSAG / ACARS / MOBITEX / ERMES decoder

---

## Inhoudsopgave

1. [Inleiding](#1-inleiding)
2. [Juridische kennisgeving](#2-juridische-kennisgeving)
3. [Vereisten](#3-vereisten)
4. [Aan de slag](#4-aan-de-slag)
   - 4.1 [Geluidskaartinvoer](#41-geluidskaartinvoer)
   - 4.2 [Serielepoortinvoer](#42-serielepoortinvoer)
   - 4.3 [Discriminatortap](#43-discriminatortap)
5. [Hoofdvenster](#5-hoofdvenster)
   - 5.1 [Signaalmeter / RX Kwaliteitsbalk](#51-signaalmeter--rx-kwaliteitsbalk)
   - 5.2 [Health-paneel](#52-health-paneel)
   - 5.3 [Berichtkolommen](#53-berichtkolommen)
6. [Bestandsmenu](#6-bestandsmenu)
7. [Monitormenu](#7-monitormenu)
   - 7.1 [Statistiekenvenster](#71-statistiekenvenster)
8. [Interfacemenu](#8-interfacemenu)
   - 8.1 [Geluidskaartinstellingen](#81-geluidskaartinstellingen)
   - 8.2 [Serielepoortinstellingen](#82-serielepoortinstellingen)
9. [Filters](#9-filters)
   - 9.1 [Filtervelden](#91-filtervelden)
   - 9.2 [Filteracties](#92-filteracties)
   - 9.3 [Berichttekst-matching](#93-berichttekst-matching)
   - 9.4 [Zoeken tijdens het typen](#94-zoeken-tijdens-het-typen)
10. [Optiesmenu](#10-optiesmenu)
    - 10.1 [SMTP e-mailmeldingen](#101-smtp-e-mailmeldingen)
    - 10.2 [Webhook](#102-webhook)
    - 10.3 [MQTT](#103-mqtt)
    - 10.3a [Telegram](#103a-telegram)
    - 10.3b [Pushover](#103b-pushover)
    - 10.4 [Telnetserver](#104-telnetserver)
    - 10.5 [MySQL-uitvoer](#105-mysql-uitvoer)
    - 10.6 [SQLite-uitvoer](#106-sqlite-uitvoer)
    - 10.7 [Systeemmeldingen](#107-systeemmeldingen)
    - 10.8 [Logbestanden en schrijfbuffering](#108-logbestanden-en-schrijfbuffering)
    - 10.9 [Programmaopties](#109-programmaopties)
    - 10.10 [Lifecycle-commandohaken (pdw.ini)](#1010-lifecycle-commandohaken-pdwini)
11. [Weergavemenu](#11-weergavemenu)
    - 11.1 [Eigen filterlabelkleuren](#111-eigen-filterlabelkleuren-pdwini)
12. [Gedecodeerde protocollen](#12-gedecodeerde-protocollen)
    - 12.1 [POCSAG](#121-pocsag)
    - 12.2 [FLEX en ReFLEX](#122-flex-en-reflex)
    - 12.3 [ACARS](#123-acars)
    - 12.4 [MOBITEX](#124-mobitex)
    - 12.5 [ERMES](#125-ermes)
13. [Berichtformaat](#13-berichtformaat)
14. [Logbestandsformaat](#14-logbestandsformaat)
15. [Ondersteuningsbestanden](#15-ondersteuningsbestanden)
16. [Windows-meldingen](#16-windows-meldingen)
17. [Hoge-DPI-ondersteuning](#17-hoge-dpi-ondersteuning)
18. [Meerdere instanties / titelbalk](#18-meerdere-instanties--titelbalk)
19. [COM-poorten >= 10](#19-com-poorten--10)
20. [Probleemoplossing](#20-probleemoplossing)
21. [Credits en licentie](#21-credits-en-licentie)

---

## 1. Inleiding

PDW is een software pagingdecoder die een geluidskaart of seriele poort omzet in een volwaardige FLEX / ReFLEX / POCSAG / ACARS / MOBITEX / ERMES-ontvanger. Het decodeert, filtert en distribueert pagingberichten naar een breed scala aan uitvoerkanalen - van eenvoudige weergave op het scherm en e-mailmeldingen tot MQTT-brokers, webhooks, Telnet-clients, MySQL-databases en lokale SQLite-bestanden.

Deze fork bouwt voort op de klassieke PDW 3.2-codebase en voegt vijf jaar productiegeharde verbeteringen toe. Een volledige versiegeschiedenis is beschikbaar in `RELEASE_NOTES.md`.

---

## 2. Juridische kennisgeving

Het ontvangen en decoderen van paginguitzendingen kan in uw land of regio bij wet beperkt of verboden zijn. Het is uw eigen verantwoordelijkheid om te controleren of uw gebruik van deze software voldoet aan alle toepasselijke lokale, nationale en internationale wet- en regelgeving. Deze software wordt aangeboden "zoals ze is", zonder enige garantie, uitdrukkelijk of stilzwijgend. De auteurs en bijdragers aanvaarden geen enkele aansprakelijkheid voor schade, juridische gevolgen of andere verliezen die voortvloeien uit het gebruik of misbruik van deze software. Door deze software te gebruiken aanvaardt u de volledige verantwoordelijkheid voor het waarborgen van het rechtmatige gebruik ervan.

---

## 3. Vereisten

| Item | Minimum |
|------|---------|
| Besturingssysteem | Windows 7, 8, 10 of 11 (32-bit of 64-bit) |
| CPU | Elke moderne CPU; een 1 GHz Pentium-klasse volstaat |
| RAM | 32 MB |
| Schijfruimte | 10 MB voor de applicatie; extra voor logbestanden en de optionele database |
| Geluidskaart | Elke Windows-compatibele geluidskaart met een line-in of microfooningang |
| Radio | Elke scanner of ontvanger die het gewenste frequentieband dekt |
| Runtime | Microsoft Visual C++ Redistributable 2017 of later (x86 voor de Win32-build, x64 voor de 64-bit-build). Download van Microsoft indien nog niet geinstalleerd. |

Een serielepoortinterface (hardware slicer) kan worden gebruikt in plaats van een geluidskaart - zie [sectie 8.2](#82-serielepoortinstellingen). Serielepoortinvoer werkt op alle ondersteunde Windows-versies inclusief Windows 10 en 11.

---

## 4. Aan de slag

### 4.1 Geluidskaartinvoer

1. Maak een kabel om uw radio of scanner te verbinden met de **line-in** (of microfoon) aansluiting van uw geluidskaart. Gebruik een mono stekker aan de radiokant en een stereo stekker aan de geluidskaart kant, waarbij het mono signaal naar zowel het linker- als het rechterkanaal wordt geleid.
2. Start PDW en open **Interface → Instelling**. Selecteer **Soundcard** en kies een configuratie:
   - **Discriminator** - beste kwaliteit; vereist een discriminatortap (zie [4.3](#43-discriminatortap))
   - **Earphone** - gebruik de koptelefoonaansluiting van de radio; geschikt voor de meeste opstellingen
   - **Speaker Out** - tap de luidsprekeruitgang
   - **Tape/Rec Out** - gebruik de tape- of opname-uitgang van de radio
3. Open **Interface → Volume** om de Windows-mixer te openen. Zorg ervoor dat **Line In** of **Mic** is geselecteerd als opnamebron.
4. Zet uw radio aan, stel de squelch in op de laagste stand en stem af op witte ruis op de VHF-band. De signaalmeter (rechtsboven in de werkbalk) zou moeten beginnen te bewegen. Als dit niet het geval is, verhoog dan het radiovolume.
5. Selecteer in het **Monitor**-menu het protocol dat u wilt decoderen.
6. Stem af op een bekende frequentie in uw gebied. Berichten zouden binnen een minuut moeten verschijnen. Als dit niet het geval is, probeer dan het volume aan te passen of een andere configuratie uit stap 2.

**Aangepaste drempel- / centrerings- / hersynchronisatie-instellingen**

Als u een baudrate wel kunt decoderen maar een andere niet (bijv. POCSAG 1200 werkt maar 2400 niet):

1. Selecteer **Custom** in het dialoogvenster Interface-instelling en klik op **Set Custom**.
2. Probeer de **Threshold** voor de probleembaudrate in te stellen op 1. Klik op OK en test.
3. Als dit niet helpt, probeer dan waarden 2 tot en met 9.
4. Herhaal voor **Centering** en **Resync** als drempelwijzigingen alleen niet voldoende zijn.

### 4.2 Serielepoortinvoer

Een hardware slicer (serielepoortinterface) zet het discriminatoraudiosignaal om naar een twee- of vier-niveau digitaal signaal en voert dit in via de COM-poort van de pc. Dit geeft de best mogelijke signaalkwaliteit en werkt op alle ondersteunde Windows-versies inclusief Windows 10 en 11.

1. Bouw of verkrijg een serielepoort slicer voor 2-niveau (POCSAG / ACARS / ERMES) of 4-niveau (FLEX / MOBITEX) FSK.
2. Sluit de slicer aan op een vrije COM-poort.
3. Open **Interface → Instelling**, selecteer **Serial** en kies het juiste COM-poortnummer. COM-poorten 10 en hoger worden ondersteund - zie [sectie 19](#19-com-poorten--10).
4. Verbind de audio-ingang van de slicer met de discriminatoruitgang of koptelefoonaansluiting van uw radio.
5. Selecteer het protocol in het **Monitor**-menu en stem af op de juiste frequentie.

### 4.3 Discriminatortap

Als geluidskaartinvoer bij elke configuratie-instelling slechte resultaten geeft, is een discriminatortap de oplossing. Deze modificatie neemt het signaal rechtstreeks van de FM-discriminator IC van uw scanner, voor het de-emphasis filter, wat een vlakke frequentierespons geeft die ideaal is voor dataontvangst.

De tap bestaat uit een **0,1 uF** condensator die gesoldeerd wordt aan de discriminatoruitgangspin. Een afgeschermde draad loopt van de condensator naar de line-in van de geluidskaart of de serielepoortinterface. Deze tap werkt altijd als hij correct is uitgevoerd.

> Discriminatortap-modificatie opent de radio en kan de garantie ongeldig maken. Als u niet comfortabel bent met solderen, gebruik dan de koptelefoonaansluiting.

---

## 5. Hoofdvenster

Het hoofdvenster toont een scrollende lijst van gedecodeerde berichten. Elke rij is een pagingbericht.

### 5.1 Signaalmeter / RX Kwaliteitsbalk

De balk rechts in de werkbalk toont de huidige ontvangstqualiteit als percentage (0-100 %). Een hogere waarde betekent een schoner signaal.

### 5.2 Health-paneel

Rechts op de werkbalkband kan een compacte statusstrip staan. Dit is een echte omschakeling met de klassieke hoek: zolang het health-paneel zichtbaar is **vervangt** het de signaalmeter en het RX-Q-percentagevak, zodat niets dubbel wordt getoond; verberg het paneel (rechtsklikmenu) en de klassieke naald + RX-Q-hoek komen exact terug zoals voorheen. Van links naar rechts:

- **Health-score** — de actieve ontvangstqualiteitsscore (0-100 %) in groen (>= 96 %), oranje of rood (onder de drempel van de e-mailmelding). `--%` betekent nog geen meting.
- **Trendgrafiekje** — de score over de laatste 1, 5, 15 of 60 minuten, of over de laatste 4 of 8 uur om de afgelopen nacht te overzien, getekend in statuskleuren: een gezonde periode is een dikke groene lijn, verslechtering toont oranje en tijd onder de meldingsdrempel toont rood. Een dun gestippeld lijntje markeert de meldingsdrempel zelf, zodat je ziet hoeveel marge de score nog heeft voordat de melding zou afgaan. De grafiek vult van rechts (nieuwste seconde rechts), dus kort na de start staat er alleen rechts al een lijn. Bij de lange (4/8-uurs) vensters beslaat één pixel meerdere minuten; naast de gemiddelde trendlijn tekent het grafiekje dan ook een dikke gekleurde band naar de *slechtste* waarde per pixel, zodat een korte ontvangstdip toch als een neerwaartse piek zichtbaar blijft in plaats van weggemiddeld te worden. Beweeg de muis over het grafiekje voor de laagste waarde in het venster en het tijdstip ervan, bijv. `lowest 40% at 03:15`.
- **COM-stip** — alleen als seriële invoer aanstaat: groen = poort open en data komt binnen, oranje ring = open maar geen data, rood = poort niet open.
- **Feed-stippen** — een stip + tag per **ingeschakelde** uitvoerfeed (`SM` SMTP, `WH` webhook, `TG` Telegram, `PO` Pushover, `MQ` MQTT, `MY` MySQL, `SQ` SQLite, `TS` telnetserver). Groen = geen bekend probleem — dat geldt ook voor "ingeschakeld maar nog niets afgeleverd", dus de per-bericht-feeds (webhook, Telegram, Pushover, SMTP) zijn vanaf de start groen, ook als hun eerste push dagen op zich laat wachten. Oranje = bezig met opnieuw proberen na een tijdelijk probleem, rood = laatste aflevering of verbinding mislukt. Voor kleurenblind-leesbaarheid verschillen de statussen ook in vorm: opnieuw proberen tekent als een holle **ring**, terwijl gezond en mislukt beide een gewone **gevulde schijf** zijn (groen en rood). Uitgeschakelde feeds worden niet getoond. De verbindingsgerichte feeds (MQTT, MySQL, telnetserver) volgen de verbinding live: rood zolang de broker/server onbereikbaar is, weer groen zodra de verbinding hersteld is.

De feed-stip toont de laatste **uitkomst**, vastgehouden: een verzending die alleen maar bezig is verandert de stip nooit (een kapotte feed knippert dus niet groen aan het begin van elke poging), en een retry verlaagt een rode stip nooit terug naar oranje — alleen een echte geslaagde aflevering of verbinding maakt de stip weer groen.

**Beweeg de muis** over een onderdeel voor uitleg: de score noemt zijn actieve bron, het grafiekje zijn venster en meldingsniveau, de COM-stip de linkstatus, en elke feed-stip zijn volledige status inclusief het **laatste probleem** en het tijdstip van de statuswissel, bijv. `Telegram: FAILED since 14:02 - API rejected message (HTTP 401)`. Beweeg de muis over het algemene statusgebied voor een tweede regel die toont hoe lang PDW al draait, bijv. `Up 3d 04:12 - since 15-08 09:03` - dezelfde uptime als in het F12-Debug Information-venster en het systeemvak-tooltip.

Elke status**wissel** — feed-stippen en COM-link — wordt bovendien weggeschreven naar een dagelijks logbestand `{datum}_health.log` in de ingestelde logmap (één regel per overgang, bijv. `TG (Telegram): OK -> FAILED - API rejected message (HTTP 401)`). Stabiele statussen schrijven niets, dus het bestand blijft klein; gebruik het om een probleem te onderzoeken dat zichzelf oploste terwijl je weg was. De allereerste succesvolle poging van een feed na opstarten (`idle -> OK`) is geen probleem en wordt niet gelogd; elke overgang die wél een echt probleem betreft (naar retrying/failed, en het herstel terug naar OK) blijft gewoon loggen.

**Rechtsklik op het paneel** voor het menu:

| Item | Beschrijving |
|------|--------------|
| Health source: RX needle (classic) | Dezelfde score als het RX-Q-vak rechtsboven (standaard) |
| Health source: Penalty system | Strengere score die direct daalt bij fouten en langzaam herstelt |
| Trend window | Venster van het trendgrafiekje: 1 / 5 / 15 / 60 minuten, of 4 / 8 uur |
| Hide/Show health panel | Paneel verbergen of tonen (rechtsklik op dezelfde plek om het terug te halen) |
| Show RX needle alongside | Houd het paneel, maar breng ook de klassieke RX-signaalsterkte-naald terug in zijn oude vak helemaal rechts |

De gekozen health-bron voedt ook de **RX Kwaliteitsmelding** per e-mail (zie paragraaf 10.7): wissel je de bron, dan wisselt ook waar de melding op reageert. Met de Penalty-system-bron zakt de score naar 0 % als er ongeveer twee minuten niets decodeerbaars is ontvangen (dode ether), zodat een ontvanger die stilvalt rood toont in plaats van zijn laatste gezonde waarde vast te houden; op netwerken met legitieme pauzes van meerdere minuten tussen uitzendingen dipt deze bron dus tussen de uitzendingen door, en is de klassieke naald-bron daar de betere keuze. De optie **Show RX needle alongside** is een derde indeling tussen "alleen paneel" en "alleen klassieke hoek": het health-paneel blijft staan, maar de klassieke RX-signaalsterkte-naald wordt er weer bij getekend in zijn oude vak helemaal rechts. Het paneel krimpt net genoeg om plaats te maken, zodat het RX-health-percentage op zijn plek blijft en de naald ernaast komt te staan; het oude RX-Q-percentagevak en waarschuwingsvierkantje blijven verborgen, want de score van het paneel vervangt ze al. Inschakelen terwijl het paneel verborgen is, toont het paneel er ook meteen bij. Bij een smal venster laat het paneel eerst de labels vallen, dan het grafiekje; past zelfs dat niet, dan verschijnt de klassieke hoek tot er weer ruimte is. Instellingen staan in `pdw.ini` onder `[HealthPanel]`.

### 5.3 Berichtkolommen

| Kolom | Inhoud |
|-------|--------|
| Tijd | Decodetijd (UU:MM:SS) |
| Datum | Decodeerdatum (DD-MM-JJ) |
| Modus | Protocol en bitsnelheid, bijv. `FLEX-1600`, `POCSAG-1200` |
| Type | `ALPHA` / `NUMERIC` / `TONE` / `GROUP` |
| Bitrate | Numerieke bitsnelheid |
| Capcode | Pagerondres (capcodes kunnen voorloopnullen hebben) |
| Label | Filterlabel toegewezen aan deze capcode |
| Bericht | Gedecodeerde tekst |

Klik op een kolomkop om op die kolom te sorteren. Dubbelklik op een berichtrij om de detailweergave te openen.

---

## 6. Bestandsmenu

| Item | Beschrijving |
|------|--------------|
| Logbestand openen/sluiten | Start of stop het schrijven van gedecodeerde berichten naar een `.log`-bestand; opent ook schrijfbuffering en ISO-tijdstempelopties |
| Filterlog openen/sluiten | Start of stop het schrijven van filtergematche berichten naar een apart `.flt`-bestand |
| Filters | Open de filtereditor (ook Ctrl+F) |
| Afdrukken | Druk de huidige berichtenlijst af |
| Afsluiten | Sluit PDW |

Logbestanden krijgen een datumstempel (`JJMMDD_*.log`) en roteren automatisch om middernacht. Zie [sectie 10.8](#108-logbestanden-en-schrijfbuffering) voor schrijfbuffering en de ISO-tijdstempeloptie.

---

## 7. Monitormenu

Selecteer welk protocol PDW moet decoderen. Slechts een modus is tegelijk actief.

| Menuitem | Beschrijving |
|----------|--------------|
| **POCSAG/FLEX** | Decodeer POCSAG (512 / 1200 / 2400 baud) en FLEX / ReFLEX (1600 / 3200 / 6400 baud) gelijktijdig |
| **ACARS** | Aircraft Communications Addressing and Reporting System (2400 baud) |
| **MOBITEX** | Mobiel pakketdatanetwerk protocol |
| **ERMES** | European Radio Messaging System |
| **Statistieken...** (Alt+S) | Open het statistiekenvenster (zie [7.1](#71-statistiekenvenster)) |

### 7.1 Statistiekenvenster

Open via **Monitor → Statistieken** of druk op **Alt+S**.

Toont uurlijkse en dagelijkse berichttellingen per protocol en type (Alpha / Numeriek). Statistieken worden bijgehouden voor alle negen protocol/snelheidscombinaties: FLEX 6400 / 3200 / 1600, POCSAG 2400 / 1200 / 512, ACARS 2400, MOBITEX en ERMES. Statistieken kunnen worden opgeslagen in een `.st`-bestand.

---

## 8. Interfacemenu

### 8.1 Geluidskaartinstellingen

Zie [sectie 4.1](#41-geluidskaartinvoer) voor de eerste instelling. De optie **Custom** in dit dialoogvenster laat u de drempel, centrering en hersynchronisatie per baudrate fijn afstellen. Na wijzigingen klikt u op **OK** om toe te passen zonder PDW opnieuw te starten.

**Auto Invert** - indien ingeschakeld, detecteert en corrigeert PDW automatisch omgekeerde FSK-polariteit. Voor de meeste opstellingen werkt dit correct; als het decoderen grillig is, probeer het dan handmatig in te stellen.

### 8.2 Serielepoortinstellingen

Selecteer het COM-poortnummer dat overeenkomt met uw hardware slicer. COM-poortnummers 10 en hoger kunnen direct in het veld worden ingevoerd. De poort wordt onmiddellijk geopend na het klikken op OK.

---

## 9. Filters

Filters laten u labels, kleuren, meldingen en afzonderlijke logbestanden toewijzen aan specifieke capcodes, labelpatronen of berichttekstpatronen. Elk inkomend bericht wordt gecontroleerd op alle actieve filters in volgorde. Er is geen harde limiet op het aantal filters.

Open de filtereditor via **Bestand → Filters** of druk op **Ctrl+F**.

### 9.1 Filtervelden

| Veld | Beschrijving |
|------|--------------|
| Capcode | Exact capcodenummer, of een voor-/wildcardmatch |
| Label | Beschrijvende naam weergegeven in de labelkolom |
| Kleur | Achtergrondkleur voor gematchte berichten |
| Type | ALPHA / NUMERIC / TONE / Elk |

### 9.2 Filteracties

Elk filter kan onafhankelijk een willekeurige combinatie van het volgende activeren:

| Actie | Beschrijving |
|-------|--------------|
| **Geluid** | Speel een WAV-bestand af |
| **E-mail** | Verstuur een SMTP-melding (gebruikt instellingen van Opties → SMTP) |
| **Opdracht** | Voer een extern programma of script uit |
| **Log 1 / 2 / 3** | Schrijf gematchte berichten naar maximaal drie afzonderlijke logbestanden |
| **Monitor only** | Toon op het scherm maar sluit uit van hoofdlog en uitvoerfeeds |
| **Negeer in groepsoproep** | Verberg deze capcode uit de schermweergave van de groepsoproep, maar log en verstuur het volledige bericht wel |
| **Reject** | Onderdruk dit bericht volledig - niet tonen of loggen |

**Negeer in groepsoproep** is bedoeld om schermruis te verminderen bij routinematige abonnee-capcodes in een FLEX-groepsoproep - denk aan wegblokkering- of station-techniekadressen - zodat de echte personeelsalarmadressen opvallen. Wanneer een capcode die bij een dergelijk filter hoort als abonnee in een groepsoproep verschijnt:

- wordt de capcoderegel weggelaten uit de schermweergave van de groepsoproep (zowel het monitor- als het filterpaneel);
- sleept de capcode zijn groep niet meer op zichzelf mee naar het **filtervenster** - een groepsoproep waarvan het enige overeenkomende lid een genegeerde capcode is, verstoort het onderste (filter)paneel niet. Een groep die ook een echte (niet-genegeerde) overeenkomende capcode heeft, verschijnt er nog steeds via die capcode;
- activeert de capcode de filterbep niet;
- wordt de capcode uit de **Windows-toast/tray-melding** voor die groepsoproep gehouden: de genegeerde capcode/label wordt niet meer in de meldingstitel opgenomen, wat overeenkomt met de schermweergave. (Een groep die ook een echte overeenkomende capcode heeft, geeft nog steeds een melding voor die capcode.)

Het groepsberichtopschrift en de tekst blijven zichtbaar in het monitorpaneel, en alle niet-genegeerde abonnees worden normaal weergegeven. Er gaat niets verloren op schijf: het **volledige** groepsbericht, inclusief de genegeerde capcode, wordt nog steeds naar het monitorlogbestand geschreven en naar elke uitvoerfeed gestuurd (e-mail, Telegram, Pushover, webhook, MQTT, database) precies zoals voorheen. (Het genegeerde lid wordt wel uit het *filter*logbestand gehouden, zodat dit overeenkomt met het filtervenster.) De optie is alleen van toepassing op niet-reject capcode-filters en alleen binnen FLEX-groepsoproepen.

In de **databasefeeds** (MySQL en SQLite) krijgt een genegeerd groepsoproepslid een toegewezen per-abonnee `match_type`-waarde van **3** in de `subscribers` JSON van de groepsrij (1 = gefilterd, 2 = monitor-only, 3 = genegeerd-in-groep, 0 = gewoon ongefilterd lid). Het lid wordt niet weggehaald - de verschillende waarde laat eenvoudig een metgezelwebsite deze "monitorCodes" op aanvraag tonen, verbergen of hervormen, apart van echte matches en van de andere ongefilterde leden van dezelfde groep. De eigen `match_type`-kolom van de groepsrij blijft 0/1/2 (een genegeerd lid telt als 0 voor het rijaggregaat), dus bestaande queries en statistieken op rijniveau worden niet beinvloed.

De vlag heeft **geen effect buiten een groepsoproep**: wanneer dezelfde capcode individueel wordt gepagineerd (een normaal alfa/numeriek bericht zonder groepsadres), of in de klassieke (niet-FlexGroupMode) uitgebreide weergave, wordt hij volledig normaal weergegeven, gefilterd, gelogd en doorgestuurd. "Negeer in groepsoproep" verbergt een capcode alleen terwijl hij *abonnee is in* een FLEX-groepsoproep.

**Negeer in groepsoproep** en **Monitor only** sluiten elkaar wederzijds uit: ze vragen om tegengesteld gedrag (monitor-only *toont* een bericht op het scherm en *onderdrukt* de feeds; negeer *verbergt* het op het scherm en *behoudt* de feeds). Het aanvinken van een van de twee wist automatisch de andere, zodat een filter nooit in beide modi tegelijk kan staan.

**Filter overzichtskolommen.** Met *Extra informatie tonen* ingeschakeld (Filters-dialoog), toont elke rij in het Ctrl+F overzicht een compacte set per-filtermarkeringen zodat u een grote filterset in één oogopslag kunt scannen. Elke markering is **HOOFDLETTERS wanneer aan, kleine letters wanneer uit**: `CMD`/`cmd` (commandobestand), `LAB`/`lab` (label tonen), `SEP`/`sep` (afzonderlijk filterbestand), `TG`/`tg` (Telegram verzenden), `PO`/`po` (Pushover verzenden), de geluidsnaam, en `IGN-GRP` wanneer *Negeer in groepsoproep* is ingesteld. De `TG`/`PO`-vlaggen weerspiegelen de per-filter *Telegram verzenden* / *Pushover verzenden*-selectievakjes, die deze feeds aansturen in de modus "Alleen geselecteerde filters" - handig voor het zien welke van duizenden capcodes zijn gekoppeld aan elke meldingsservice.

Een reject-filter kan worden beperkt tot een specifiek bericht door de capcode te combineren met een **Tekst**-waarde: het bericht wordt alleen verworpen wanneer **zowel** de capcode **als** de tekst matcht. Dit laat u bijvoorbeeld alleen de berichten van een capcode verwerpen die een bepaald woord bevatten, terwijl alle andere berichten van diezelfde capcode normaal worden doorgelaten.

Wanneer **Reject** is aangevinkt, worden de actie-instellingen die geen effect hebben op een verworpen bericht - Geluid, E-mail, Telegram, Pushover, Monitor only, Commando en aparte filterbestanden - grijs weergegeven, omdat een verworpen bericht wordt onderdrukt voordat een van die acties draait. Het **Label**-veld blijft wel beschikbaar: een reject-filter wordt nooit op het scherm getoond, maar als u er een label aan geeft, wordt dat naast het bericht geschreven in de rejected-regel van het logbestand (wanneer "Verworpen berichten ook loggen" aan staat). Dat maakt dat logrecord beter leesbaar.

Een verworpen bericht wordt normaal uit elke uitvoer gehouden, inclusief het logbestand op schijf. De globale optie **"Verworpen berichten ook loggen"** in het Logbestand-dialoogvenster (zie sectie 10.8) overschrijft alleen het log-gedeelte: indien ingeschakeld worden verworpen berichten alsnog naar het monitorlogbestand geschreven, terwijl ze onderdrukt blijven op het scherm, in het filterlog en in alle feeds.

Filterlabels en filtertekstpatronen kunnen elk maximaal **256 tekens** lang zijn.

### 9.3 Berichttekst-matching

Wanneer een filter een **Tekst**-waarde heeft, moet het bericht die tekst bevatten opdat het filter overeenkomt. Matching is niet hoofdlettergevoelig. Het tekstbereik accepteert tot **256 tekens** en begrijpt de volgende operatoren:

| Operator | Betekenis | Voorbeeld | Overeenkomst wanneer |
|----------|-----------|-----------|-----------|
| (gewoon)  | Substring-match | `alpha` | het bericht bevat `alpha` ergens |
| `&`      | AND - alle delen moeten aanwezig zijn, in volgorde | `alpha&bravo` | het bericht bevat `alpha` **en**, daarna, `bravo` |
| `\|`      | OR - een van de termen mag overeenkomen (laagste prioriteit) | `alpha&bravo\|alpha&charlie` | het bericht komt overeen met `alpha` EN `bravo`, **of** met `alpha` EN `charlie` |
| `^`      | Anker - bericht moet *beginnen* met de tekst | `^ALARM` | het bericht begint met `ALARM` |
| `=`      | Geheel woord - de term overeenkomt alleen als een compleet woord, niet in een langer woord | `alpha&=cat` | `cat` overeenkomt alleen als zelfstandig woord, **niet** in `category` |

`&` bindt strakker dan `|`, dus `A&B|C&D` leest als `(A EN B) OF (C EN D)`, net als normale rekenregels. Een OR-lijst zoals `alpha|bravo|charlie` overeenkomt als **een** van de drie termen verschijnt. Lege termen worden genegeerd, dus `|alpha` en `alpha|` beide gedragen als gewoon `alpha`.

Plaats `=` direct voor een enkel woord of term om een **geheel-woord**-overeenkomst te vereisen. Een woordgrens is elk niet-alfanumeriek teken (spatie, leesteken, koppelteken) of het begin/einde van het bericht. Dit is de manier om te voorkomen dat korte codes ook binnen langere woorden overeenkomen: `=cat` komt overeen met `cat` en `the cat-1` maar niet met `category`. De `=` geldt per term, dus u kunt het vrij mixen - bijv. `alpha&=cat` houdt `alpha` als normale substring terwijl u `cat` als geheel woord vereist. Geheel-woord-matching blijft niet hoofdlettergevoelig.

**Werkende voorbeelden** (alle operatoren samen):

| Filter-tekst | Overeenkomst |
|-------------|-----------|
| `alpha` | elk bericht met `alpha` |
| `alpha&bravo` | berichten met zowel `alpha` als `bravo` |
| `alpha\|bravo` | berichten met `alpha` **of** `bravo` |
| `alpha&bravo\|alpha&charlie` | `(alpha EN bravo)` **of** `(alpha EN charlie)` |
| `^alpha` | berichten die **beginnen met** `alpha` |
| `=cat` | het gehele woord `cat` alleen - niet `category` of `vacate` |
| `alpha&=cat` | `alpha` ergens **en** `cat` als geheel woord |

Het **Exacte tekst vergelijken** selectievakje vergelijkt het volledige bericht met de filtertekst in plaats van een substring-zoeking te doen. Omdat exact-geheel-bericht-matching incompatibel is met de `&`, `|` en `^` operatoren, wordt het selectievakje automatisch grijs weergegeven (en gewist) zodra de filtertekst `&` of `|` bevat. Het wordt weer beschikbaar wanneer de tekst die operators niet meer gebruikt.

> Opmerking: `^` verankering is niet gecombineerd met `|`. Wanneer filtertekst `|` bevat, wordt een toonaangevende `^` op een term genegeerd en wordt de term als gewoon substring behandeld.

### 9.4 Zoeken tijdens het typen

In de filtereditor begint u te typen in het zoekvak om de weergegeven vermeldingen in realtime te filteren. Dit is nuttig als u honderden filters heeft.

---

## 10. Optiesmenu

### 10.1 SMTP e-mailmeldingen

Configureer via **Opties → SMTP-instellingen**.

PDW bevat een volledig op zichzelf staande SMTP-client (geen externe bibliotheek).

| Instelling | Beschrijving |
|------------|--------------|
| Server | Hostnaam of IP van uw SMTP-server |
| Poort | 465 voor impliciete TLS; 587 of 25 voor STARTTLS |
| Gebruikersnaam / Wachtwoord | LOGIN of PLAIN authenticatie |
| Van / Naar | Afzender- en ontvangeradressen; meerdere ontvangers gescheiden door puntkomma's |
| Onderwerp / Belichaamsvelden | Kies welke berichtvelden in de onderwerpregel verschijnen en welke in de berichttekst |

**Versleuteling is automatisch:** poort 465 gebruikt impliciete TLS (SSL vanaf de eerste byte); poorten 587 en 25 gebruiken STARTTLS met een verplichte tweede EHLO via het versleutelde kanaal. Er is geen aparte instelling nodig.

**Gesplitste Onderwerp / Belichaams-modus** laat u verschillende velden onafhankelijk kiezen voor de onderwerpregel en de berichttekst. Dit is nuttig voor push-notificatiediensten (zoals pushover.net) waar het onderwerp de notificatietitel wordt en de belichaming de detailtekst.

SMTP-activiteit wordt gelogd naar `JJMMDD_mail.log`. Klik op **Test** in het dialoogvenster om een testbericht te verzenden.

### 10.2 Webhook

Configureer via **Opties → Webhook**.

Verstuurt een HTTP of HTTPS POST naar een willekeurige URL bij elk gedecodeerd bericht (of alleen bij gefilterde berichten).

| Instelling | Beschrijving |
|------------|--------------|
| URL | Doelendpunt (http:// of https://) |
| JSON-formaat | PDW-eigen (genest) of Plat / Node-RED formaat |
| Verzendfilter | Alle berichten / Alleen gefilterd / Gefilterd + Monitor / Ruwe feed |
| Zelfondertekend vertrouwen | Sla certificaatverificatie over voor HTTPS met zelfondertekende certificaten |
| Capcode opvullen | Vul capcode links aan tot 9 cijfers met nullen |

Drie bezorgpogingen met exponentieel uitstel (1 s, 2 s, 4 s). Activiteit gelogd naar `JJMMDD_webhook.log`.

**Plat JSON-voorbeeld:**
```json
{
  "message": "Brandalarm geactiveerd",
  "address": "1234567",
  "label": "Brandweer",
  "mode": "FLEX",
  "type": "ALPHA",
  "bitrate": "1600",
  "timestamp": 1748880000
}
```

### 10.3 MQTT

Configureer via **Opties → MQTT**.

Publiceert elke gedecodeerde pagina naar een MQTT-broker. Gebruikt de statisch gelinkte Paho C-bibliotheek - geen externe DLL's vereist.

| Instelling | Beschrijving |
|------------|--------------|
| Broker | Hostnaam of IP |
| Poort | Standaard 1883; gebruik 8883 voor TLS |
| Onderwerp | Basisonderwerp |
| Client-ID | MQTT-clientidentificator |
| Veldenbitmask | Kies welke velden gepubliceerd worden |
| JSON-formaat | PDW-eigen of Plat / Node-RED |
| Verzendfilter | Alle / Gefilterd / Gefilterd + Monitor / Ruwe feed |

**Gepubliceerde velden:**

| Veld | Beschrijving |
|------|--------------|
| `message` | Gedecodeerde tekst |
| `address` | Capcode |
| `label` | Gematcht filterlabel |
| `time` | UU:mm:ss |
| `date` | DD-MM-JJ |
| `timestamp` | Unix-epoch (seconden) |
| `mode` | FLEX / POCSAG / ... |
| `type` | ALPHA / NUMERIC / TONE |
| `bitrate` | 1600 / 3200 / 6400 (FLEX) of 512 / 1200 / 2400 (POCSAG) |
| `subscribers` | JSON-array van `{capcode, label}` voor FLEX-groepsoproepen |

Klik op **Verbinding testen** in het dialoogvenster om de brokerverbinding te verifiëren. Activiteit gelogd naar `JJMMDD_mqtt.log`.

De MQTT-feed houdt één verbinding open naar de broker en houdt deze in leven met keepalive (45 s), in plaats van opnieuw te verbinden per bericht, zodat korte broker-hikjes (bijvoorbeeld een herstart van een Home Assistant add-on of een backup-venster) worden doorstaan zoals elke langlevende MQTT-client dat doet. Als de broker de verbinding verbreekt terwijl PDW inactief is (broker-herstart of -update), herstelt de feed deze automatisch binnen maximaal een minuut, zodat het volgende bericht weer een warme verbinding aantreft; een broker die uit blijft staan wordt hooguit eens per minuut opnieuw geprobeerd, zonder het log vol te loggen. Als een publicatie de verbinding toch verbroken aantreft, wordt opnieuw verbonden en tot vier keer geprobeerd met exponentiële back-off voordat wordt opgegeven. Schakel voor onbeheerd 24/7-gebruik **Loggen naar bestand** in (`MqttLogToFile=1`): een rustige, stille periode toont platte `SENT`-regels, en een normale reconnect toont één `RECONNECT` gevolgd door `SENT`. Als u de feed vanaf een ander systeem op inactiviteit bewaakt, houd die timeout dan ruim boven uw heartbeat-interval (reken op minstens twee gemiste berichten) zodat een enkele vertraagde publicatie nooit een vals alarm veroorzaakt.

### 10.3a Telegram

Configureer via **Telegram** in het menu. Stuurt gedecodeerde pagina's naar Telegram-chats, groepen en supergroepen via de Bot API (WinHTTP - geen externe bibliotheken).

**Instellen:**

1. Maak een bot aan via [@BotFather](https://t.me/BotFather) en kopieer het **bot-token** naar het dialoogvenster.
2. Stuur `/start` naar uw bot (voor een groep: voeg de bot toe aan de groep en stuur een bericht). Een bot kan nooit als eerste een gebruiker berichten, dus deze stap is vereist.
3. Klik op **Ontdekken...** om de chat_id terug te lezen via `getUpdates`, of plak numerieke chat_id's handmatig. 1:1-chats zijn positief, groepen negatief, supergroepen beginnen met `-100`. Scheid meerdere chat_id's met `;`. Ontdekken leest *alle* afzonderlijke chats uit de recente updates van de bot (id, type, naam), dedupliceerd, en voegt elk nieuwe id toe aan het veld in één klik - zodat een 1-op-1-chat ook wordt gevonden als de bot ook in een drukke groep zit. Telegram slaat slechts ~24 uur recente updates op; als een chat niet meer verschijnt, stuur dan een nieuw bericht naar de bot en klik opnieuw op Ontdekken.

| Instelling | Beschrijving |
|------------|--------------|
| Bot-token | Van @BotFather (lokaal opgeslagen, nooit naar het log geschreven) |
| Chat-ID's | Numeriek, `;`-gescheiden |
| Titel | Vetgedrukte eerste regel template (onderwerpemulatie). Leeg = geen titel. Plaatshouders hieronder |
| Belichaming | Berichttekst-template (standaard `{message}`). Plaatshouders hieronder |
| Thread-ID | Optionele `message_thread_id` voor supergroep-onderwerpen (0 = geen). Wordt alleen toegepast op groep/supergroep-doelen; 1-op-1-chats ontvangen nooit een topic, dus ze mislukken niet langer met HTTP 400 wanneer een topic is ingesteld |
| Stil | Bezorg zonder meldingsgeluid |
| Linkvoorvertoning uitschakelen | Onderdruk webpagina-voorvertoningen |
| Lange berichten splitsen | Splits berichten over 4096 tekens (uit = afkappen) |
| Verzendfilter | Alle / Gefilterd / Gefilterd + Monitor / Alleen geselecteerde filters |

Berichten zijn HTML-opgemaakt (`parse_mode=HTML`); als Telegram de opmaak afwijst, verstuurt PDW automatisch opnieuw als platte tekst. Snelheidslimiet (HTTP 429) reacties worden gehonoreerd met uitstel, en supergroepmigraties werken de opgeslagen chat_id automatisch bij. **Test** verstuurt een eenmalig bericht weergegeven via de **huidige Titel/Belichamingsvelden** met voorbeeldwaarden, zodat u de exacte opmaak - vet, regelafbrekingen en labelstapeling - in Telegram ziet voor het opslaan. De standaardindeling is titelloos met Belichaming `<b>{message}</b>\n{label}`.

**Titel- en Belichamingstemplates.** Een bericht is opgebouwd als de **Titel** (een vetgedrukte eerste regel) gevolgd door een lege regel en de **Belichaming**. Beide velden zijn templates die deze plaatshouders accepteren:

`{message}` `{label}` `{capcode}` `{time}` `{date}` `{mode}` `{type}` `{bitrate}`

Elke andere tekst (inclusief HTML zoals `<b>...</b>`) wordt letterlijk gekopieerd. Standaarden zijn Titel `<b>{label}</b>` en Belichaming `{message}`.

Typ `\n` ergens in een template om een regelafbreking te forceren (`\\` voor een letterlijke backslash). De Titel en Belichaming zijn afzonderlijke velden en worden altijd samengevoegd door een **lege** regel. Als u alles in een blok wilt zonder lege regel, laat de Titel leeg en bouw alles in de Belichaming met `\n`.

**Template-kookboek** (groepsoproeplabels zijn al een-per-regel, dus `{label}` stapelt automatisch):

| Doel | Titel | Belichaming | Resultaat |
|------|-------|-------------|-----------|
| **Bericht als kop, labels eronder, geen lege regel (standaard)** | *(leeg)* | `<b>{message}</b>\n{label}` | **vetgedrukte paginatekst** dan elk label op zijn eigen regel |
| Klassiek | `<b>{label}</b>` | `{message}` | vetgedrukt(e) label(s), lege regel, paginatekst |
| Berichtkop + labels (met lege regel) | `<b>{message}</b>` | `{label}` | vetgedrukte paginatekst, lege regel, labels |
| Compacte enkele regelkop | *(leeg)* | `<b>{message}</b>\n{capcode} - {time}` | vetgedrukte tekst, dan `capcodes - UU:MM:SS` |
| Alles gelabeld | `<b>{label}</b>` | `{message}\n\n{capcode} @ {time} {date}` | labelkop, tekst, dan een metadataregel |
| Gewoon, helemaal geen vet | *(leeg)* | `{message}\n{label}` | paginatekst dan labels, niets vet |
| Alleen capcodes (geen tekst) | `<b>{label}</b>` | `{capcode}` | labelkop, dan de capcodelijst |

De aanbevolen indeling voor een drukke groepsoproep is **Titel leeg + Belichaming `<b>{message}</b>\n{label}`**: u krijgt de paginatekst vet op de eerste regel en elke gepagineerde dienst op zijn eigen regel er direct onder, zonder een extra lege regel. Onthoud dat `\n` een backslash (`\`) gebruikt, niet een schuine streep voorwaarts.

Voor een FLEX-groepsoproep breiden `{label}` en `{capcode}` uit tot de volledige lijst van overeenkomende abonnees. Labels worden **een per regel** geplaatst, dus `Belichaming = {label}` geeft elke gepagineerde dienst op zijn eigen regel. De lijst wordt verzameld tot 32 KB (dezelfde capaciteit als de MQTT/webhook-feeds), zodat zelfs een 122-capcode-testmelding past. Om een dergelijke grote groepsoproep **volledig** te bezorgen, zet **Berichten splitsen over 4096 tekens AAN** - PDW verstuurt het dan als meerdere berichten. Met Splitsen **uit** gaan alleen de eerste 4096 tekens eruit en de rest wordt afgekapt met `...`.

**Verzendfilter-modi** zijn exact hetzelfde als de SMTP-modi:

- *Alle berichten* - elke gedecodeerde pagina.
- *Alleen gefilterde berichten* - pagina's die overeenkomen met een filter (niet monitor-only).
- *Gefilterd + Monitor* - gematchte pagina's inclusief monitor-only matches.
- *Alleen geselecteerde filters* - alleen filters waarvan het selectievakje *Telegram verzenden* is aangevinkt.

**Per-capcode-controle:** in het filtervenster (**Ctrl-F**) heeft elk filter een selectievakje *Telegram verzenden*. Dit selectievakje wordt **alleen** gebruikt in de modus *Alleen geselecteerde filters*, zodat u slechts een handvol capcodes/riccodes kunt doorsturen; in de andere modi wordt het genegeerd. **FLEX-groepsoproepen** worden bezorgd als een enkel bericht met alle overeenkomende abonnee-capcodes/labels, niet als een bericht per lid. Activiteit gelogd naar `JJMMDD_telegram.log`.

**Per-filter stille overschrijving:** het Ctrl+F filtereditor-dialoog heeft een *Telegram stil* selectievakje dat de globale *Stille bezorging*-instelling voor een specifiek filter overschrijft. Indien aangevinkt, bezorgt Telegram meldingen voor die capcode stil (geen buzz/ring) zelfs als globaal stil uit staat. Indien niet aangevinkt, neemt het de globale instelling over. Niet beschikbaar voor reject-regels. In multi-edit-modus is het selectievakje tri-state: een onbepaalde staat betekent "huidige waarde niet wijzigen" over de geselecteerde filters. **Binnen FLEX-groepsoproepen** telt de vlag nog steeds: de enkele samengevoegde groepsmelding wordt stil verzonden alleen wanneer *alle* overeenkomende abonnees in die groep stil verzoeken, dus één normale (niet-stille) leden houdt de hele groep hoorbaar.

**Per-filter routing (topic en chat overschrijving):** het Ctrl+F filtereditor-dialoog heeft twee aanvullende per-filter Telegram-velden die een specifieke capcode naar een ander doel dan de globale configuratie laten bezorgen:

- *TG topic* — een numerieke forumtopic / thread-id (`message_thread_id`). Laat blanco (of 0) om naar de standaardthread van de chat te plaatsen. Stel het in om deze capcode's berichten naar een specifiek topic in een forum-ingeschakelde groep te routeren.
- *TG chat* — een chat-id overschrijving. Laat blanco om de globale chat-id(s) uit het Telegram-dialoog te gebruiken. Anders een of meer doelen invoeren, bijv. een numerieke id (`-1001234567890`), een publieke kanaalnaam (`@mychannel`), of meerdere gescheiden door `;` (`-1001234567890;@mychannel`). Indien ingesteld, wordt deze capcode naar het overschreven doel(en) verzonden in plaats van naar de globale lijst. (U kunt ook scheiden met komma of spatie; PDW slaat komma's op als `;` zodat de `filters.ini`-regel intact blijft.)

Beide velden zijn alleen beschikbaar wanneer Telegram is ingeschakeld en de regel geen reject-regel is. In multi-edit-modus tonen ze `(huidige waarde niet wijzigen)` om huidige waarden ongewijzigd te laten. **Binnen FLEX-groepsoproepen** wordt de routing samengevoegd op de enkele groepsmelding: het eerste niet-nul topic en de eerste niet-lege chat overschrijving tussen de overeenkomende abonnees winnen.

> **Voorzichtigheid:** een topic-id hoort bij een *specifieke* chat. Indien u een chat overschrijving met een topic-id combineert, moet dat topic werkelijk in de overschreven chat bestaan - anders mislukt die verzending. Een mislukte overschrijvingsverzending wordt gelogd naar `JJMMDD_telegram.log` en blokkeert nooit de rest van de Telegram-stroom (andere berichten gaan nog naar de globale chat). Het bot-token wordt nooit naar het log geschreven.

### 10.3b Pushover

Configureer via **Pushover** in het menu. Stuurt gedecodeerde pagina's naar [Pushover](https://pushover.net) via de Berichten-API (WinHTTP - geen externe bibliotheken).

**Instellen:** maak een applicatie aan op het Pushover-dashboard om een **API-token** te verkrijgen, en kopieer uw **gebruikerssleutel** (of een bezorggroepsleutel) van uw accountpagina. Voer beide in het dialoogvenster in en druk op **Test**.

| Instelling | Beschrijving |
|------------|--------------|
| App-token | Applicatie API-token (lokaal opgeslagen, nooit naar het log geschreven) |
| Gebruikers-/Groepssleutel | Uw gebruikerssleutel of een bezorggroepsleutel |
| Titel | Titeltemplate. Leeg = geen titel. Plaatshouders hieronder |
| Belichaming | Berichttekst-template (standaard `{message}`). Plaatshouders hieronder |
| Prioriteit | -2 Laagst / -1 Laag / 0 Normaal / 1 Hoog |
| Geluid | Pushover-geluidsnaam (leeg = gebruikerstandaard) |
| Apparaat | Optionele naam doelapparaat (leeg = alle apparaten) |
| HTML-opmaak | Stuur `html=1` zodat beperkte HTML in het bericht wordt weergegeven |
| Verzendfilter | Alle / Gefilterd / Gefilterd + Monitor / Alleen geselecteerde filters |

Berichten zijn beperkt tot 1024 tekens en titels tot 250 (Pushover-limieten); langere inhoud wordt afgekapt. Snelheidslimiet (HTTP 429) reacties worden opnieuw geprobeerd met uitstel. Noodprioriteit 2 (bevestiging en ontvangstpeiling) wordt in deze versie bewust niet aangeboden.

**Titel- en Belichamingstemplates** werken precies zoals Telegram (zie het kookboek hierboven): beide accepteren `{message} {label} {capcode} {time} {date} {mode} {type} {bitrate}`, elke andere tekst wordt letterlijk gekopieerd, en `\n` forceert een regelafbreking. De standaard is titelloos met Belichaming `<b>{message}</b>\n{label}` en **HTML-opmaak aan**, overeenkomend met Telegram. Enkele voorbeelden:

| Doel | Titel | Belichaming |
|------|-------|-------------|
| Standaard (HTML aan) | *(leeg)* | `<b>{message}</b>\n{label}` |
| Gewoon, geen vet (HTML uit) | *(leeg)* | `{message}\n{label}` |
| Labelkop | `{label}` | `{message}` |

Een lege belichaming valt terug op de ruwe paginatekst (Pushover vereist een niet-leeg bericht). Merk op dat Pushover geen `<b>`/`<i>`/etc. weergeeft tenzij **HTML-opmaak** aan staat, en het bericht is hard beperkt tot 1024 tekens. Een `\n` regelafbreking in de template geeft een enkele schone regelafbreking in zowel gewone als HTML-modus (Pushover eert regelafbrekingen in beide modi), dus `<b>{message}</b>\n{label}` toont de vetgedrukte tekst met elk label op zijn eigen regel. Gebruik `\n\n` waar u witruimte wilt scheiden (bijv. `<b>{message}</b>\n\n{label}` om de labels van het bericht af te zetten). In gewone modus tonen de `<b>`-tags letterlijk - zet HTML aan voor vet. **Test** geeft een voorbeeldpagina weer via de huidige Titel/Belichamingsvelden (en het HTML-selectievakje) zodat u een voorbeeld van de echte opmaak ziet, net als Telegram.

Voor een FLEX-groepsoproep breiden `{label}`/`{capcode}` uit naar de overeenkomende abonnees, een label per regel. Pushover heeft geen splitsen, dus de melding is hard beperkt tot 1024 tekens (titel 250); een groepsoproep met meer labels dan er passen wordt afgekapt. Als u elk label van een grote 122-capcode-groepsoproep nodig heeft, gebruik dan Telegram (dat kan splitsen over berichten).

**Per-capcode-controle:** net als bij Telegram weerspiegelen de *Verzendfilter*-modi SMTP, en elk filter (**Ctrl-F**) heeft een selectievakje *Pushover verzenden* dat **alleen** wordt geraadpleegd in de modus *Alleen geselecteerde filters*. FLEX-groepsoproepen worden bezorgd als een enkele melding met alle overeenkomende abonnee-capcodes. Activiteit gelogd naar `JJMMDD_pushover.log`.

**Per-filter prioriteits- en geluidsen overschrijvingen:** het Ctrl+F filtereditor-dialoog heeft twee aanvullende per-filter Pushover-bedieningselementen:

- *PO prioriteit* vervolgkeuzelijst — overschrijft de globale prioriteit voor dit filter. Kies "Global" om de Pushover-dialooginstelling te volgen, of kies een expliciet niveau (-2 Laagst / -1 Laag / 0 Normaal / 1 Hoog). Gebruik dit om hoog af te gaan voor een alarm-capcode terwijl u routinepagers op -2 houdt.
- *PO geluid* tekstbereik — overschrijft het globale geluid voor dit filter. Laat blanco om het globaal geconfigureerde geluid te gebruiken; voer een Pushover-geluidsnaam in (bijv. `siren`, `alien`, `none`) om het alleen voor deze capcode te overschrijven. Geluidsnamen zijn hoofdlettergevoelig en moeten exact overeenkomen met de Pushover API-naam.

Beide bedieningselementen zijn uitgeschakeld voor reject-regels en wanneer de Pushover-sink inactief is. In multi-edit-modus heeft de prioriteitsvervolgkeuzelijst een "Don't change"-item en het geluidsreeks toont "(huidige waarde niet wijzigen)" wanneer de geselecteerde filters verschillende waarden dragen. **Binnen FLEX-groepsoproepen** gelden de overschrijvingen nog steeds voor de enkele samengevoegde melding: de meest urgente prioriteit tussen de overeenkomende abonnees wint, en het eerste niet-lege per-filter geluid wordt gebruikt. Dit is belangrijk omdat een P2000-monitorCapcode meestal alleen als abonnee in een groepsoproep voorkomt - dus zonder dit zouden de overschrijvingen nooit van kracht worden.

### 10.4 Telnetserver

Configureer via **Opties → Telnetserver**.

> **Let op:** De ingebouwde Telnetserver is een aangepaste uitbreiding bedoeld voor integratie met gespecialiseerde monitorsoftware. Hij is niet nodig voor normaal PDW-gebruik en is standaard uitgeschakeld. Gewone gebruikers kunnen deze sectie overslaan.

PDW bevat een ingebouwde Telnetserver (standaard poort **8024**) die elk gedecodeerd bericht streamt via een gewone TCP-verbinding met behulp van een intern wire-formaat. Tot 25 gelijktijdige clients worden ondersteund. Een herverbindende client ontvangt een configureerbaar achterstallig venster (standaard 60 s) zodat het berichten kan inhalen die het miste tijdens de verbreking.

TCP-keepalive is ingeschakeld op elke verbinding, zodat een half-open client (na een router/NAT-rebind, crash of netwerkblip) wordt gedetecteerd en zijn slot binnen ongeveer een minuut wordt vrijgegeven, in plaats van als fantoom-"verbonden" client te blijven hangen. Meerdere gelijktijdige verbindingen van hetzelfde adres (verschillende clients achter een NAT, of test-setups) worden volledig ondersteund - elke TCP-verbinding wordt als zijn eigen sessie behandeld.

| Instelling | Beschrijving |
|------------|--------------|
| Poort | Standaard 8024 |
| Bindadres | Standaard 0.0.0.0 (alle interfaces) |
| Max. clients | Standaard 25 |
| Watchdog-interval | Seconden tussen `<WD>`-heartbeatberichten |
| Achterstallig venster | Seconden berichten die worden afgespeeld naar een herverbindende client (standaard 60 s) |

**Wire-formaatberichten:**

| Bericht | Betekenis |
|---------|-----------|
| `CC/FFF -ALPHA- capcode tekst` | FLEX alpha (CC=cyclus, FFF=frame) |
| `-ALPHA- capcode-N tekst` | POCSAG alpha (N=functie 0-3) |
| `<TX_START>` / `<TX_STOP>` | Transmissie start / einde |
| `<RXQ:NN>` | RX kwaliteit 0-100 % |
| `<WD>` | Watchdog-heartbeat |
| `<RS232:0>` / `<RS232:1>` | Serieledata verloren / hersteld |
| `<AUDIO:0>` / `<AUDIO:1>` | Audiosignaal verloren / hersteld |
| `<BUFFER_START>` / `<BUFFER_STOP>` | Herverbindingsvenster voor inhaalberichten |

Gebeurtenissen worden gelogd naar `JJMMDD_telnet_server.log`; de ruwe wire-stroom wordt gelogd naar `JJMMDD_telnet_traffic.log`.

### 10.5 MySQL-uitvoer

Configureer via **Opties → MySQL**.

Slaat alle gedecodeerde berichten op in een MySQL of MariaDB database. Geen externe DLL's of MySQL-clientbibliotheken vereist - het MySQL-wire-protocol is rechtstreeks in PDW geimplementeerd.

| Instelling | Beschrijving |
|------------|--------------|
| Host / Poort | Databaseserveradres (standaard poort 3306) |
| Database | Databasenaam (automatisch aangemaakt indien niet aanwezig) |
| Gebruikersnaam / Wachtwoord | MySQL-referenties |
| Schema | Klassiek / Uitgebreid / Geoptimaliseerd (zie hieronder) |
| Velden | Kies welke berichtvelden worden geschreven |
| Activiteitenlog | Schakel `pdw_mysql.log` in |

**Drie schemamodi:**

**Klassiek** - minimaal, drie kolommen:
```sql
CREATE TABLE `messages` (
    `id`        INT(11)     NOT NULL AUTO_INCREMENT,
    `timestamp` TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `capcode`   VARCHAR(10) NOT NULL DEFAULT '',
    `melding`   TEXT        NOT NULL,
    `label`     TEXT        NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

**Uitgebreid** - alle velden als tekst:
```sql
CREATE TABLE `messages` (
    `id`        INT(11)     NOT NULL AUTO_INCREMENT,
    `timestamp` TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `address`   VARCHAR(20) NOT NULL DEFAULT '',
    `msg_time`  VARCHAR(10) NOT NULL DEFAULT '',
    `msg_date`  VARCHAR(12) NOT NULL DEFAULT '',
    `mode`      VARCHAR(15) NOT NULL DEFAULT '',
    `msg_type`  VARCHAR(20) NOT NULL DEFAULT '',
    `bitrate`   VARCHAR(10) NOT NULL DEFAULT '',
    `message`   TEXT        NOT NULL,
    `label`     TEXT        NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

**Geoptimaliseerd** - getypte kolommen met indexen; aanbevolen voor nieuwe installaties:
```sql
CREATE TABLE `messages` (
    `id`          BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,
    `received`    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `address`     CHAR(9)           NOT NULL DEFAULT '',
    `mode`        VARCHAR(15)       NOT NULL DEFAULT '',
    `msg_type`    VARCHAR(10)       NOT NULL DEFAULT '',
    `bitrate`     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `message`     TEXT              NOT NULL,
    `label`       VARCHAR(256)      NOT NULL DEFAULT '',
    `subscribers` TEXT              NOT NULL DEFAULT '',
    `match_type`  TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    `label_color` VARCHAR(7)        NOT NULL DEFAULT '',
    PRIMARY KEY (`id`),
    INDEX `idx_address`   (`address`),
    INDEX `idx_received` (`received`),
    INDEX `idx_match`     (`match_type`),
    INDEX `idx_label`     (`label`(64)),
    FULLTEXT `ft_message` (`message`, `label`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
```

**Kolomreferentie (Geoptimaliseerd schema):**

| Kolom | Type | Opmerkingen |
|-------|------|-------------|
| `id` | BIGINT | Monotone auto-increment. Gebruik voor live polling: `WHERE id > :since` |
| `received` | DATETIME | Berichttijd in lokale tijdzone |
| `address` | CHAR(9) | Nul-geopvuld pageradres. Voorloopnullen worden bewaard. FLEX-groepscapcodes zijn `2029568`-`2029583` |
| `mode` | VARCHAR | Protocol + snelheid, bijv. `FLEX-1600`, `POCSAG-1200` |
| `msg_type` | VARCHAR | `ALPHA` / `NUMERIC` / `TONE` / `GROUP` |
| `bitrate` | SMALLINT | 512 / 1200 / 2400 (POCSAG) of 1600 / 3200 / 6400 (FLEX) |
| `message` | TEXT | Gedecodeerde tekst. `>>` (byte 0xBB) markeert een regelafbreking - geef weer als nieuwe regel |
| `label` | VARCHAR | Filterlabel; leeg als geen regel overeenkwam |
| `subscribers` | TEXT | JSON-array van groepsleden (zie hieronder); leeg voor niet-groepsberichten |
| `match_type` | TINYINT | `0` = geen match, `1` = gefilterd, `2` = monitor-only. Voor een groepsoproep is dit de sterkste match over alle leden (zodat de groep verschijnt in `match_type >= 1` queries); weergavestatus per lid staat in de `subscribers` JSON |
| `label_color` | VARCHAR(7) | `#RRGGBB`; leeg als geen |

**FLEX-groepsoproepen** slaan lidadressen op in `subscribers`. Elk lid heeft zijn eigen `match_type` (`0` = geen match, `1` = gefilterd, `2` = monitor-only) zodat een viewer elke capcode in het juiste paneel kan weergeven, precies zoals het PDW-venster dat doet:
```json
[
  {"address": "1234567", "label": "Ambulance 1", "match_type": 1, "color": "#1565c0"},
  {"address": "1234568", "label": "Ambulance 2", "match_type": 2}
]
```
De eigen `match_type`-kolom van de groepsrij wordt ingesteld op de sterkste match over alle leden, puur zodat de groep nog steeds verschijnt in `WHERE match_type >= 1` queries; weergave per lid wordt aangestuurd door de `match_type` in elk `subscribers`-item, niet door die kolom.

**Nuttige queries:**
```sql
-- Laatste 100 berichten
SELECT * FROM messages ORDER BY id DESC LIMIT 100;

-- Live polling (nieuwer dan het laatste geziene id)
SELECT * FROM messages WHERE id > :since ORDER BY id DESC LIMIT 50;

-- Een adres
SELECT * FROM messages WHERE address = '1234567' ORDER BY id DESC;

-- Alleen gematchte/gefilterde berichten
SELECT * FROM messages WHERE match_type >= 1;

-- Volledige-tekst zoeken
SELECT * FROM messages
WHERE MATCH(message, label) AGAINST ('brandweer' IN BOOLEAN MODE)
ORDER BY id DESC LIMIT 50;
```

Alleen `mysql_native_password` authenticatie wordt ondersteund. Op MySQL 8.0+ configureert u het account met `ALTER USER ... IDENTIFIED WITH mysql_native_password`.

### 10.6 SQLite-uitvoer

Configureer via **Opties → SQLite**.

Slaat alle gedecodeerde berichten op in een lokaal SQLite-databasebestand. Geen server, geen installatie, geen externe DLL's - de SQLite-engine is rechtstreeks in PDW gecompileerd.

| Instelling | Beschrijving |
|------------|--------------|
| Bestandspad | Standaard: `<PDW-map>\pdw.db` |
| Velden | Kies welke berichtvelden worden geschreven |
| LowWrite-modus | Verminder NVMe-schrijfversterking door commits te batchen elke ~15 s |
| PurgeDays | Verwijder rijen ouder dan N dagen (standaard uit) |
| MaxSizeMB | Verwijder oudste rijen als bestand deze grootte overschrijdt (standaard uit) |
| Activiteitenlog | Schakel `JJMMDD_pdw_sqlite.log` in |

**Schema:**
```sql
CREATE TABLE IF NOT EXISTS "messages" (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    received    TEXT    NOT NULL DEFAULT '',   -- 'YYYY-MM-DD HH:MM:SS'
    address     TEXT    NOT NULL DEFAULT '',   -- voorloopnullen bewaard
    mode        TEXT    NOT NULL DEFAULT '',
    msg_type    TEXT    NOT NULL DEFAULT '',
    bitrate     INTEGER NOT NULL DEFAULT 0,
    message     TEXT    NOT NULL DEFAULT '',
    label       TEXT    NOT NULL DEFAULT '',
    subscribers TEXT    NOT NULL DEFAULT '',
    match_type  INTEGER NOT NULL DEFAULT 0,
    label_color TEXT    NOT NULL DEFAULT ''
);
```

De kolomnamen en inhoud zijn identiek aan het MySQL Geoptimaliseerd schema; de enige verschillen zijn dat `address` `TEXT` is in plaats van `CHAR(9)` en er geen FULLTEXT-index is (gebruik `LIKE '%term%'` voor tekstzoeken).

**Automatisch toegepaste SQLite PRAGMA-instellingen:**

| PRAGMA | Normale modus | LowWrite-modus |
|--------|--------------|----------------|
| `journal_mode` | WAL | WAL |
| `synchronous` | NORMAL | OFF |
| `auto_vacuum` | INCREMENTAL | INCREMENTAL |
| Commit-cadans | Elk bericht | ~15 s gebatcht |

**LowWrite-modus** vermindert schrijfversterking op SSD's aanzienlijk. Het nadeel is dat tot 15 seconden berichten verloren kunnen gaan bij een harde crash of stroomstoring.

**Automatisch onderhoud** (standaard uit): PurgeDays en MaxSizeMB zijn beide standaard uitgeschakeld. PDW verwijdert nooit gegevens zonder expliciete configuratie. Onderhoud wordt eenmaal per uur uitgevoerd in de werkthread.

Klik op **Test** in het dialoogvenster om te verifiëren dat het bestand kan worden geopend. De database kan worden bekeken met een SQLite-browser (bijv. DB Browser for SQLite).

### 10.7 Systeemmeldingen

Configureer via **Opties → Systeemmeldingen**. Dit ene dialoogvenster bevat twee onafhankelijke e-mailmeldingen die dezelfde ontvangerlijst delen: de **RX-kwaliteitsmelding** en de **COM-verbindingsmelding**.

**RX-kwaliteitsmelding** - verstuurt een e-mailmelding wanneer de ontvangstqualiteit gedurende een aanhoudende periode onder een drempel blijft.

| Instelling | Standaard | Beschrijving |
|------------|-----------|--------------|
| Drempel | 80 % | Kwaliteit onder dit niveau start de timer |
| Herstelniveau | 90 % | Kwaliteit boven dit niveau annuleert een in behandeling zijnde melding |
| Minimale duur | 15 min | Hoe lang onder de drempel voordat de melding wordt verstuurd |
| Afkoeltijd | 120 min | Minimale tijd tussen herhaalde meldingen |

Gebruikt dezelfde SMTP-instellingen als op filters gebaseerde meldingen. Het ontvangeradres kan apart worden geconfigureerd van het filtermailontvanger.

De kwaliteitswaarde die de melding bewaakt is de **actieve health-bron** die op het Health-paneel in de werkbalk is gekozen (zie paragraaf 5.2): de klassieke RX-Q-score (standaard) of de strengere Penalty-system-score. De bron wisselen op het paneel wisselt direct wat de melding meet; de meldingsmail vermeldt de gebruikte bron. Let op het verschil bij totale radiostilte: de Penalty-system-score zakt na ongeveer twee minuten zonder iets decodeerbaars naar 0 %, zodat een stilgevallen ontvanger de melding wel degelijk triggert; de klassieke RX-Q-score verandert alleen als er gedecodeerd wordt en houdt bij dode ether zijn laatste waarde vast - gebruik bij die bron de COM-stip op het paneel om een dode seriële verbinding te zien.

**COM-verbindingsmelding (alleen seriële invoer).** De tweede melding in hetzelfde dialoogvenster bewaakt de seriële verbinding zelf in plaats van de decodeerkwaliteit. Hij verstuurt een e-mail wanneer seriële (COM) invoer aanstaat maar er gedurende een instelbaar aantal minuten geen data is ontvangen. Dit vangt het geval dat de kwaliteitsmelding met de klassieke bron kan missen: als de seriële invoer volledig wegvalt (adapter losgekoppeld, of een Moxa NPort waarvan de netwerktunnel wegvalt) bevriest de klassieke RX-Q-score alleen maar en triggert hij nooit - de COM-verbindingsmelding vangt precies dat. Hij herstelt automatisch zodra er weer data binnenkomt en mailt hooguit eens per afkoeltijd zolang de verbinding weg blijft.

Vink **Enable COM link-lost alert** aan in het dialoogvenster en stel de twee waarden in:

| Instelling | Standaard | Beschrijving |
|------------|-----------|--------------|
| Alert after no serial data for | 3 min | Hoe lang zonder seriële data voordat de eerste melding komt |
| Suppress repeated alerts for | 120 min | Minimale tijd tussen herhaalde meldingen |

Hij hergebruikt de ontvangerlijst en SMTP-host van de RX-kwaliteitsmelding (beide meldingen mailen naar dezelfde ontvangers). De instellingen worden in `pdw.ini` bewaard onder `[ComLinkAlert]` (`Enabled`, `Minutes`, `Cooldown`). Alleen actief bij seriële (COM) invoer; wordt genegeerd bij geluidskaartinvoer. Omdat een Moxa NPort de virtuele COM-poort open houdt wanneer de netwerkzijde wegvalt, ziet PDW dan "open maar geen data" (de COM-stip wordt oranje, niet rood); de melding behandelt zowel "open maar geen data" als "poort niet open" als een weggevallen verbinding.

### 10.8 Logbestanden en schrijfbuffering

Configureer via **Bestand → Logbestand openen/sluiten**.

Alle PDW-logbestanden gebruiken de datumgestempelde naamgevingsconventie `JJMMDD_<type>.log` en roteren automatisch om middernacht.

| Logbestand | Inhoud |
|------------|--------|
| `JJMMDD_monitor.log` | Alle gedecodeerde berichten |
| `JJMMDD_filter.log` | Filtergematche berichten |
| `JJMMDD_mail.log` | SMTP verzend-/foutlog |
| `JJMMDD_mqtt.log` | MQTT publicatielog |
| `JJMMDD_webhook.log` | Webhook verzendlog |
| `JJMMDD_telnet_server.log` | Telnet verbindingsgebeurtenissen |
| `JJMMDD_telnet_traffic.log` | Telnet wire-formaatstroom |
| `JJMMDD_pdw_sqlite.log` | SQLite feed-activiteit |
| `pdw_mysql.log` | MySQL feed-activiteit |

**ISO-tijdstempelformaat** - schakel in het Logbestand-dialoogvenster in om `JJJJ-MM-DD UU:MM:SS` tijdstempels te schrijven in monitor- en filterlograegels (de bestandsnamen op schijf blijven ongewijzigd). Indien ingeschakeld, worden de afzonderlijke selectievakjes Tijd en Datum grijs weergegeven.

**Schrijfbuffering (NVMe-bescherming)** - schakel in via **"Schrijfacties verminderen (buffer)"** in het Logbestand-dialoogvenster:

| Instelling | Standaard | Beschrijving |
|------------|-----------|--------------|
| Spoelinterval | 500 ms | Hoe vaak de buffer naar schijf wordt geschreven |
| Bufferslots | 512 | Maximum vermeldingen die worden vastgehouden voor een vroege spoeloperation |

Aanbevolen voor drukke POCSAG/FLEX-netwerken waar veel berichten per seconde hoge schrijfversterking kunnen veroorzaken op een SSD. Het maximale logverlies bij een harde crash is gelijk aan het spoelinterval.

**Verworpen berichten ook loggen** - een globale optie in het Logbestand-dialoogvenster. Standaard onderdrukt een filter met de **Reject**-actie zijn berichten overal, inclusief het logbestand op schijf. Met deze optie ingeschakeld worden verworpen berichten alsnog naar het monitorlogbestand geschreven - met dezelfde kolommen en tijdstempel (inclusief het ISO-formaat, indien geselecteerd) als een normale regel - zodat het log een compleet overzicht blijft, terwijl ze verborgen blijven op het scherm, buiten het filterlog en buiten elke feed (SMTP, MQTT, webhook, enz.). Het selectievakje is alleen beschikbaar wanneer het berichtenlog zelf is ingeschakeld.

### 10.9 Programmaopties

| Optie | Beschrijving |
|-------|--------------|
| Auto-scrollen | Houd het laatste bericht zichtbaar |
| Duplicaten tonen | Toon herhaalde berichten of onderdruk ze |
| Piep bij bericht | Hoorbare piep voor elk gedecodeerd bericht |
| Geminimaliseerd starten | Start PDW in het systeemvak |
| Positie opslaan | Herstel vensterpositie bij volgende start |

### 10.10 Lifecycle-commandohaken (pdw.ini)

PDW kan automatisch een extern commando uitvoeren bij het starten en bij het afsluiten - handig voor
een watchdog-script, een statusbestand voor een ander systeem, of om een dashboard te laten weten dat
de decoder wel/niet actief is. Dit is een functie die uitsluitend via `pdw.ini` wordt ingesteld, onder
een eigen `[Lifecycle]`-sectie (er is geen menu voor):

```
[Lifecycle]
LifecycleCmdEnabled=1
LifecycleCmd=C:\Scripts\pdw-lifecycle.bat
LifecycleCmdArgs="%S" "%V" "%P" "%U"
```

- `LifecycleCmd` is het volledige pad naar het programma of script dat uitgevoerd moet worden.
- `LifecycleCmdArgs` is een optionele argumentensjabloon met plaatshouders:

  | Plaatshouder | Betekenis |
  |---|---|
  | `%S` | `START` of `STOP` |
  | `%V` | Versiestring van PDW |
  | `%P` | Proces-ID van PDW |
  | `%U` | Uptime in seconden sinds PDW startte (alleen bij STOP; leeg bij START) |

- Het commando wordt eenmaal uitgevoerd bij opstarten (direct na het aanmaken van het hoofdvenster,
  `%S` = `START`) en eenmaal bij afsluiten (helemaal aan het begin van het afsluitproces, vóórdat PDW
  zijn feeds en logs sluit, `%S` = `STOP`), fire-and-forget - PDW wacht niet tot het commando klaar is.
- De werkmap wordt ingesteld op de map waarin `LifecycleCmd` zich bevindt, zodat een script dat
  bestanden naast zichzelf leest deze altijd vindt, ongeacht de werkmap van PDW zelf.

Laat `LifecycleCmdEnabled=0` (of laat de hele sectie weg) en er verandert niets - de functie staat
standaard uit.

---

## 11. Weergavemenu

| Item | Beschrijving |
|------|--------------|
| Lettertype | Verander het berichtenlijst-lettertype |
| Kolommen | Toon of verberg afzonderlijke kolommen |
| Kleuren | Bewerk het kleurenschema. De "Default" filterlabelkleur is een goed leesbaar azuurblauw; met "Better contrast" aan zijn alle 17 labelkleuren onderling verschillend |
| Woordafbreking | Laat lange berichten doorlopen in de berichtkolom |
| Menubalk tonen | Verberg of toon de menubalk (de werkbalk blijft in beide gevallen zichtbaar); sneltoets **Ctrl+Shift+M** |

### Menubalk verbergen

**Weergave > Menubalk tonen** schakelt de hele menubalk (Bestand, Bewerken, Interface, ...) aan of
uit; de werkbalk en de berichtvensters blijven ongewijzigd en vullen de vrijgekomen ruimte op. Zodra
de menubalk verborgen is, kan er niet meer op geklikt worden, dus biedt PDW twee manieren om hem
terug te krijgen: de sneltoets **Ctrl+Shift+M** (werkt zowel met zichtbare als verborgen menubalk),
of rechtsklikken ergens in het hoofdvenster (ook op de werkbalk) en **Show Menu Bar** kiezen in het
contextmenu. De instelling wordt onthouden in `pdw.ini` en bij de volgende start hersteld.

#### 11.1 Eigen filterlabelkleuren (pdw.ini)

De 17 filterlabelkleuren kunnen in `pdw.ini` worden overschreven zonder enige menu-instelling. Slot 0 gebruikt de sleutel `Color.FilterLabel`; slots 1-16 gebruiken `Color.FilterLabel1` tot en met `Color.FilterLabel16`. Elke waarde is `R,G,B` (0-255). Hier volgt een referentie van alle mogelijke sleuven met hun ingebouwde standaardkleuren:

```ini
Color.FilterLabel=64,128,255        ; 0  Default (Light Blue)
Color.FilterLabel1=255,255,0        ; 1  Yellow
Color.FilterLabel2=255,0,0          ; 2  Red
Color.FilterLabel3=255,170,0        ; 3  Orange
Color.FilterLabel4=0,0,255          ; 4  Blue
Color.FilterLabel5=0,128,128        ; 5  Cyan
Color.FilterLabel6=255,255,255      ; 6  White
Color.FilterLabel7=0,255,0          ; 7  Green
Color.FilterLabel8=192,192,192      ; 8  Gray
Color.FilterLabel9=128,128,64       ; 9  Brown
Color.FilterLabel10=0,255,255       ; 10 Light Cyan
Color.FilterLabel11=0,51,153        ; 11 Dark Blue
Color.FilterLabel12=255,0,255       ; 12 Magenta
Color.FilterLabel13=51,204,153      ; 13 Sea Green
Color.FilterLabel14=255,153,204     ; 14 Pink
Color.FilterLabel15=153,255,255     ; 15 Ice Blue
Color.FilterLabel16=128,128,255     ; 16 Light Blue (alt)
```

Laat een sleutel weg (of leeg) en dat slot houdt zijn ingebouwde kleur - er is niets om aan te zetten. Filters blijven naar een kleur verwijzen via het slotnummer, dus bestaande filters worden niet beinvloed. Eigen kleuren worden getoond zoals opgegeven; de "Better contrast"-remap past alleen de ingebouwde standaardkleuren aan.

---

## 12. Gedecodeerde protocollen

### 12.1 POCSAG

POCSAG (Post Office Code Standardisation Advisory Group) is het meest gebruikte pagingprotocol wereldwijd.

| Snelheid | Gebruik |
|----------|---------|
| 512 baud | Oudere systemen |
| 1200 baud | Meest gebruikelijk |
| 2400 baud | Hogesnelheidssystemen |

PDW decodeert alle drie snelheden gelijktijdig wanneer POCSAG/FLEX-modus actief is. Berichttypen: **Alpha**, **Numeriek**, **Toon**.

Typische pagingfrequenties (Europa):

| Bereik | Opmerkingen |
|--------|-------------|
| 136-139 MHz NFM | |
| 153-154 MHz NFM | |
| 454-455 MHz NFM | |
| 466-467 MHz NFM | P2000 / Semafoon (NL/BE) |
| 35-36 MHz NFM | |
| 43-44 MHz NFM | |

### 12.2 FLEX en ReFLEX

FLEX is een hogesnelheid pagingprotocol ontwikkeld door Motorola.

| Snelheid | Opmerkingen |
|----------|-------------|
| 1600 baud | 2-niveau FSK |
| 3200 baud | 4-niveau FSK |
| 6400 baud | 4-niveau FSK |

PDW decodeert alle drie snelheden gelijktijdig. Berichttypen: **Alpha**, **Numeriek**, **Toon**, **Korte instructie**, **Frame-info**, **Groepsoproepen**.

**Meervoudig frameherstel:** lange FLEX-alpha-berichten die meerdere frames beslaan, worden verzameld en weergegeven als een volledige string. Een succesvol hersteld bericht kan optioneel op het scherm gemarkeerd worden met een asterisk (`*`) direct achter de capcode. Deze markering staat **standaard uit**; schakel hem in met **Mark reassembled fragmented messages with '*' after the capcode** in **Weergave -> Schermopties** als u in een oogopslag wilt zien welke berichten uit fragmenten zijn samengesteld. Het in- of uitschakelen van de markering verandert alleen de weergave - de fragmentherstel-logica zelf draait altijd. Een fragment dat (nog) niet kon worden voltooid, krijgt in plaats daarvan een tekstmarkering naast het bericht - `[fragmented]`, of `[<type> fragment - incomplete]` voor een los fragment zonder voorafgaande reeks - zodat u een volledig herstel kunt onderscheiden van een gedeeltelijk.

**Groepsoproepen:** een groepscapcode (bereik 2029568-2029583) richt zich tegelijkertijd op meerdere individuele pagers. Het netwerk stuurt eerst Short Instructions ("luister naar groep X, het bericht volgt in frame N"), die PDW vertellen welke abonnee-capcodes tot de groep behoren en in welk frame het bericht moet worden verwacht; PDW toont dan alle abonnee-capcodes en hun labels samen met het groepsbericht. Omdat een lang bericht in fragmenten kan worden gesplitst die in latere frames worden verzonden - en een drukke frame kan het ook vertragen - kan het groepsbericht rechtmatig een frame of meer later aankomen dan aangekondig. PDW verdraagt dit binnen een kleine uitstelvenster, zodat de abonneelijst altijd bij het juiste bericht blijft en wordt weergegeven zodra het bericht is voltooid.

**ReFLEX** is een uitgebreide versie van FLEX die tweerichtingspaging ondersteunt. PDW decodeert ReFLEX via hetzelfde decoderpad.

### 12.3 ACARS

ACARS (Aircraft Communications Addressing and Reporting System) wordt gebruikt voor gegevensuitwisseling tussen vliegtuigen en grondstations.

PDW decodeert ACARS op **2400 baud** via geluidskaartinvoer.

**ACARS-frequenties:**

| Frequentie | Regio |
|------------|-------|
| 131,550 MHz | Primair - VS, Canada, Azie/Pacific |
| 131,725 MHz | Primair - Europa |
| 130,025 MHz | Secundair - VS |
| 129,125 MHz | Tertiair - VS |
| 131,475 MHz | Air Canada (privee) |
| 131,525 MHz | Tertiair - Europa |
| 131,450 MHz | Primair - Japan |

**ACARS-databasebestanden:** PDW kan luchtvaartmaatschappijcodes, vliegtuigtypes, grondstation-ID's en route-informatie opzoeken uit optionele databasebestanden geplaatst in de PDW-toepassingsmap:

| Bestand | Inhoud |
|---------|--------|
| `label.df` | Berichtlabelbeschrijvingen |
| `aircraft.df` | Vliegtuigtype-codes |
| `country.df` | Landcodes |
| `airline.df` | Luchtvaartmaatschappijcodes |
| `ground.df` | Grondstation-ID's |
| `routes.df` | Vluchtroute-informatie |

Deze bestanden zijn optioneel. Als ze niet aanwezig zijn, decodeert PDW nog steeds ACARS-berichten maar zonder de opzoeklabels.

### 12.4 MOBITEX

MOBITEX is een mobiel pakketdatanetwerk protocol dat voornamelijk wordt gebruikt voor paging en dataterminals.

**Eerste keer instellen - framesynchronisatie:**

Bij het voor de eerste keer decoderen van een Mobitex-netwerk moet u de juiste framesynchronisatie instellen voor uw lokale netwerk:

1. Stem af op een actief Mobitex-signaal.
2. Stel de optie **Gegevens omdraaien** handmatig in via Interface-instelling (Auto werkt pas als de framesync bekend is).
3. Let op de 4-cijferige framesync-nummers aan de linkerkant van het scherm. Het nummer dat het meest verschijnt en soms berichtgegevens ernaast heeft, is het juiste. De gebruikelijke Europese framesync is **EB90**.
4. Open **Opties → Programma** en schakel **Framesync controleren** in.
5. Voer uw 4-cijferige framesync-nummer in het veld **Framesync** in.

**Basisstationlabels (base-ids.txt):** PDW leest `base-ids.txt` uit de toepassingsmap om basisstationnamen weer te geven in de titelbalk. Het formaat is een vermelding per regel:

```
# base-ids.txt
0001=London
001B=Amsterdam
011B=Zoetermeer
```

Regels die beginnen met `#` zijn opmerkingen. Het ID is de hexadecimale Base-ID zoals weergegeven in het PDW-scherm. Als het bestand niet aanwezig is, wordt het ruwe numerieke Base-ID weergegeven.

### 12.5 ERMES

ERMES (European Radio Messaging System) is een pan-Europese pagingstandaard die werkt op 6,25 kHz kanaalafstand. PDW decodeert ERMES-toon, numerieke en alfanumerieke berichten inclusief foutdetectie en -correctie.

---

## 13. Berichtformaat

Een typische gedecodeerde berichtregel:

```
10:24:31  03-06-26  FLEX-1600  ALPHA  1600  0012345  Ambulance  Patient contact req
```

| Deel | Voorbeeld | Betekenis |
|------|-----------|-----------|
| Tijd | `10:24:31` | Decodetijd |
| Datum | `03-06-26` | DD-MM-JJ |
| Modus | `FLEX-1600` | Protocol en bitsnelheid |
| Type | `ALPHA` | Berichttype |
| Bitrate | `1600` | Bitsnelheid in bps |
| Capcode | `0012345` | Pageradres (voorloopnullen bewaard) |
| Label | `Ambulance` | Filterlabel (leeg als geen filter overeenkwam) |
| Bericht | `Patient contact req` | Gedecodeerde tekst |

---

## 14. Logbestandsformaat

Monitor log (`.log`) regels:
```
10:24:31 03-06-26 FLEX-1600 ALPHA 0012345 Patient contact req
```

Met ingeschakelde ISO-tijdstempeloptie:
```
2026-06-03 10:24:31 FLEX-1600 ALPHA 0012345 Patient contact req
```

Filter log (`.flt`) regels volgen hetzelfde formaat maar bevatten het gematchte label.

---

## 15. Ondersteuningsbestanden

PDW leest verschillende optionele ondersteuningsbestanden uit de toepassingsmap. Geen van deze bestanden is vereist voor basisbediening.

| Bestand | Gebruikt door | Beschrijving |
|---------|---------------|--------------|
| `filters.ini` | Filtersysteem | Uw filterregels. Aangemaakt en bijgewerkt door PDW wanneer u filters opslaat. |
| `pdw.ini` | Alles | Hoofdinstellingenbestand. Aangemaakt door PDW bij eerste start. |
| `language.df` | Gebruikersinterface | Interface-taalstrings. Plaats in de PDW-map om standaard Engelse tekst te overschrijven. |
| `base-ids.txt` | MOBITEX | Koppelt hexadecimale Base-ID-codes aan leesbare stationnamen. Formaat: `ID=Naam` per regel, `#` voor opmerkingen. |
| `label.df` | ACARS | ACARS-berichtlabelbeschrijvingen. |
| `aircraft.df` | ACARS | Vliegtuigtype-code opzoeken. |
| `country.df` | ACARS | Landcode opzoeken. |
| `airline.df` | ACARS | Luchtvaartmaatschappijcode opzoeken. |
| `ground.df` | ACARS | Grondstation-ID opzoeken. |
| `routes.df` | ACARS | Vluchtroutenopzoeken. |

---

## 16. Windows-meldingen

Druk op **Ctrl+T** om een Windows toast-testmelding te sturen. PDW gebruikt de native `IUserNotification` COM-interface (Action Center toast), niet de verouderde balloon-tip API die Windows 10 en Windows 11 niet meer weergeven.

Het systeemvakpictogram biedt:
- Minimaliseren naar systeemvak door het venster te sluiten
- Klik op het systeemvakpictogram om het venster te herstellen
- Optionele per-bericht toast-meldingen voor filtergematche berichten

---

## 17. Hoge-DPI-ondersteuning

PDW declareert `System DPI Aware` in zijn toepassingsmanifest. Lettertypen, werkbalk en indeling worden herberekend vanuit de werkelijke scherm-DPI bij het opstarten. PDW wordt correct weergegeven op monitoren met 125 %, 150 % en 200 % schaling (4K / HiDPI) zonder vervaging of afkapping.

De werkbalkknoppen gebruiken een hoge-resolutie-iconenset: elk icoon is opgeslagen als een 72x72 32-bits afbeelding met alfakanaal en wordt vloeiend verkleind naar de exacte knopgrootte voor uw schermschaling, zodat de iconen bij elke schaalfactor scherp en anti-aliased blijven. PDW gebruikt daarnaast de moderne Windows-stijl (Common Controls v6) voor zijn dialoogvensters.

---

## 18. Meerdere instanties / titelbalk

Bij het gelijktijdig uitvoeren van twee PDW-vensters (bijvoorbeeld een die audio decodeert en een op een seriele poort), toont de titelbalk de actieve **[MODUS]** - FLEX of POCSAG - zodat u onmiddellijk kunt zien welk venster welk is.

---

## 19. COM-poorten >= 10

COM-poortnummers 10 en hoger worden volledig ondersteund. Voer het poortnummer direct in het Interface-instellingsdialoogvenster in. PDW opent de poort met de `\\.\COMn` notatie die Windows vereist voor hoge poortnummers.

### Exclusieve COM-poorttoegang

PDW opent zijn COM-poort voor exclusieve toegang. Terwijl PDW actief is en verbonden, kan geen enkel ander programma dezelfde poort openen - de actieve instantie behoudt volledig, ononderbroken eigendom van de verbinding. Als PDW een poort niet kan openen omdat een ander programma die al vasthoudt, meldt het **"Kan de geselecteerde COM-poort niet openen - deze is mogelijk al in gebruik door een ander programma."** Sluit het andere programma (of kies een andere poort) en probeer opnieuw. Dit is vooral van belang voor virtuele COM-poorten (zoals een Moxa NPort-redirector via TCP): zonder exclusieve toegang zou een tweede opener de bytestroom splitsen en het decoderen stoppen.

PDW opent de COM-poort met `GENERIC_READ | GENERIC_WRITE`-machtigingen en share-modus 0 (geen delen) zodat de OS zelf het afdwingt - dit werkt tegen alle programma's, niet alleen PDW.

---

## 20. Probleemoplossing

**Geen berichten worden gedecodeerd**
- Zorg ervoor dat geen enkel ander programma de COM-poort vasthoudt. PDW opent de poort exclusief, dus als een ander programma het eerst had geopend, meldt PDW dat het de poort niet kan openen. Zie [sectie 19](#19-com-poorten--10).
- Controleer of de signaalmeterbar (rechtsboven) beweegt. Als dat niet het geval is, bereikt het audiosignaal PDW niet. Verhoog het radiovolume of controleer de kabel.
- Zorg ervoor dat de juiste invoerbron (Line In / Mic) is geselecteerd in de Windows-opnamemixer.
- Controleer of het juiste protocol is geselecteerd in het Monitormenu.
- Probeer alle vier configuraties in Interface-instelling (Discriminator, Earphone, Speaker Out, Tape/Rec Out).
- Een discriminatortap geeft de beste signaalkwaliteit en lost hardnekkige problemen vaak op.

**Slecht decoderingspercentage / veel fouten**
- Probeer de aangepaste drempelinstellingen (zie [sectie 4.1](#41-geluidskaartinvoer)).
- Verminder andere audiobronnen die kunnen interfereren.
- Een discriminatortap zal hardnekkige kwaliteitsproblemen meestal oplossen.

**MOBITEX decodeert niets**
- Controleer of het framesync-nummer correct is ingesteld voor uw netwerk (zie [sectie 12.4](#124-mobitex)).
- Zorg ervoor dat Gegevens omdraaien correct is ingesteld - Auto werkt pas als de framesync bekend is.

**ACARS: geen luchtvaartmaatschappij- / vliegtuiglabels**
- De ACARS-databasebestanden (`label.df`, `airline.df`, etc.) ontbreken in de PDW-map. PDW decodeert nog steeds berichten maar kan geen labels opzoeken.

**SMTP-test mislukt**
- Controleer server, poort, gebruikersnaam en wachtwoord.
- Poort 465 = impliciete TLS; poort 587 = STARTTLS. Niet verwisselen.
- Controleer `JJMMDD_mail.log` in de PDW-map voor gedetailleerde foutmeldingen.

**Telnet-clients maken geen verbinding**
- Controleer of de Telnetserver is ingeschakeld in Opties en dat de poort (standaard 8024) niet wordt geblokkeerd door een firewall.
- Controleer `JJMMDD_telnet_server.log` voor verbindings- en foutmeldingen.

**SQLite / MySQL-feed schrijft niet**
- Klik op **Test** in het betreffende instellingsdialoogvenster om de verbinding te verifiëren.
- Controleer het activiteitenlogbestand van de feed.
- Voor MySQL: zorg ervoor dat het account `mysql_native_password` gebruikt.

**RX Kwaliteit is altijd laag na PDW-herstart**
- Dit is normaal: de kwaliteitsbuckets zijn leeg bij het opstarten en vullen zich gedurende de eerste paar minuten. De `<RXQ:NN>`-waarde in de Telnetstroom zal stabiliseren zodra de decoder een volledige cyclus heeft gezien.

**Hoge-DPI-indeling ziet er verkeerd uit**
- Zorg ervoor dat u de release-build uitvoert. Debug-builds bevatten mogelijk niet het DPI-bewuste manifest.

**Hoofdvenster is niet zichtbaar na het starten (alleen het tray-icoon werkt)**
- Een oudere PDW-build kon de Windows-positie van een geminimaliseerd venster (-32000,-32000) opslaan in `pdw.ini` wanneer PDW geminimaliseerd werd afgesloten; de volgende start plaatste het venster dan ver buiten beeld. Huidige builds herstellen dit automatisch bij het opstarten (het venster valt terug naar linksboven op de primaire monitor). Gebruikt u een oudere build: sluit PDW, open `pdw.ini` en zet `xPos=0` en `yPos=0`.

---

## 21. Credits en licentie

PDW is gelicentieerd onder de **GNU General Public License v3.0** (GPL-3.0). Alle toevoegingen in deze repository worden onder dezelfde voorwaarden uitgebracht. Zie `LICENSE` voor de volledige tekst.

PDW is oorspronkelijk ontwikkeld door **Jason Petty** (2001-2004) en **Peter Hunt** (2004-2010), die het in 2013 als open source uitbrachten. Deze repository bouwt voort op de community-fork op [github.com/Discriminator/PDW](https://github.com/Discriminator/PDW).

Bijdragers aan de Discriminator-fork: Discriminator, andrey2805, evroza, Muspah, lt-holman, senf666.

Verdere ontwikkeling in deze fork door Rob de Hoog.
