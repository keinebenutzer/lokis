# LOKIS

LOKIS ist eine leichtgewichtige, triggerwortbasierte Spracherkennung für Linux. Erkannte Triggerwörter werden über eine FIFO-Schnittstelle zur Weiterverarbeitung an andere Programme ausgegeben.

LOKIS kann beispielsweise als Auslöser für Sprachsteuerungen oder andere Automatisierungen eingesetzt werden. Die Erkennung erfolgt lokal und benötigt keine GPU. Ein exklusiv verwendetes Mikrofon lauscht dabei permanent auf die zuvor erstellten Triggerwort-Schablonen.

## Voraussetzungen

FFTW installieren:

```bash
sudo apt update
sudo apt install libfftw3-dev
```

## Verzeichnisstruktur

Beispielsweise:

```text
lokis/
├── lokis
├── lokis.cpp
├── schablonen_macher
├── schablonen_macher.cpp
├── audio_processing.h
├── config.ini
├── schablonen/
├── runtime/
└── logs/
```

Benötigte Verzeichnisse anlegen:

```bash
mkdir -p schablonen runtime logs
```

## FIFO für die Ausgabe erstellen

LOKIS gibt erkannte Triggerwörter über eine FIFO-Datei aus.

Beispiel:

```bash
mkfifo ./runtime/lokis.signal
```

Die FIFO kann auch auf einer persistenten RAM-Disk angelegt werden. In diesem Fall muss der entsprechende Pfad in der `config.ini` eingetragen werden.

## Programme kompilieren

Schablonen-Macher:

```bash
g++ -O3 schablonen_macher.cpp -o schablonen_macher -lfftw3
```

LOKIS:

```bash
g++ -O3 -std=c++17 lokis.cpp -o lokis -lfftw3
```

## Mikrofon ermitteln

PipeWire-Geräte anzeigen:

```bash
wpctl status
```

Beispiel:

```text
Sources:
  * 57. Built-in Audio Analoges Stereo [vol: 0.37]
```

Den PipeWire-Namen der ID `57` auslesen:

```bash
pw-cli info 57 | grep node.name
```

Beispiel:

```text
node.name = "alsa_input.pci-0000_00_1f.3.analog-stereo"
```

Alternativ können die Aufnahmequellen mit folgendem Befehl angezeigt werden:

```bash
pactl list short sources
```

## config.ini anlegen

```bash
vim config.ini
```

Beispiel:

```ini
[lokis]
device=alsa_input.pci-0000_00_1f.3.analog-stereo
templates=./schablonen
output=./runtime/lokis.signal

[erkennung]
noise_margin=0.05
# Optional kann anstelle der automatischen Ermittlung
# ein fester Schwellenwert verwendet werden:
# threshold=0.75

[debug]
enabled=false
file=./logs/lokis_statistik.csv
```

Relative Pfade werden relativ zum Verzeichnis der `config.ini` aufgelöst. Dadurch kann das komplette LOKIS-Verzeichnis verschoben werden, ohne Pfade im Quellcode ändern oder LOKIS neu kompilieren zu müssen.

## Parameter der config.ini

**device**

PipeWire-Aufnahmegerät, das LOKIS exklusiv für die Erkennung verwendet.

**templates**

Verzeichnis mit den `.template`-Dateien der Triggerwörter.

**output**

FIFO-Datei, über die erkannte Triggerwörter ausgegeben werden.

**noise_margin**

Abstand zum höchsten während der Kalibrierung gemessenen Rausch-Score.

Ist beispielsweise der höchste Rausch-Score `0.71` und `noise_margin=0.05`, ergibt sich ein Schwellenwert von `0.76`.

**threshold**

Optionaler fester Schwellenwert zwischen `0.0` und `1.0`.

Ist kein `threshold` konfiguriert, ermittelt LOKIS den Schwellenwert automatisch aus dem gemessenen Umgebungsgeräusch und `noise_margin`.

**debug**

Aktiviert oder deaktiviert das Statistik-Logging:

```ini
enabled=false
```

oder:

```ini
enabled=true
```

**file**

Ausgabedatei für das Statistik-Logging.

## Triggerwort aufnehmen

Beispielsweise soll das Triggerwort „Computer“ eingerichtet werden.

Aufnahme starten:

```bash
pw-record \
  --target="alsa_input.pci-0000_00_1f.3.analog-stereo" \
  --format=s16 \
  --rate=16000 \
  --channels=1 \
  ./computer.wav
```

Während der Aufnahme das Wort „Computer“ etwa zehnmal in normaler Lautstärke aussprechen.

Anschließend die Aufnahme mit `Ctrl+C` beenden.

## Template erzeugen

Aus der WAV-Datei wird die Schablone für das Triggerwort erzeugt:

```bash
./schablonen_macher computer.wav ./schablonen/computer.template
```

Für jedes weitere Triggerwort wird eine eigene WAV-Datei aufgenommen und daraus eine eigene `.template`-Datei erzeugt.

Der Speicherort des Template-Verzeichnisses ist frei wählbar. Entscheidend ist lediglich, dass in der `config.ini` unter `templates` dasselbe Verzeichnis angegeben wird.

## LOKIS starten

Mit der `config.ini`:

```bash
./lokis -i config.ini
```

### Schwellenwert temporär überschreiben

Ein in der `config.ini` konfigurierter oder automatisch ermittelter Schwellenwert kann für einen einzelnen Start überschrieben werden:

```bash
./lokis -i config.ini -s 0.82
```

Für den Schwellenwert gilt folgende Priorität:

```text
1. -s / --threshold
2. threshold aus config.ini
3. automatische Kalibrierung aus Rausch-Score + noise_margin
```

Damit bleibt die automatische Anpassung an unterschiedliche Umgebungsgeräusche der Standard.

## Kommandozeilenoptionen

```text
Verwendung: ./lokis [Optionen]

  -i, --ini DATEI          Konfiguration aus INI-Datei laden
  -d, --device NAME        PipeWire-Aufnahmegeraet
  -p, --path VERZEICHNIS  Verzeichnis der .template-Dateien
      --templates VERZ.    Alias fuer --path
  -o, --output FIFO        Ausgangs-FIFO fuer erkannte Woerter
  -s, --threshold WERT     Fester Trigger-Schwellwert 0.0 bis 1.0
      --noise-margin WERT  Abstand ueber gemessenem Rausch-Score
      --debug              Statistik-Logging aktivieren
      --stats DATEI        Statistikdatei, aktiviert Debug-Logging
  -h, --help               Diese Hilfe anzeigen
```
