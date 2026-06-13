// FIX [Version]: single source of truth for PDW version number
#define PDW_VERSION_MAJOR  3
#define PDW_VERSION_MINOR  6
#define PDW_VERSION_PATCH  7
#define PDW_VERSION_STR    "3.6.7"

#define IDC_STATIC             -1

#define IDD_ICON                    97
#define PDWICON                     98
#define PDWMENU                     99
#define PDWACCEL                   100

#define ABOUTDLGBOX                101
#define LOGFILEDLGBOX              102
#define SETUPDLGBOX                103
#define OPTIONSDLGBOX              104
#define GENERALOPTIONSDLGBOX       105
#define COLORSDLGBOX               106
#define SCROLLDLGBOX               107
#define SYSTEMTRAYDLGBOX           110
#define FILTERDLGBOX               111
#define FILTEREDITDLGBOX           112
#define FILTEROPTIONSDLGBOX        113
#define FILTERFINDDLGBOX           114
#define FILTERCHECKDUPLICATEDLGBOX 115
#define MONSTATDLGBOX              116
#define CUSTOM_AUDIO_DLG           117
#define DEBUGDLGBOX                118
#define ACARS_COLORSDLGBOX         119
#define MOBITEX_COLORSDLGBOX       120
#define ERMES_COLORSDLGBOX         121
#define CLEARSCREENDLGBOX          123
#define MAIL_DLGBOX                124
#define SCREENOPTIONSDLGBOX        125
#define WEBHOOK_DLGBOX             131

#define IDM_LOGFILE                201
#define IDM_EXIT                   202

#define IDM_COPY_SELECTION         210
#define IDM_COPY_UPPER             211
#define IDM_COPY_LOWER             212
#define IDM_COPY_SAVE              213
#define IDM_COPY_PRINT             214

#define IDM_INTERFACE              220
#define IDM_VOLUME                 221

#define IDM_OPTIONS                230
#define IDM_GENERAL                231
#define IDM_MAIL                   232
#define IDM_WEBHOOK                233

#define IDM_FILTERS                240
#define IDM_FILTEROPTIONS          241
#define IDM_RELOAD                 242
#define IDM_RESET_HITCOUNTERS      243
#define IDM_FILTERFILE_EN          244
#define IDM_FILTERCOMMANDFILE      245

#define IDM_CLEARDISPLAY           250
#define IDM_COLOR                  251
#define IDM_FONT                   252
#define IDM_SCREENOPTIONS          253
#define IDM_SCROLLBACK             254
#define IDM_SYSTEMTRAY             255

#define IDM_POCSAGFLEX             260
#define IDM_ACARS                  261
#define IDM_MOBITEX                262
#define IDM_ERMES                  263
#define IDM_MONSTAT                264

#define IDM_HELP                   280
#define IDM_ABOUT                  281

#define IDM_DEBUG                  290
#define IDM_PANE_SWITCH            291
#define IDM_RESET_COUNTERS         292  // Shift+F12: reset diagnostic counters (missed/buffer/frag)
#define IDM_RESET_ALL              293  // Alt+Shift+F12: full reset + separator line

#define IDM_PLAYBACK               295
#define IDM_RECORD                 296
#define IDM_AUTORECORD             297

#define IDS_APPNAME                591
#define IDS_SHORT_APPNAME          592
#define IDS_API_FAILED_MSG         593

#define IDC_ERR                    600
#define IDC_OKCANCEL               601
#define IDC_VERSION                602

#define IDC_LOGFILEEN              610
#define IDC_LOGFILE                611
#define IDC_LOGFILEDATE            612
#define IDC_LOGBROWSE              613
#define IDC_LOGCOLUMN              620 // 621 to 627 are reserved

#define IDC_COMENABLE              630
#define IDC_COMPORT                631
#define IDC_COMADDR                632
#define IDC_COMIRQ                 633
#define IDC_COMSLICER              634
#define IDC_COMRS232               635
#define IDC_RS232MODE              636
#define IDC_LEVEL                  637
#define IDC_AUDIOENABLE            638
#define IDC_AUDIOSAMPLERATE        639
#define IDC_AUDIOCONFIG            640
#define IDC_AUDIOCUSTOM            641
#define IDC_AUDIODEVICES           642

#define IDC_CONFIRMEXIT            650
#define IDC_DECODEPOCSAG           651
#define IDC_DECODEFLEX             652
#define IDC_POCSAG_512             653
#define IDC_POCSAG_1200            654
#define IDC_POCSAG_2400            655
#define IDC_POCSAG_FNU             656
#define IDC_POCSAG_BOTH            657
#define IDC_FLEX_1600              659
#define IDC_FLEX_3200              660
#define IDC_FLEX_6400              661
#define IDC_ACARS_PC_YES           662
#define IDC_ACARS_PC_NO            663
#define IDC_MB_USEFRSYNC           664
#define IDC_MB_FRSYNC              665
#define IDC_MB_BITSYNC             666
#define IDC_MB_BITRATE             667
#define IDC_MB_RAMNET              668
#define IDC_MB_MIN_MSG             669
#define IDC_MB_BITSCRAMBLER        670
#define IDC_MB_SHOWMPAK            671
#define IDC_MB_SHOWTEXT            672
#define IDC_MB_SHOWHPDATA          673
#define IDC_MB_SHOWDATA            674
#define IDC_MB_SHOWHPID            675
#define IDC_MB_SHOWSWEEP           676
#define IDC_MB_VERBOSE             677
#define IDC_SHOWINSTR              678
#define IDC_CONVERT_SI             679
#define IDC_SHOW_CFS               680
#define IDC_GENERALOPTIONS         681
#define IDC_BLOCKDUPLICATE         682
#define IDC_BLOCKDUPOPTION         683
#define IDC_BLOCKDUPTIMER          684
#define IDC_BLOCKEDTXT             685
#define IDC_WORDWRAP               686
#define IDC_LOGFILEPATH            687
#define IDC_LOGFILEPATHBROWSE      688
#define IDC_LOGFILEPATHDEFAULT     689
#define IDC_LOGFILEPATHTEXT        690
#define IDC_LINEFEEDS              691
#define IDC_FLEXTIME               692
#define IDC_SHOW_REJECTBLOCKED     693
#define IDC_SEP_SCREEN             694
#define IDC_SEP_LOGFILE            695
#define IDC_SEP_FILTERFILE         696
#define IDC_SEP_SEPFILES           697
#define IDC_SEP_MESSAGE            698
#define IDC_SEP_MSGTIMEDATE        699
#define IDC_DATEFORMAT             700
#define IDC_MONTHNUMBER            701
#define IDC_MONTHNUMBER2           702
#define IDC_SEPARATOR              703
#define IDC_SEPARATOR_FILTER       704

#define IDC_COLORBACKGND           710
#define IDC_COLORCAPCODE           711
#define IDC_COLORFLEXPHASE         712
#define IDC_COLORTIMESTAMP         713
#define IDC_COLORBITERRORS         714
#define IDC_COLORNUMERIC           715
#define IDC_COLORALPHANUM          716
#define IDC_COLORFLEXBIN           717
#define IDC_COLORFILTMATCH         718
#define IDC_COLORDEFAULT           719
#define IDC_COLORWIN               720
#define IDC_COLORFILTERLABEL       721
#define IDC_COLORINSTRUCTIONS      722

#define IDC_AC_COLORMSGNO          730
#define IDC_AC_COLORTIMESTAMP      731
#define IDC_AC_COLORDBI            732
#define IDC_AC_COLORMODE           733
#define IDC_AC_COLORTAGS           734
#define IDC_AC_COLORMSG            735
#define IDC_AC_COLORBACKGND        736
#define IDC_AC_COLORBITERRORS      737
#define IDC_AC_COLORFILTMATCH      738
#define IDC_AC_COLORDEFAULT        739
#define IDC_AC_COLORWIN            740
#define IDC_AC_COLORFILTERLABEL    741

#define IDC_MB_COLORMAN            750
#define IDC_MB_COLORTIMESTAMP      751
#define IDC_MB_COLORSENDER         752
#define IDC_MB_COLORTYPE           753
#define IDC_MB_COLORMESSAGE        754
#define IDC_MB_COLORMISC           755
#define IDC_MB_COLORBACKGND        756
#define IDC_MB_COLORFILTMATCH      757
#define IDC_MB_COLORBITERRORS      758
#define IDC_MB_COLORDEFAULT        759
#define IDC_MB_COLORWIN            760
#define IDC_MB_COLORFILTERLABEL    761

#define IDC_EM_COLORCAPCODE        770
#define IDC_EM_COLORTIMESTAMP      771
#define IDC_EM_COLORFUNCTION       772
#define IDC_EM_COLORBITERRORS      773
#define IDC_EM_COLORNUMERIC        774
#define IDC_EM_COLORALPHANUM       775
#define IDC_EM_COLORBACKGND        776
#define IDC_EM_COLORFILTMATCH      777
#define IDC_EM_COLORTRANSPARENT    778
#define IDC_EM_COLORFILTERLABEL    779
#define IDC_EM_COLORWIN            780
#define IDC_EM_COLORDEFAULT        781

#define IDC_SCREEN_COLUMN          800 // 801 to 807 are reserved

#define IDC_FLEXGROUPMODE          810
#define IDC_FGM_LOGGING            811
#define IDC_FGM_COMBINE            812
#define IDC_FGM_HIDEGROUPCODES     813

#define IDC_SCROLLPANE1            890
#define IDC_SCROLLPANE2            891
#define IDC_SCROLLSPEED            892
#define IDC_PERCENTPANE1           893
#define IDC_PERCENTPANE2           894

#define IDC_FILTERS                900
#define IDC_FILTERRELOAD           901
#define IDC_FILTERADD              902
#define IDC_FILTEREDIT             903
#define IDC_FILTERDEL              904
#define IDC_FILTEROPTIONS          905
#define IDC_FILTER_CHECK_RESULTS   906

#define IDC_FILTERFILEEN           910
#define IDC_FILTERBROWSE           911
#define IDC_FILTERFILE             912
#define IDC_FILTERFILEDATE         913
#define IDC_FILTERTONE             914
#define IDC_FILTERNUMERIC          915
#define IDC_FILTERBINARYHEX        916
#define IDC_FILTERBEEP             917
#define IDC_FILTERDEFTYPE          918
#define IDC_FILTERWINDOWONLY       919
#define IDC_FILTERSCOLORS          920
#define IDC_FILTERSEXTRA           921

#define IDC_FILTERLOGCOLUMN        930 // 931 to 937 are reserved

#define IDC_LABELLOG               940
#define IDC_LABELSEPFILES          941
#define IDC_LABELNEWLINE           942

#define IDC_FILTERCMDBROWSE        950
#define IDC_FILTERCMDFILE          951
#define IDC_FILTERCMDARGS          952
#define IDC_FILTERCMDEN            953
#define IDC_FILTERHITS             954
#define IDC_FILTERLASTHIT          955
#define IDC_FILTERRESET            956
#define IDC_HITCOUNTER_BOX         957
#define IDC_DONTCHANGE             958

#define IDC_FILTERTYPE             960
#define IDC_FILTERREJECT           961
#define IDC_FILTERCAPCODE          962
#define IDC_FILTERFNU              963
#define IDC_FILTERLABEL            964
#define IDC_FILTERLABELEN          965
#define IDC_FILTER_MONITOR_ONLY    966
#define IDC_FILTERTEXT             967
#define IDC_FILTERCMD              968
#define IDC_FILTERAUDIO            969
#define IDC_FILTERLABELCOLOR       970
#define IDC_FILTERMATCHEXACT       971
#define IDC_FILTERSMTP             972
#define IDC_FILTERTELEGRAM         975   // FIX [Telegram]: per-filter Telegram checkbox
#define IDC_FILTERPUSHOVER         979   // FIX [Telegram]: per-filter Pushover checkbox (phase 2)
#define IDC_FILTERRXTXMAN          973
#define IDC_FILTEREDITHELP         974

#define IDC_FILTER_PREVIOUS        976
#define IDC_FILTER_APPLY           977
#define IDC_FILTER_NEXT            978

#define IDC_FILTERFIND             980
#define IDC_FILTERFIND_CASE        981
#define IDC_FILTERFIND_HITS        982
#define IDC_FILTERFIND_DUPLICATE   983
#define IDC_FILTERDUP_PCT          984
#define IDC_PROGRESS1              985

#define IDC_SEPFILTERFILEEN        990
#define IDC_SEPFILTERFILE1         991
#define IDC_SEPFILTERFILE2         992
#define IDC_SEPFILTERFILE3         993
#define IDC_SEPFILTERFILEBROWSE1   994
#define IDC_SEPFILTERFILEBROWSE2   995
#define IDC_SEPFILTERFILEBROWSE3   996
#define IDC_SEPFILTERBOX           997
#define IDC_FILTER_IGNORE_GROUPCALL 998   // FIX [GroupcallScreenHide]: hide capcode from on-screen group view

#define IDC_SYSTEMTRAY            1000
#define IDC_SYSTEMTRAY_RESTORE    1001
#define IDC_SYSTEMTRAY_NEW        1002
#define IDC_SYSTEMTRAY_MONLY      1003
#define IDC_SYSTEMTRAY_FILTER     1004
#define IDC_SYSTEMTRAY_NOTIFY     1005  // FIX [TrayBalloon]: combobox balloon-tip modus
#define IDC_SYSTEMTRAY_LABEL      1006  // FIX [TrayBalloon]: checkbox filter label in balloon

#define IDC_CLEAR_PANE1           1010
#define IDC_CLEAR_PANE2           1011

#define IDC_STATHRF64N            1091
#define IDC_STATHRF64A            1092
#define IDC_STATHRF32N            1093
#define IDC_STATHRF32A            1094
#define IDC_STATHRF16N            1095
#define IDC_STATHRF16A            1096
#define IDC_STATHRP24N            1097
#define IDC_STATHRP24A            1098
#define IDC_STATHRP12N            1099
#define IDC_STATHRP12A            1100
#define IDC_STATHRP512N           1101
#define IDC_STATHRP512A           1102
#define IDC_STATHR_ACARS_N        1103
#define IDC_STATHR_ACARS_A        1104
#define IDC_STATHR_MB_N           1105
#define IDC_STATHR_MB_A           1106
#define IDC_STATHR_EM_N           1107
#define IDC_STATHR_EM_A           1108

#define IDC_STATDLF64N            1110
#define IDC_STATDLF64A            1111
#define IDC_STATDLF32N            1112
#define IDC_STATDLF32A            1113
#define IDC_STATDLF16N            1114
#define IDC_STATDLF16A            1115
#define IDC_STATDLP24N            1116
#define IDC_STATDLP24A            1117
#define IDC_STATDLP12N            1118
#define IDC_STATDLP12A            1119
#define IDC_STATDLP512N           1120
#define IDC_STATDLP512A           1121
#define IDC_STATDL_ACARS_N        1122
#define IDC_STATDL_ACARS_A        1123
#define IDC_STATDL_MB_N           1124
#define IDC_STATDL_MB_A           1125
#define IDC_STATDL_EM_N           1126
#define IDC_STATDL_EM_A           1127

#define IDC_STATFILEEN            1130
#define IDC_STATFILE              1131
#define IDC_STATFILEDATE          1132
#define IDC_STATBROWSE            1133

#define IDC_THRESHOLD512          1140
#define IDC_THRESHOLD1200         1141
#define IDC_THRESHOLD2400         1142
#define IDC_THRESHOLD1600         1143
#define IDC_RESYNC512             1144
#define IDC_RESYNC1200            1145
#define IDC_RESYNC2400            1146
#define IDC_RESYNC1600            1147
#define IDC_CENTERING512          1148
#define IDC_CENTERING1200         1149
#define IDC_CENTERING2400         1150
#define IDC_CENTERING1600         1151

#define IDC_WEBSITE               1160

#define IDC_DEBUG_OS              1170
#define IDC_DEBUG_STARTED         1171
#define IDC_DEBUG_RUNNING         1172
#define IDC_DEBUG_INPUT           1173
#define IDC_DEBUG_FLEXTIME        1174
#define IDC_DEBUG_MSG             1175
#define IDC_DEBUG_REJECTED        1176
#define IDC_DEBUG_BLOCKED         1177
#define IDC_DEBUG_BLOCKBUFFER     1178
#define IDC_DEBUG_MISSED          1179
#define IDC_DEBUG_GROUPMSG        1180
#define IDC_DEBUG_TEST            1181
#define IDC_DEBUG_FRAGMSG         1182
#define IDC_DEBUG_RESET           1183  // Reset confirmation label (F11)

#define IDW_TOOL_BAR              1200
#define IDT_TOOLBAR_BTN0          1201
#define IDT_TOOLBAR_BTN1          1202
#define IDT_TOOLBAR_BTN2          1203
#define IDT_TOOLBAR_BTN3          1204
#define IDT_TOOLBAR_BTN4          1205
#define IDT_TOOLBAR_BTN5          1206
#define IDT_TOOLBAR_BTN6          1207
#define IDT_TOOLBAR_BTN7          1208
#define IDT_TOOLBAR_BTN8          1209
#define IDT_TOOLBAR_BTN9          1210
#define IDT_TOOLBAR_BTN10         1211
#define IDT_TOOLBAR_BTN11         1212
#define IDT_TOOLBAR_BTN12         1213
#define IDT_TOOLBAR_BTN13         1214
#define IDT_TOOLBAR_BTN14         1215
#define IDT_TOOLBAR_BTN15         1216
#define IDT_TOOLBAR_BTN16         1217

#define IDC_SMTP                  1221
#define IDC_SMTP_SETTING          1222
#define IDC_SMTP_HOST             1223
#define IDC_SMTP_HELO             1224
#define IDC_SMTP_TO               1225
#define IDC_SMTP_FROM             1226
#define IDC_SMTP_TEST             1227
#define IDC_SMTP_RESPONSE         1228
#define IDC_SMTP_ADDRESS          1229
#define IDC_SMTP_TIME             1230
#define IDC_SMTP_DATE             1231
#define IDC_SMTP_MODE             1232
#define IDC_SMTP_TYPE             1233
#define IDC_SMTP_BITRATE          1234
#define IDC_SMTP_MESSAGE          1235
#define IDC_SMTP_LABEL            1236
#define IDC_SMTP_USER             1237
#define IDC_SMTP_PASSWORD         1238
#define IDC_SMTP_PORT             1239
#define IDC_SMTP_AUTH             1240
#define IDC_SMTP_SENDIN           1241
#define IDC_SMTP_CHARSET          1242
#define IDC_SMTP_SESSIONS         1243
#define IDC_SMTP_EMAILS           1244
#define IDC_SMTP_ERRORS           1245
#define IDC_SMTP_LASTERROR        1246
#define IDC_SMTP_SSL			  1247
#define IDC_SMTP_ENCRYPTION       1248
#define IDC_MAIL_SPLIT_CONFIG     1249	// FIX [MailSplit]: enable split Subject/Body mode
#define IDC_SMTP_LOG_ERRORS       1391	// FIX [SmtpLog]

#define IDS_SIGIND                1250

// FIX [MailSplit]: Subject-row checkboxes (parallel to the existing IDC_SMTP_* body row)
// IDs 1392-1402: kept clear of IDC_TS_* (1380-1390) and IDC_SMTP_LOG_ERRORS (1391)
#define IDC_SMTP_SUBJ_ADDRESS     1392
#define IDC_SMTP_SUBJ_TIME        1393
#define IDC_SMTP_SUBJ_DATE        1394
#define IDC_SMTP_SUBJ_MODE        1395
#define IDC_SMTP_SUBJ_TYPE        1396
#define IDC_SMTP_SUBJ_BITRATE     1397
#define IDC_SMTP_SUBJ_MESSAGE     1398
#define IDC_SMTP_SUBJ_LABEL       1399
#define IDC_SMTP_SUBJ_TXT         1400	// static "Subject:" label
#define IDC_SMTP_BODY_TXT         1401	// static "Body:" label
#define IDC_SMTP_NOTIF_TXT        1402	// static "Notification" label (legacy mode only)
#define IDS_EXCLAM                1251
#define IDS_ABOUTLOGO             1252

#define IDM_ENGLISH               1260  // Note there must be a gap of atleast 11
                                        // places before next ID begins. i.e. 1271.
                                        // 1260 to 1270 are in use.

#define IDT_MENU_COPY             1300
#define IDT_MENU_PASTE            1301
#define IDT_MENU_SORT_ADDRESS     1302
#define IDT_MENU_SORT_LABEL       1303
#define IDT_MENU_RESET            1304
#define IDT_MENU_SELECTALL        1305
#define IDT_FILTERCHECKDUPLICATE  1306

#define IDT_MENU_RESTORE          1310

#define IDC_WEBHOOK_ENABLED       1320
#define IDC_WEBHOOK_URL           1321
#define IDC_WEBHOOK_TRUST_SS      1322
#define IDC_WEBHOOK_LOG           1323
#define IDC_WEBHOOK_STATUS        1324
#define IDC_WEBHOOK_PAD_CAPCODES  1325
#define IDC_WEBHOOK_PAGERMON_FMT  1326
#define IDC_WEBHOOK_SEND_IN       1327
#define IDC_WEBHOOK_LABEL_CSV     1328   // label – CSV mode (bit 0)
#define IDC_WEBHOOK_FIELD_TIME    1329
#define IDC_WEBHOOK_FIELD_DATE    1330
#define IDC_WEBHOOK_FIELD_TS      1331
#define IDC_WEBHOOK_FIELD_MODE    1332
#define IDC_WEBHOOK_FIELD_TYPE    1333
#define IDC_WEBHOOK_FIELD_BITRATE 1334
#define IDC_WEBHOOK_LABEL_PERCAP  1335   // label – per-capcode mode (bit 7)
#define IDC_WEBHOOK_LABEL_ARRAY   1336   // label – subscribers array mode (bit 8)

#define MQTT_DLGBOX               132
#define IDM_MQTT                  234
#define IDM_DEBUGLOG              235

#define IDC_MQTT_ENABLED          1340
#define IDC_MQTT_BROKER           1341
#define IDC_MQTT_PORT             1342
#define IDC_MQTT_CLIENTID         1343
#define IDC_MQTT_USER             1344
#define IDC_MQTT_PASSWORD         1345
#define IDC_MQTT_TOPIC            1346
#define IDC_MQTT_QOS              1347
#define IDC_MQTT_RETAIN           1348
#define IDC_MQTT_LOG              1349
#define IDC_MQTT_PAD_CAPCODES     1350
#define IDC_MQTT_FLAT_JSON        1351
#define IDC_MQTT_TOPIC_SUFFIX     1352
#define IDC_MQTT_SEND_IN          1353
#define IDC_MQTT_STATUS           1354
#define IDC_MQTT_LABEL_CSV        1355   // label - CSV mode (bit 0)
#define IDC_MQTT_FIELD_TIME       1356
#define IDC_MQTT_FIELD_DATE       1357
#define IDC_MQTT_FIELD_TS         1358
#define IDC_MQTT_FIELD_MODE       1359
#define IDC_MQTT_FIELD_TYPE       1360
#define IDC_MQTT_FIELD_BITRATE    1361
#define IDC_MQTT_LABEL_PERCAP     1362   // label - per-capcode mode (bit 7)
#define IDC_MQTT_LABEL_ARRAY      1363   // label - subscribers array mode (bit 8)
#define IDC_MQTT_TEST             1364   // FIX [ConnTest]: "Test connection" button

#define DISPLAYOPTIONSDLGBOX      133
#define IDM_DISPLAYOPTIONS        236
#define IDC_DISPLAY_BETTER_CONTRAST  1370
#define IDC_DISPLAY_LIGHTER_BG       1371
#define IDC_DISPLAYOPTIONS_BTN       1372

// Telnet server (utils/telnet_server.cpp) — Ctrl-N opens the config dialog.
#define TELNETSERVER_DLGBOX       134
#define IDM_TELNETSERVER          237
#define IDC_TS_ENABLED            1380
#define IDC_TS_BIND               1381
#define IDC_TS_PORT               1382
#define IDC_TS_MAXCLIENTS         1383
#define IDC_TS_WDSEC              1384
#define IDC_TS_BUFFERTIME         1385
#define IDC_TS_LOGTOFILE          1386
#define IDC_TS_STATUS             1387
#define IDC_TS_WIRELOG            1388
#define IDC_TS_CLIENTS            1389
#define IDC_TS_ACTIVITY           1390

// RX Quality Alert dialog (FIX [RxQualAlert])
#define RXQUAL_ALERT_DLGBOX       135
#define IDM_RXQUAL_ALERT          238
#define IDC_RXQA_EN               1400
#define IDC_RXQA_MAILTO           1401
#define IDC_RXQA_THR              1402
#define IDC_RXQA_REC              1403
#define IDC_RXQA_MIN              1404
#define IDC_RXQA_COOL             1405
#define IDC_RXQA_WARN             1406

// FIX [MySQLFeed]: MySQL output feed dialog (utils/mysql.cpp)
#define MYSQL_DLGBOX              136
#define IDM_MYSQL                 239
#define IDC_MYSQL_ENABLED         1410
#define IDC_MYSQL_HOST            1411
#define IDC_MYSQL_PORT            1412
#define IDC_MYSQL_USER            1413
#define IDC_MYSQL_PASSWORD        1414
#define IDC_MYSQL_DATABASE        1415
#define IDC_MYSQL_TABLE           1416
#define IDC_MYSQL_FIELD_MODE      1417
#define IDC_MYSQL_FIELD_TYPE      1418
#define IDC_MYSQL_FIELD_BITRATE   1419
#define IDC_MYSQL_FIELD_MESSAGE   1420
#define IDC_MYSQL_FIELD_LABEL     1421
#define IDC_MYSQL_STATUS          1422
#define IDC_MYSQL_LOG             1423
#define IDC_MYSQL_SCHEMA          1424
#define IDC_MYSQL_TEST            1425   // FIX [ConnTest]: "Test connection" button

// FIX [SqliteFeed]: SQLite output feed dialog
#define SQLITE_DLGBOX             137
#define IDM_SQLITE                246
#define IDC_SQLITE_ENABLED        1430
#define IDC_SQLITE_PATH           1431
#define IDC_SQLITE_BROWSE         1432
#define IDC_SQLITE_TABLE          1433
#define IDC_SQLITE_FIELD_MODE     1434
#define IDC_SQLITE_FIELD_TYPE     1435
#define IDC_SQLITE_FIELD_BITRATE  1436
#define IDC_SQLITE_FIELD_MESSAGE  1437
#define IDC_SQLITE_FIELD_LABEL    1438
#define IDC_SQLITE_LOWWRITE       1439
#define IDC_SQLITE_PURGE_EN       1440
#define IDC_SQLITE_PURGE_DAYS     1441
#define IDC_SQLITE_MAXSIZE        1442
#define IDC_SQLITE_LOG            1443
#define IDC_SQLITE_STATUS         1444
#define IDC_SQLITE_TEST           1445

// FIX [Telegram]: Telegram Bot API output sink dialog (utils/telegram.cpp)
#define TELEGRAM_DLGBOX           138
#define IDM_TELEGRAM              247
#define IDC_TG_ENABLED            1550
#define IDC_TG_TOKEN              1551
#define IDC_TG_CHATIDS            1552
#define IDC_TG_TITLE              1553
#define IDC_TG_THREADID           1554
#define IDC_TG_SILENT             1555
#define IDC_TG_NOPREVIEW          1556
#define IDC_TG_SPLIT              1557
#define IDC_TG_SEND_IN            1558
#define IDC_TG_LOG                1559
#define IDC_TG_STATUS             1560
#define IDC_TG_TEST               1561
#define IDC_TG_DISCOVER           1562
#define IDC_TG_BODY               1563

// FIX [Pushover]: Pushover output sink dialog (utils/pushover.cpp)
#define PUSHOVER_DLGBOX           139
#define IDM_PUSHOVER              248
#define IDC_PO_ENABLED            1600
#define IDC_PO_APPTOKEN           1601
#define IDC_PO_USERKEY            1602
#define IDC_PO_TITLE              1603
#define IDC_PO_PRIORITY           1604
#define IDC_PO_SOUND              1605
#define IDC_PO_DEVICE             1606
#define IDC_PO_HTML               1607
#define IDC_PO_SEND_IN            1608
#define IDC_PO_LOG                1609
#define IDC_PO_STATUS             1610
#define IDC_PO_TEST               1611
#define IDC_PO_BODY               1612

// FIX [LogManager]: write-buffering controls in the logfile dialog (1500+ to avoid SQLite collision)
#define IDC_LOG_BUFFER_EN         1500   // checkbox  "Reduce disk writes (buffer)"
#define IDC_LOG_FLUSH_MS          1501   // edit       flush interval in ms
#define IDC_LOG_BUFFER_SLOTS      1502   // edit       ring-buffer slot count
#define IDC_LOG_ISO_DATETIME      1503   // checkbox  "ISO date format in log content"
// FIX [LogRejected]: global option to also write reject-filtered messages to the message log
#define IDC_LOG_REJECTED          1504   // checkbox  "Also log rejected messages"
