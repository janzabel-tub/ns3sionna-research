# Unabhängige Mehrstream-BER- und PER-Validierung

Der Versuch prüft separat die korrekte Implementierung der allgemeinen
`MEAN_UNCODED_BER`-Abstraktion und ihre Genauigkeit für tatsächlich kodierte
HE-Datenpakete. Die Ergebnisse gehören zum Lauf vom 28. Oktober 2026 unter
[`data/he_error_matlab_2026_10_28`](../data/he_error_matlab_2026_10_28/).
Der frühere rein numerische MU-MIMO-Vergleich bleibt davon getrennt.

## Ergebnis

**Die getestete BER-Implementierung besteht; die quantitative PER-Abbildung
besteht den unabhängigen Paketversuch nicht.**

| Prüfung | Ergebnis |
| --- | --- |
| Mathematische und Integrationsfälle | 1002 bestanden |
| Numerische Einzelprüfungen | 70752 |
| Größte absolute BER-Abweichung | 9,992007221626409e-16 |
| Größte absolute effektive-SINR-Abweichung | 1,6370904631912708e-11 |
| MATLAB-Pakete | 42 Fälle mit je 600 Paketen, insgesamt 25200 |
| Native PER außerhalb des Konfidenzintervalls | 30 von 42 Fällen |
| Davon gleichmäßige AWGN-Fälle | 12 von 23 Fällen |
| Davon ungleiche Stream-/Tonqualitäten | 18 von 19 Fällen |
| Gleiche mittlere BER, statistisch verschiedene PER | 17 von 19 Paaren |
| Absichtlich ausgelassener Stream | Negativtest erkennt den Fehler |
| Bestehende native Suite `he-effective-sinr-mapping` | alle fünf Fälle bestanden |

Ein besonders deutlicher Gegenfall verwendet BPSK/BCC, zwei Streams und eine
tatsächliche PSDU-Länge von 260 Byte:

| Größe | Gleiche Streams | Ungleiche Streams |
| --- | --- | --- |
| Stream-SINRs, linear | 0,9332543 und 0,9332543 | 0,4481961 und 14,7910839 |
| Mittlere unkodierte BER | 0,08593774497 | 0,08593774497 |
| Effektive SINR, linear | 0,9332543 | 0,9332543 |
| Vorhergesagte PER | 0,5310929 | 0,5310929 |
| Beobachtete Paketfehler | 277/600 | 0/600 |
| Gemessene PER | 0,4616667 | 0 |
| Verwendetes Wilson-Intervall | [0,38673; 0,53837] | [0; 0,02313] |

Null beobachtete Fehler bedeuten hier eine obere statistische Grenze von
etwa 2,31 Prozent, nicht eine bewiesene wahre PER von null. Im ungleichen
Fall liegen die gemessenen unkodierten Stream-BERs bei etwa 0,17145 und 0;
die Referenz erwartet 0,17188 und 2,68e-8. Der große PER-Unterschied entsteht
somit trotz korrekt vorgegebener und kontrollierter Bitfehlerwahrscheinlichkeiten.

Die getesteten Streamschleifen, Datentonauswahl, Mittelung und inverse
BER-Abbildung stimmen mit der unabhängigen Rechnung überein. Die Abstraktion
verliert jedoch Informationen über die Zuverlässigkeitsverteilung. Ein
Soft-FEC-Decoder kann sehr zuverlässige Bits zur Rekonstruktion schlechter
Bits verwenden. Derselbe Mittelwert der Hard-Decision-BER garantiert deshalb
keine identische kodierte PER. Auch ein einzelner AWGN-Offset kann diesen
Gegenfall nicht beheben: Beide Fälle würden weiterhin dieselbe korrigierte
effektive SINR erhalten.

Die Produktionsabbildung wurde für diesen Versuch nicht verändert oder auf
die Messungen gefittet. Eine verbesserte Abstraktion benötigt eine gesondert
validierte Metrik für Soft-Information beziehungsweise eine kalibrierte
effektive-SINR-Abbildung und unabhängige Testdaten. Die vorliegenden Ergebnisse
rechtfertigen keine allgemeine quantitative PER-Abnahme des aktuellen Modells.

## Implementierungsprüfung

`tools/he_gray_ber_reference.m` integriert die Gauß-Wahrscheinlichkeit über
die Entscheidungsregionen der von MATLAB `qammod` gelieferten Gray-Konstellation.
Die Hamming-Abstände der tatsächlichen Bitlabels gewichten jede
Fehlentscheidung. Dies ist ein anderer Rechenweg als die QAM-Koeffizientenreihe
im produktiven C++-Code. BPSK wird über die Gauß-Tailwahrscheinlichkeit geprüft.
Alle SINRs sind Symbol-SINRs Es/N0. Eine Umrechnung mit der Coderate ist für
diese unkodierte BER nicht erforderlich.

Die Referenz überprüft alle HE-MCS 0 bis 11, entsprechend BPSK, QPSK,
16-/64-/256-/1024-QAM. Die 960 mathematischen Fälle enthalten skalare SINRs
von null bis 10000 sowie Fälle mit 1, 2, 3, 4 und 8 Streams. Die weiteren 42
Fälle sind zugleich die Eingaben des kodierten Paketversuchs.

Die Streamfälle umfassen gleiche und stark ungleiche Qualitäten,
Frequenzselektivität, Nullstreams und explizite Datentonauswahl. Absichtlich
gute ausgeschlossene Töne prüfen, dass diese nicht in den Mittelwert eingehen.
Die BER wird zuerst pro Stream und Datenton berechnet und erst danach gemittelt.
Die inverse BER-Funktion wird unabhängig mit MATLAB `fzero` bestimmt.

`examples/ns3_he_error_reference_export.cc` führt diese Fälle durch die
produktiven SU- und DL-MU-HE-Abstraktionen. Diagonale synthetische Kanäle
erzeugen die gewünschten Post-MMSE-SINRs. Zwei DL-Nutzer besitzen getrennte
Streamblöcke; der zweite hat um 10 dB geringere SINRs. Damit wird auch geprüft,
dass Nutzer nicht versehentlich gemeinsam komprimiert werden. Die Eingabe der
Nutzer erfolgt in umgekehrter Reihenfolge.

Der Vergleich prüft einzelne BER-Werte, Post-MMSE-SINRs, mittlere BER,
effektive SINR, getrennte Nutzerabbildung und SU-/DL-PER-Parität. Für die BER
gilt eine absolute Toleranz von 2e-13, für effektive SINR 1e-8 + 1e-8 mal
Referenzwert. Die PER-Parität zwischen SU und DL ist ein Integrationstest
und wird nicht als unabhängiger physikalischer PER-Nachweis gewertet.

## Kodierter Paketversuch

Die 42 Fälle verwenden WLAN Toolbox R2026a mit 1, 2 oder 4 Streams:

- BCC: MCS 0, 1, 3, 5 und 8;
- LDPC: MCS 0 und 10; BP-Decodierung, maximal 12 Iterationen;
- 20 MHz, 800 ns GI, direkte räumliche Zuordnung und keine STBC;
- überwiegend APEP-Länge 256 Byte, zusätzlich 32 und 1200 Byte bei BPSK/BCC.

Die tatsächliche PSDU-Länge wird mit `getPSDULength(cfg)` bestimmt und dem
C++-Modell übergeben. Sie kann abhängig von MCS und NSS von der APEP-Länge
abweichen. Dadurch wird vermieden, ungleiche kodierte Bitlängen zu vergleichen.

Für jedes Paket erzeugt `wlanWaveformGenerator` neue zufällige PSDU-Bits mit
zufälliger Scramblerinitialisierung. `wlanHEDemodulate` liefert die
HE-Datensymbole auf den 234 Datentönen. Perfekte, aus einem rauschfreien HE-LTF
gewonnene CSI und bekannte Rauschvarianz bilden die Modellannahmen nach.
Unabhängiges komplexes AWGN mit Varianz eins wird im FFT-Bereich addiert.
Die diagonalen Kanalgewinne werden so gewählt, dass jeder Datenton und Stream
seine vorgegebene SINR erhält. `wlanHEEqualize` und `wlanHEDataBitRecover`
führen MMSE-Entzerrung, Soft-Demapping und FEC-Decodierung aus.

Ein Paket gilt als fehlerhaft, sobald sich die zurückgewonnenen PSDU-Bits von
den gesendeten Bits unterscheiden. Pro Fall werden 600 Pakete, insgesamt
25200 Pakete, geprüft. Eine rauschfreie Kontrollübertragung muss in jedem
Fall alle Bits korrekt zurückliefern. Zusätzlich werden die unkodierten
Hard-Decision-Bitfehler pro Stream gezählt und mit der analytischen BER
gespeichert; dies kontrolliert die SNR- und Konstellationsnormierung.

Der Test untersucht ideal synchronisierte HE-Nutzdaten. Header-/CCA-Empfang,
Kanalschätzfehler, reale MIMO-Interferenz, Raytracing und MAC-Durchsatz sind
nicht Gegenstand dieses Paketversuchs. Der MATLAB-Empfänger verwendet die
öffentlichen SU-Datenfunktionen; die native DL-MU-Abbildung wird numerisch
pro Nutzer mitgeprüft. Ein vollständiger HE-MU-/HE-TB-Wellenformversuch ist
damit nicht belegt.

## Gleiche BER, verschiedene Zuverlässigkeitsverteilungen

Die nativen AWGN-Kurven wählen vor der MATLAB-Paketmessung je PHY-Kontext
einen Punkt mit vorhergesagter PER nahe 0,5. Es werden keine Parameter anhand
der gemessenen MATLAB-PER angepasst und keine Betas oder Offsets gefittet.
Dies ist ein gezielter Test im Wasserfallbereich, keine vollständige PER-Kurve.

Zu einem gleichmäßigen Referenzfall werden ungleiche Fälle konstruiert:
Die Hälfte der Streams beziehungsweise Datentöne wird sehr gut, die andere
entsprechend schlechter. Die mittlere unkodierte BER bleibt exakt gleich.
Damit müssen effektive SINR und PER-Vorhersage des untersuchten Modells gleich
bleiben. Die MATLAB-PER wird für beide Verteilungen unabhängig gemessen.

Dieser Vergleich trennt zwei Fehlerquellen:

1. Die native AWGN-PER-Kurve kann bereits beim gleichmäßigen Fall vom
   unabhängigen MATLAB-Empfänger abweichen.
2. Auch bei gleicher mittlerer BER kann der FEC-Decoder unterschiedliche
   Paketfehlerwahrscheinlichkeiten liefern. Dann reicht die BER-Mittelung
   nicht aus, selbst wenn man die AWGN-Kurve separat korrigiert.

Für 42 Tabellenvergleiche und 19 Vergleiche mit der jeweiligen gleichmäßigen
Referenz wird vor der Messung ein Bonferroni-Niveau von 0,01/61 festgelegt.
Die Auswertung verwendet Wilson-Intervalle für einzelne PER-Werte und
Newcombe-Intervalle für die Differenz zweier unabhängiger PER-Messungen.
Das angestrebte gemeinsame Konfidenzniveau beträgt 99 Prozent; die verwendeten
Score-Intervalle sind approximativ. Bei null beobachteten Fehlern wird eine
positive obere Konfidenzgrenze angegeben, keine wahre PER von exakt null.

## Wiederholen und Ergebnisse lesen

Aus dem ns-3-Hauptverzeichnis:

```sh
cmake -S . -B cmake-cache
./ns3 build -j 2 ns3_he_error_reference_export ns3_wifi_mimo_awgn_curve
python3 contrib/sionna/tools/validate_he_error_matlab.py \
  --trials 600 --output /tmp/he-error-new-run
```

Das Ausgabeverzeichnis muss neu sein. MATLAB benötigt Communications Toolbox
und WLAN Toolbox. Der Runner speichert den Aufruf, relevante Quell- und
Binärhashes, Rohdaten und Paketfehlerzähler. Quelltexte und Programme dürfen
während des Laufs nicht verändert werden.

`mapping_report.json` beschreibt die Implementierungsprüfung;
`packet_measurements.json` enthält die Messungen und Fehlerzähler;
`report.json` trennt Implementierungsstatus und PER-Genauigkeit;
`provenance.json` dokumentiert Versionen und Prüfsummen. Exitcode 2 bedeutet
einen abgeschlossenen Versuch mit verfehlter PER-Genauigkeit. Andere Fehler
werden als Ausführungs- oder Prüfungsfehler behandelt.

Ein Negativversuch ersetzt bei einem Zweistreamfall die effektive SINR durch
die SINR ausschließlich des ersten Streams. Die unabhängige Prüfung muss
dies als Fehler erkennen. Die bestehende native Suite
`he-effective-sinr-mapping` prüft ergänzend die vorhandenen PHY-Verträge.

Die Ergebnisgrafiken und eine CSV-Tabelle lassen sich mit einer vorhandenen
Matplotlib-Umgebung erstellen:

```sh
python contrib/sionna/tools/plot_he_error_validation.py \
  contrib/sionna/data/he_error_matlab_2026_10_28
```

Quellen: [Gray-Konstellationen in qammod](https://www.mathworks.com/help/comm/ref/qammod.html),
[HE-Entzerrung](https://www.mathworks.com/help/wlan/ref/wlanheequalize.html),
[HE-Datenrückgewinnung](https://www.mathworks.com/help/wlan/ref/wlanhedatabitrecover.html).
Die konkreten verwendeten APIs wurden zusätzlich gegen die lokal installierte
R2026a-Version geprüft.
