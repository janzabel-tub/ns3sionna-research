# MATLAB-Abgleich des MU-MIMO-Kerns

Der Vergleich verwendet eine lokal ausgeführte, unabhängige MATLAB-Rechnung
und die produktiven C++-Klassen `DlMuMimoLinkModel`,
`DlMuMimoPrecodingMatrix` und `UlMuMimoLinkModel`. Er prüft die numerische
MU-MIMO-Abstraktion bei perfekter Kanalkenntnis, idealer Synchronisation,
unabhängigen Streams und räumlich weißem Rauschen mit Einheitskovarianz.
Eine WLAN-Wellenform-, EESM-, PER- oder Hardwarevalidierung ist damit nicht
abgedeckt. Es wird ausschließlich Basis-MATLAB benötigt.

## Reproduzieren

Aus dem ns-3-Hauptverzeichnis:

```sh
# Nach erstmaligem Hinzufügen des neuen CMake-Ziels:
cmake -S . -B cmake-cache
./ns3 build -j 2 ns3_mumimo_reference_export
python3 contrib/sionna/tools/validate_mumimo_matlab.py \
  --output /tmp/mumimo-matlab-new-run
```

Das Ausgabeverzeichnis muss neu sein. `--matlab` und `--binary` erlauben
abweichende Programmpfade. Der Python-Runner benötigt Python >= 3.11.
Alternativ kann `validate_mumimo_matlab(binaryPath, outputDirectory)` nach
`addpath('contrib/sionna/tools')` direkt in MATLAB aufgerufen werden; die
zusätzlichen SHA-256-Nachweise liefert der Python-Runner.

## Unabhängige Vergleichsrechnung

MATLAB erzeugt sämtliche Eingaben mit festem Zufallsseed 20261028 und
schreibt sie mit 17 signifikanten Stellen in `fixtures.txt`. Der dünne
C++-Adapter liest nur diese Eingaben, ruft den Produktionscode auf und
exportiert Ergebnisse nach `native.json`. Er enthält keine Referenzformeln
und keine fest hinterlegten Erwartungswerte.

MATLAB berechnet danach aus den ursprünglichen Eingaben seine eigene
Referenz. Insbesondere übernimmt es weder Precoder noch Combiner aus dem
C++-Ergebnis:

- Downlink-ZF: QR-Zerlegung von `A'` statt der Eigen-SVD des C++-Kerns.
  Für `A' = Q*R` wird `W = Q/R'` berechnet.
- Downlink-RZF: `W = A'/(A*A' + alpha*I)`. Anschließend wird wie im
  Modellvertrag die gesamte Precoderleistung auf die Streamzahl normiert.
- Downlink-MMSE und Uplink-MMSE: Lösung über eine Cholesky-Zerlegung statt
  der Eigen-LDLT-Zerlegung. Die Kovarianz wird gegen numerische Rundungsreste
  explizit hermitesch symmetrisiert.
- Uplink-ZF: QR-basierte Berechnung des Detektors statt Eigen-SVD.
- MMSE-SINR zusätzlich über die Kovarianz **ohne** den gewünschten Stream:
  `p * real(h' * (C_interference_plus_noise \ h))`. Diese Identität wird
  sowohl gegen die MATLAB-Leistungsbilanz als auch gegen die native SINR
  geprüft.

Dabei bedeutet `'` die konjugierte Transposition. Beim Austausch der
komplexen Tensoren wird ausschließlich die nicht konjugierende
Transposition `.'` verwendet. Die MATLAB-Referenz wird separat unter
`matlab_reference.json` gespeichert.

MATLABs [QR-Dokumentation](https://www.mathworks.com/help/matlab/ref/qr.html)
und die Dokumentation zu
[linearen Gleichungssystemen](https://www.mathworks.com/help/matlab/ref/double.mldivide.html)
beschreiben die verwendeten numerischen Operationen. Die konkreten
Erwartungswerte entstehen aus den Testeingaben und sind keine universellen
MU-MIMO-Konstanten.

## Abdeckung und Akzeptanz

Die 233 Fälle umfassen:

- die besprochenen Downlink-SINRs 5/6 bei gemeinsamem Interferenzraum und
  10 bei orthogonalen Gruppen;
- den Uplink-Nah-Fern-Fall mit ZF-SINRs 75 und 0,75 sowie MMSE-SINRs 87,5
  und 0,752475247524752;
- orthogonale, rangdefiziente und schlecht konditionierte Kanäle;
- 200 deterministische Zufallsfälle mit komplexen, frequenzselektiven
  Kanälen, 1 bis 4 Nutzern, bis zu 8 Antennen und 1 bis 7 Subträgern;
- 12 zusätzliche Downlink-Fälle mit zwei RX-Antennen und zwei Streams pro
  Nutzer, vertauschter Spaltenzuordnung und ungleichen Spaltenleistungen;
- Nullkanal, Null-SNR und einen überlasteten Uplink-MMSE-Fall.

Die vier erwarteten Ablehnungen sind Rangverlustfälle. Auch Downlink-RZF
lehnt diese gemäß dem aktuellen Produktionsvertrag ab, obwohl
Regularisierung mathematisch eine rangdefiziente Matrix behandeln könnte.
Diese Ablehnung ist eine geprüfte Implementierungsgrenze.

Verglichen werden komplexe Precoderkoeffizienten, gewünschte Leistung,
Inter-User-Interferenz und SINR; im Uplink zusätzlich die Rauschleistung nach
der Kombination. Downlink-Intra-User-Interferenz geht in die SINR-Prüfung
ein. Nutzer-, Subträger- und Stream-IDs müssen exakt übereinstimmen.
Für jeden numerischen Wert gilt:

```text
abs(native - matlab) <= 1e-10 + 1e-9 * abs(matlab)
```

Diese numerische Toleranz ist keine physikalische Fehlergrenze.

## Ergebnisse vom 28. Oktober 2026

Der gespeicherte Lauf befindet sich unter
[`data/mu_mimo_matlab_2026_10_28`](../data/mu_mimo_matlab_2026_10_28/).
`report.json` dokumentiert die Fehler jedes Falls, `provenance.json` enthält
den exakten Aufruf und SHA-256-Hashes der Eingaben, Ergebnisse, Quelltexte,
des Exportprogramms und der Sionna-Bibliothek. MATLAB-Version:
R2026a Update 4, 26.1.0.3312084.

| Prüfung | Ergebnis |
| --- | --- |
| Numerische Fälle | 229 bestanden |
| Erwartete Rangverlust-Ablehnungen | 4 bestanden |
| Nutzer-/Subträger-/Stream-Ergebniszeilen | 2266 |
| Numerische Einzelvergleiche | 14340 |
| Größte absolute SINR-Abweichung | 8,446932042716071e-11 |
| Größte absolute Abweichung über alle Größen | 6,984919309616089e-10 |
| Größter Fehler geteilt durch seine zulässige Toleranz | 0,0002614298425104097; Grenze 1 |

Der größte absolute Fehler über alle Größen liegt in einer Rauschleistung
nach ZF-Kombination von etwa 1000001 im schlecht konditionierten
Uplink-Fall. Die relative Abweichung bleibt entsprechend sehr klein.
Quelltexte, Exportprogramm und Sionna-Bibliothek blieben während des
gespeicherten Laufs unverändert.

In einem zusätzlichen Negativversuch wurde ausschließlich die erste native
SINR im Nah-Fern-ZF-Fall um +1 verändert. Die MATLAB-Prüfung brach wie
erwartet mit `MATLAB/native mismatch: ul_near_far_zf` und Exitcode 1 ab.
`negative_control.json`, das zugehörige Log und der gespeicherte
Export-Wrapper dokumentieren den Versuch. Damit wurde auch die Wirksamkeit
des Vergleichs geprüft.

Die vom MATLAB-Prozess an Kindprozesse vererbten Loader-Overrides
`LD_LIBRARY_PATH` und `LD_PRELOAD` werden ausschließlich für den
C++-Exportprozess entfernt. Dadurch verwendet er seine System-C++-Laufzeit
und seinen eingebetteten RUNPATH statt MATLABs älterer `libstdc++`.
An der MATLAB-Installation werden keine Einstellungen geändert.
