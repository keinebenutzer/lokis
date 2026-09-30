#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <fftw3.h>

// WAV-Header-Struktur zur sicheren Verifizierung der Original-Physik
struct WavHeader {
    char chunkId[4]; int chunkSize; char format[4]; char subchunk1Id[4];
    int subchunk1Size; short audioFormat; short numChannels; int sampleRate;
    int byteRate; short blockAlign; short bitsPerSample; char subchunk2Id[4];
    int subchunk2Size;
};

const int SAMPLE_RATE = 16000;
const int FRAME_SIZE = 512;    // 32ms Fenster für die FFT
const int HOP_SIZE = 256;      // 16ms Sprungweite (50% Überlappung)
const int WORD_FRAMES = 50;    // Ein Wort entspricht ca. 800ms (50 Frames)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Benutzung: " << argv[0] << " <training.wav> <ausgabe.template>\n";
        return 1;
    }

    std::ifstream wavFile(argv[1], std::ios::binary);
    if (!wavFile.is_open()) {
        std::cerr << "🛑 Fehler: Konnte " << argv[1] << " nicht oeffnen!\n";
        return 1;
    }

    WavHeader header;
    wavFile.read(reinterpret_cast<char*>(&header), sizeof(WavHeader));
    if (header.sampleRate != SAMPLE_RATE || header.numChannels != 1 || header.bitsPerSample != 16) {
        std::cerr << "🛑 Fehler: WAV muss zwingend 16000Hz, Mono, 16-Bit sein!\n";
        return 1;
    }

    // Rohe Audiodaten in den RAM laden
    size_t numSamples = header.subchunk2Size / sizeof(int16_t);
    std::vector<int16_t> rawAudio(numSamples);
    wavFile.read(reinterpret_cast<char*>(rawAudio.data()), header.subchunk2Size);
    wavFile.close();

    std::cout << "📋 Datei geladen. Samples: " << numSamples << " (" << numSamples/SAMPLE_RATE << " Sek.)\n";

    // 1. PEAK-DETEKTION & ISOLIERUNG (Wort-Grenzen finden)
    std::vector<size_t> wordStarts;
    long long globalEnergySum = 0;
    for (int16_t s : rawAudio) globalEnergySum += std::abs(s);
    double silenceThreshold = (globalEnergySum / numSamples) * 2.5; // Dynamische Schwelle

    bool inWord = false;
    for (size_t i = 0; i < numSamples - (WORD_FRAMES * HOP_SIZE); i += HOP_SIZE) {
        double frameEnergy = 0;
        for (int j = 0; j < FRAME_SIZE; ++j) frameEnergy += std::abs(rawAudio[i + j]);
        frameEnergy /= FRAME_SIZE;

        if (frameEnergy > silenceThreshold && !inWord) {
            wordStarts.push_back(i);
            inWord = true;
            i += SAMPLE_RATE; // 1 Sekunde überspringen, um dasselbe Wort nicht doppelt zu fangen
        } else if (frameEnergy < silenceThreshold) {
            inWord = false;
        }
    }

    std::cout << "🎯 " << wordStarts.size() << " Wort-Durchlaeufe automatisch isoliert.\n";
    if (wordStarts.empty()) {
        std::cerr << "🛑 Fehler: Keine klaren Worte gefunden. Zu leise aufgenommen?\n";
        return 1;
    }

    // 2. SPEKTRALANALYSE VIA FFTW3
    size_t bins = FRAME_SIZE / 2 + 1;
    std::vector<std::vector<double>> masterTemplate(WORD_FRAMES, std::vector<double>(bins, 0.0));

    // FIX: Explizite FFTW3-Zuweisungsfunktionen nutzen für korrekte Ausrichtung im RAM!
    double* fft_in = fftw_alloc_real(FRAME_SIZE);
    fftw_complex* fft_out = fftw_alloc_complex(bins);
    fftw_plan plan = fftw_plan_dft_r2c_1d(FRAME_SIZE, fft_in, fft_out, FFTW_ESTIMATE);

    // Alle gefundenen Wörter durch die FFT jagen und aufsummieren
    for (size_t startSample : wordStarts) {
        for (int f = 0; f < WORD_FRAMES; ++f) {
            size_t offset = startSample + (f * HOP_SIZE);
            
            // Hann-Fensterfunktion anwenden, um Artefakte zu verhindern
            for (int i = 0; i < FRAME_SIZE; ++i) {
                double window = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (FRAME_SIZE - 1)));
                fft_in[i] = rawAudio[offset + i] * window;
            }

            fftw_execute(plan);

            for (size_t b = 0; b < bins; ++b) {
                double real = fft_out[b][0];
                double imag = fft_out[b][1];
                double magnitude = std::sqrt(real * real + imag * imag);
                masterTemplate[f][b] += magnitude;
            }
        }
    }

    // Ressourcen freigeben
    fftw_destroy_plan(plan);
    fftw_free(fft_in);
    fftw_free(fft_out);

    // 3. MITTELWERT BERECHNEN (DURCHSCHNITT DER MATRIX)
    std::ofstream out(argv[2], std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "🛑 Fehler: Konnte Template nicht schreiben!\n";
        return 1;
    }

    // Header-Infos in das Template schreiben
    out.write(reinterpret_cast<const char*>(&WORD_FRAMES), sizeof(int));
    out.write(reinterpret_cast<const char*>(&bins), sizeof(size_t));

    for (int f = 0; f < WORD_FRAMES; ++f) {
        for (size_t b = 0; b < bins; ++b) {
            masterTemplate[f][b] /= wordStarts.size(); // Teilen durch Anzahl der Durchläufe
            float val = static_cast<float>(masterTemplate[f][b]);
            out.write(reinterpret_cast<char*>(&val), sizeof(float));
        }
    }
    out.close();

    std::cout << "💾 Erfolgreich exportiert nach: " << argv[2] << " (Größe: " 
              << (WORD_FRAMES * bins * sizeof(float)) / 1024 << " KB)\n";
    return 0;
}

