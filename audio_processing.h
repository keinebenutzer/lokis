#ifndef AUDIO_PROCESSING_H
#define AUDIO_PROCESSING_H

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

const int SAMPLE_RATE = 16000;
const int FRAME_SIZE = 512;
const int HOP_SIZE = 256;
const int WORD_FRAMES = 50;
const size_t BINS = FRAME_SIZE / 2 + 1;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Template {
    std::string name;
    std::vector<std::vector<double>> matrix;
};

// Lädt alle mathematischen Vorlagen aus der RAM-Disk
inline std::vector<Template> lade_katalog(const std::string& katalog_path) {
    std::vector<Template> katalog;
    if (!fs::exists(katalog_path)) {
        std::cerr << "🛑 CONFIG-FEHLER: Ordner " << katalog_path << " existiert nicht!\n" << std::flush;
        return katalog;
    }
    for (const auto& entry : fs::directory_iterator(katalog_path)) {
        if (entry.path().extension() == ".template") {
            std::ifstream in(entry.path().string(), std::ios::binary);
            if (!in.is_open()) continue;

            int frames; size_t bins;
            in.read(reinterpret_cast<char*>(&frames), sizeof(int));
            in.read(reinterpret_cast<char*>(&bins), sizeof(size_t));

            if (frames != WORD_FRAMES || bins != BINS) {
                std::cerr << "⚠️ Warnung: " << entry.path().filename() << " hat falsche Dimensionen. Ueberspringe.\n" << std::flush;
                continue;
            }

            Template t;
            t.name = entry.path().stem().string();
            t.matrix.resize(frames, std::vector<double>(bins));

            std::vector<float> row_buffer(bins);
            for (int f = 0; f < frames; ++f) {
                in.read(reinterpret_cast<char*>(row_buffer.data()), bins * sizeof(float));
                for(size_t b = 0; b < bins; ++b) {
                    t.matrix[f][b] = static_cast<double>(row_buffer[b]);
                }
            }
            katalog.push_back(t);
            std::cerr << "📂 Katalog geladen: " << t.name << " (" << frames << "x" << bins << ")" << std::endl;
        }
    }
    return katalog;
}

// Hilfsfunktion: Berechnet die Kosinus-Ähnlichkeit zwischen zwei einzelnen Frames
inline double kosinus_frame_dist(const std::vector<double>& live_frame, const std::vector<double>& temp_frame) {
    double dot_product = 0.0;
    double norm_live = 0.0;
    double norm_temp = 0.0;

    for (size_t b = 0; b < BINS; ++b) {
        dot_product += live_frame[b] * temp_frame[b];
        norm_live += live_frame[b] * live_frame[b];
        norm_temp += temp_frame[b] * temp_frame[b];
    }

    if (norm_live > 0.0 && norm_temp > 0.0) {
        return dot_product / (std::sqrt(norm_live) * std::sqrt(norm_temp));
    }
    return 0.0;
}

// NEU: Elastischer Zeitabgleich (DTW kombiniert mit Kosinus-Metrik)
inline double berechne_kosinus_match(const std::vector<std::vector<double>>& live, const Template& t) {
    // DTW-Kostenmatrix im RAM aufbauen (WORD_FRAMES x WORD_FRAMES)
    std::vector<std::vector<double>> dtw(WORD_FRAMES, std::vector<double>(WORD_FRAMES, 0.0));

    // Basis-Zustand initialisieren
    dtw[0][0] = kosinus_frame_dist(live[0], t.matrix[0]);

    for (int i = 1; i < WORD_FRAMES; ++i) {
        dtw[i][0] = dtw[i-1][0] + kosinus_frame_dist(live[i], t.matrix[0]);
        dtw[0][i] = dtw[0][i-1] + kosinus_frame_dist(live[0], t.matrix[i]);
    }

    // Das Gummiband-Verfahren berechnen (Sucht den optimalen Zeitpfad)
    for (int i = 1; i < WORD_FRAMES; ++i) {
        for (int j = 1; j < WORD_FRAMES; ++j) {
            double cost = kosinus_frame_dist(live[i], t.matrix[j]);
            // Finde das Maximum der vorherigen Pfade (da wir Ähnlichkeit maximieren wollen)
            double max_prev = std::max({dtw[i-1][j], dtw[i][j-1], dtw[i-1][j-1]});
            dtw[i][j] = cost + max_prev;
        }
    }

    // Normierter Score am Ende des Pfades (Wertebereich wieder sauber gegen 1.0 normiert)
    return dtw[WORD_FRAMES-1][WORD_FRAMES-1] / (WORD_FRAMES * 2.0);
}

#endif

