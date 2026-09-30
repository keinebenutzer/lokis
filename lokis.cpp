#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstring>
#include <sys/wait.h>
#include <string>
#include <csignal>
#include <cerrno>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <cctype>
#include <fftw3.h>

// Unser Mathe-Kern
#include "audio_processing.h"

namespace fs = std::filesystem;

struct Config {
    std::string device_name;
    std::string katalog_path;
    std::string output_pipe;
    bool debug = false;
    std::string stats_file;
    double noise_margin = 0.05;
    std::optional<double> threshold;
    std::string threshold_quelle = "automatisch";
};

static std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

static std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

static bool parse_bool(const std::string& value, bool& result) {
    const std::string v = lower(trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "ja" || v == "on") {
        result = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "nein" || v == "off") {
        result = false;
        return true;
    }
    return false;
}

static bool parse_double(const std::string& value, double& result) {
    try {
        size_t pos = 0;
        const double parsed = std::stod(trim(value), &pos);
        if (pos != trim(value).size()) return false;
        result = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

static std::string resolve_ini_path(const fs::path& ini_file, const std::string& value) {
    if (value.empty()) return value;
    fs::path p(value);
    if (p.is_absolute()) return p.lexically_normal().string();
    return (ini_file.parent_path() / p).lexically_normal().string();
}

static bool lade_ini(const std::string& ini_path, Config& config) {
    std::ifstream in(ini_path);
    if (!in) {
        std::cerr << "START-FEHLER: INI-Datei kann nicht geoeffnet werden: " << ini_path << std::endl;
        return false;
    }

    const fs::path ini_file = fs::absolute(fs::path(ini_path));
    std::string section;
    std::string line;
    size_t line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            std::cerr << "INI-FEHLER: Ungueltige Zeile " << line_no << ": " << line << std::endl;
            return false;
        }

        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        const std::string full_key = section.empty() ? key : section + "." + key;

        if (full_key == "lokis.device" || key == "device") {
            config.device_name = value;
        } else if (full_key == "lokis.templates" || full_key == "lokis.katalog" ||
                   key == "templates" || key == "katalog" || key == "path") {
            config.katalog_path = resolve_ini_path(ini_file, value);
        } else if (full_key == "lokis.output" || full_key == "lokis.pipe" ||
                   key == "output" || key == "pipe") {
            config.output_pipe = resolve_ini_path(ini_file, value);
        } else if (full_key == "debug.enabled" || key == "debug") {
            bool parsed = false;
            if (!parse_bool(value, parsed)) {
                std::cerr << "INI-FEHLER: Ungueltiger Bool-Wert in Zeile " << line_no << std::endl;
                return false;
            }
            config.debug = parsed;
        } else if (full_key == "debug.file" || full_key == "debug.stats_file" ||
                   key == "stats_file" || key == "log_file") {
            config.stats_file = resolve_ini_path(ini_file, value);
        } else if (full_key == "erkennung.threshold" || key == "threshold") {
            double parsed = 0.0;
            if (!parse_double(value, parsed) || parsed < 0.0 || parsed > 1.0) {
                std::cerr << "INI-FEHLER: threshold muss zwischen 0.0 und 1.0 liegen, Zeile " << line_no << std::endl;
                return false;
            }
            config.threshold = parsed;
            config.threshold_quelle = "config.ini";
        } else if (full_key == "erkennung.noise_margin" || key == "noise_margin") {
            double parsed = 0.0;
            if (!parse_double(value, parsed) || parsed < 0.0 || parsed > 1.0) {
                std::cerr << "INI-FEHLER: noise_margin muss zwischen 0.0 und 1.0 liegen, Zeile " << line_no << std::endl;
                return false;
            }
            config.noise_margin = parsed;
        }
    }

    return true;
}

static void zeige_hilfe(const char* programm) {
    std::cerr
        << "Verwendung: " << programm << " [Optionen]\n\n"
        << "  -i, --ini DATEI          Konfiguration aus INI-Datei laden\n"
        << "  -d, --device NAME        PipeWire-Aufnahmegeraet\n"
        << "  -p, --path VERZEICHNIS  Verzeichnis der .template-Dateien\n"
        << "      --templates VERZ.   Alias fuer --path\n"
        << "  -o, --output FIFO        Ausgangs-FIFO fuer erkannte Woerter\n"
        << "  -s, --threshold WERT     Fester Trigger-Schwellwert 0.0 bis 1.0\n"
        << "      --noise-margin WERT  Abstand ueber gemessenem Rausch-Score\n"
        << "      --debug              Statistik-Logging aktivieren\n"
        << "      --stats DATEI        Statistikdatei, aktiviert Debug-Logging\n"
        << "  -h, --help               Diese Hilfe anzeigen\n\n"
        << "Prioritaet fuer threshold: -s/--threshold > config.ini > automatische Kalibrierung\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    Config config;

    // INI zuerst ermitteln und laden. CLI-Werte werden danach bewusst als Override angewendet.
    std::string ini_path;
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-i") == 0 || std::strcmp(argv[i], "--ini") == 0)) {
            if (i + 1 >= argc) {
                std::cerr << "START-FEHLER: " << argv[i] << " benoetigt einen Dateipfad." << std::endl;
                return 1;
            }
            ini_path = argv[++i];
        }
    }

    if (!ini_path.empty() && !lade_ini(ini_path, config)) return 1;

    // Kommandozeile ueberschreibt immer die INI-Konfiguration.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-i" || arg == "--ini") {
            ++i;
        } else if (arg == "-d" || arg == "--device") {
            if (i + 1 >= argc) {
                std::cerr << "START-FEHLER: " << arg << " benoetigt einen Wert." << std::endl;
                return 1;
            }
            config.device_name = argv[++i];
        } else if (arg == "-p" || arg == "--path" || arg == "--templates") {
            if (i + 1 >= argc) {
                std::cerr << "START-FEHLER: " << arg << " benoetigt einen Wert." << std::endl;
                return 1;
            }
            config.katalog_path = argv[++i];
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "START-FEHLER: " << arg << " benoetigt einen Wert." << std::endl;
                return 1;
            }
            config.output_pipe = argv[++i];
        } else if (arg == "-s" || arg == "--threshold") {
            if (i + 1 >= argc) {
                std::cerr << "START-FEHLER: " << arg << " benoetigt einen Wert." << std::endl;
                return 1;
            }
            double parsed = 0.0;
            if (!parse_double(argv[++i], parsed) || parsed < 0.0 || parsed > 1.0) {
                std::cerr << "START-FEHLER: threshold muss zwischen 0.0 und 1.0 liegen." << std::endl;
                return 1;
            }
            config.threshold = parsed;
            config.threshold_quelle = "Kommandozeile";
        } else if (arg == "--noise-margin") {
            if (i + 1 >= argc) {
                std::cerr << "START-FEHLER: --noise-margin benoetigt einen Wert." << std::endl;
                return 1;
            }
            double parsed = 0.0;
            if (!parse_double(argv[++i], parsed) || parsed < 0.0 || parsed > 1.0) {
                std::cerr << "START-FEHLER: noise-margin muss zwischen 0.0 und 1.0 liegen." << std::endl;
                return 1;
            }
            config.noise_margin = parsed;
        } else if (arg == "--debug") {
            config.debug = true;
        } else if (arg == "--stats") {
            if (i + 1 >= argc) {
                std::cerr << "START-FEHLER: --stats benoetigt einen Dateipfad." << std::endl;
                return 1;
            }
            config.stats_file = argv[++i];
            config.debug = true;
        } else if (arg == "-h" || arg == "--help") {
            zeige_hilfe(argv[0]);
            return 0;
        } else {
            std::cerr << "START-FEHLER: Unbekannte Option: " << arg << std::endl;
            zeige_hilfe(argv[0]);
            return 1;
        }
    }

    if (config.device_name.empty()) {
        std::cerr << "START-FEHLER: Kein Aufnahmegeraet konfiguriert. Nutze -d/--device oder config.ini." << std::endl;
        return 1;
    }
    if (config.katalog_path.empty()) {
        std::cerr << "START-FEHLER: Kein Schablonenverzeichnis konfiguriert. Nutze -p/--path oder config.ini." << std::endl;
        return 1;
    }
    if (config.output_pipe.empty()) {
        std::cerr << "START-FEHLER: Kein Ausgangs-FIFO konfiguriert. Nutze -o/--output oder config.ini." << std::endl;
        return 1;
    }
    if (config.debug && config.stats_file.empty()) {
        std::cerr << "START-FEHLER: Debug ist aktiviert, aber keine Statistikdatei konfiguriert." << std::endl;
        return 1;
    }

    auto katalog = lade_katalog(config.katalog_path);
    if (katalog.empty()) {
        std::cerr << "START-FEHLER: Keine passenden .template Dateien gefunden in: " << config.katalog_path << std::endl;
        return 1;
    }

    // PipeWire-Kanal vorbereiten
    int audio_pipe[2];
    if (pipe(audio_pipe) < 0) {
        std::cerr << "UNIX-FEHLER: Interner POSIX-Kanal fehlgeschlagen!" << std::endl;
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(audio_pipe[0]);
        dup2(audio_pipe[1], STDOUT_FILENO);
        close(audio_pipe[1]);
        std::string target_arg = "--target=" + config.device_name;
        execlp("pw-record", "pw-record", target_arg.c_str(), "--format=s16", "--rate=16000", "--channels=1", "-", (char*)NULL);
        exit(1);
    }

    close(audio_pipe[1]);
    int fd_in = audio_pipe[0];

    // --- MATHEMATISCHE EINMESSUNG STARTEN ---
    std::cerr << "\n[KALIBRIERUNG] Bitte fuer 3 Sekunden SCHWEIGEN. Messe Industrie-Hintergrund..." << std::endl;

    std::vector<std::vector<double>> live_matrix(WORD_FRAMES, std::vector<double>(BINS, 0.0));
    std::vector<char> accumulation_buffer;
    const size_t expected_bytes = HOP_SIZE * sizeof(int16_t);
    std::vector<char> read_buffer(expected_bytes);

    double* fft_in = fftw_alloc_real(FRAME_SIZE);
    fftw_complex* fft_out = fftw_alloc_complex(BINS);
    fftw_plan plan = fftw_plan_dft_r2c_1d(FRAME_SIZE, fft_in, fft_out, FFTW_ESTIMATE);

    std::vector<int16_t> current_window(FRAME_SIZE, 0);

    double max_noise_pegel = 0.0;
    double max_noise_score = 0.0;
    auto start_cal = std::chrono::steady_clock::now();

    // 3 Sekunden lang rein den Leerlauf analysieren
    while (std::chrono::steady_clock::now() - start_cal < std::chrono::seconds(3)) {
        ssize_t bytes_read = read(fd_in, read_buffer.data(), read_buffer.size());
        if (bytes_read > 0) {
            accumulation_buffer.insert(accumulation_buffer.end(), read_buffer.begin(), read_buffer.begin() + bytes_read);
            while (accumulation_buffer.size() >= expected_bytes) {
                std::move(current_window.begin() + HOP_SIZE, current_window.end(), current_window.begin());
                std::memcpy(&current_window[FRAME_SIZE - HOP_SIZE], accumulation_buffer.data(), expected_bytes);

                double pegel = 0.0;
                for (int i = FRAME_SIZE - HOP_SIZE; i < FRAME_SIZE; ++i) pegel += std::abs(current_window[i]);
                pegel /= HOP_SIZE;
                if (pegel > max_noise_pegel) max_noise_pegel = pegel;

                for (int i = 0; i < FRAME_SIZE; ++i) {
                    fft_in[i] = current_window[i] * (0.5 * (1.0 - std::cos(2.0 * M_PI * i / (FRAME_SIZE - 1))));
                }
                fftw_execute(plan);

                std::rotate(live_matrix.begin(), live_matrix.begin() + 1, live_matrix.end());
                for (size_t b = 0; b < BINS; ++b) {
                    live_matrix[WORD_FRAMES - 1][b] = std::sqrt(fft_out[b][0]*fft_out[b][0] + fft_out[b][1]*fft_out[b][1]);
                }

                for (const auto& t : katalog) {
                    double score = berechne_kosinus_match(live_matrix, t);
                    if (score > max_noise_score) max_noise_score = score;
                }
                accumulation_buffer.erase(accumulation_buffer.begin(), accumulation_buffer.begin() + expected_bytes);
            }
        }
    }

    // Pegelschwelle bleibt immer automatisch. Die Score-Schwelle kann bewusst ueberschrieben werden.
    double auto_pegel_threshold = max_noise_pegel * 1.5;
    double auto_score_threshold = max_noise_score + config.noise_margin;
    if (auto_score_threshold > 0.95) auto_score_threshold = 0.90; // Bestehender Begrenzungsschutz

    const double aktiver_score_threshold = config.threshold.value_or(auto_score_threshold);

    std::cerr << "KALIBRIERUNG BEENDET!" << std::endl;
    std::cerr << "Gemessenes Grundrauschen: Max-Pegel=" << int(max_noise_pegel)
              << " | Max-Score=" << int(max_noise_score * 100) << "%" << std::endl;
    std::cerr << "Pegel-Limit=" << int(auto_pegel_threshold) << std::endl;
    if (config.threshold.has_value()) {
        std::cerr << "Trigger-Limit=" << int(aktiver_score_threshold * 100)
                  << "% [" << config.threshold_quelle << "]\n" << std::endl;
    } else {
        std::cerr << "Trigger-Limit=" << int(aktiver_score_threshold * 100)
                  << "% [automatisch: Max-Score + " << config.noise_margin << "]\n" << std::endl;
    }

    // Jetzt erst die Ausgangs-Pipe oeffnen
    std::cerr << "Warte auf externen Empfaenger an " << config.output_pipe << "..." << std::endl;
    int fd_out = open(config.output_pipe.c_str(), O_WRONLY | O_NONBLOCK);

    std::ofstream log_file;
    if (config.debug) {
        log_file.open(config.stats_file);
        if (!log_file) {
            std::cerr << "START-FEHLER: Statistikdatei kann nicht geoeffnet werden: " << config.stats_file << std::endl;
            kill(pid, SIGTERM);
            waitpid(pid, NULL, 0);
            close(fd_in);
            if (fd_out >= 0) close(fd_out);
            fftw_destroy_plan(plan);
            fftw_free(fft_in);
            fftw_free(fft_out);
            return 1;
        }
        log_file << "Zeitstempel;Wort;MatchProzent;MaxPegel;FFTEnergie\n" << std::flush;
        std::cerr << "Debug-Logging aktiv: " << config.stats_file << std::endl;
    }

    std::cerr << "SYSTEM LIVE. Lausche auf Katalog-Worte..." << std::endl;
    auto last_trigger_time = std::chrono::steady_clock::now();
    auto last_log_time = std::chrono::steady_clock::now();
    double stat_max_pegel = 0.0;
    double stat_fft_energie = 0.0;
    long long stat_rohe_bytes = 0;

    // --- DIE LIVE SCHLEIFE NUTZT DIE AKTIVE SCHWELLE ---
    while (true) {
        ssize_t bytes_read = read(fd_in, read_buffer.data(), read_buffer.size());
        if (bytes_read > 0) {
            stat_rohe_bytes += bytes_read;
            accumulation_buffer.insert(accumulation_buffer.end(), read_buffer.begin(), read_buffer.begin() + bytes_read);

            while (accumulation_buffer.size() >= expected_bytes) {
                std::move(current_window.begin() + HOP_SIZE, current_window.end(), current_window.begin());
                std::memcpy(&current_window[FRAME_SIZE - HOP_SIZE], accumulation_buffer.data(), expected_bytes);

                double current_pegel = 0.0;
                for (int i = FRAME_SIZE - HOP_SIZE; i < FRAME_SIZE; ++i) current_pegel += std::abs(current_window[i]);
                current_pegel /= HOP_SIZE;
                if (current_pegel > stat_max_pegel) stat_max_pegel = current_pegel;

                for (int i = 0; i < FRAME_SIZE; ++i) {
                    fft_in[i] = current_window[i] * (0.5 * (1.0 - std::cos(2.0 * M_PI * i / (FRAME_SIZE - 1))));
                }
                fftw_execute(plan);

                std::rotate(live_matrix.begin(), live_matrix.begin() + 1, live_matrix.end());
                double current_fft_sum = 0.0;
                for (size_t b = 0; b < BINS; ++b) {
                    double magnitude = std::sqrt(fft_out[b][0]*fft_out[b][0] + fft_out[b][1]*fft_out[b][1]);
                    live_matrix[WORD_FRAMES - 1][b] = magnitude;
                    current_fft_sum += magnitude;
                }
                stat_fft_energie = current_fft_sum / BINS;

                auto now = std::chrono::steady_clock::now();
                double bester_score_im_durchlauf = 0.0;
                std::string bestes_wort_im_durchlauf = "keines";

                // Abgleich findet nur statt, wenn die Lautstaerke das gemessene Grundrauschen bricht
                if (stat_max_pegel > auto_pegel_threshold) {
                    for (const auto& t : katalog) {
                        double score = berechne_kosinus_match(live_matrix, t);
                        if (score > bester_score_im_durchlauf) {
                            bester_score_im_durchlauf = score;
                            bestes_wort_im_durchlauf = t.name;
                        }

                        if (score > aktiver_score_threshold && (now - last_trigger_time > std::chrono::milliseconds(2000))) {
                            std::cerr << "\nTRIGGER ERKANNT: '" << t.name << "' (" << score * 100.0 << "%)" << std::endl;
                            if (fd_out >= 0) {
                                std::string signal = t.name + "\n";
                                ssize_t written = write(fd_out, signal.c_str(), signal.length());
                                if (written < 0) {
                                    close(fd_out);
                                    fd_out = open(config.output_pipe.c_str(), O_WRONLY | O_NONBLOCK);
                                }
                            }
                            last_trigger_time = now;
                            break;
                        }
                    }
                }

                if (config.debug && now - last_log_time > std::chrono::milliseconds(250)) {
                    log_file << std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() << ";"
                             << bestes_wort_im_durchlauf << ";"
                             << int(bester_score_im_durchlauf * 100.0) << ";"
                             << int(stat_max_pegel) << ";"
                             << int(stat_fft_energie) << "\n" << std::flush;
                    stat_max_pegel *= 0.5;
                    last_log_time = now;
                }
                accumulation_buffer.erase(accumulation_buffer.begin(), accumulation_buffer.begin() + expected_bytes);
            }
        } else if (bytes_read == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(2000); continue; }
            break;
        }
    }

    fftw_destroy_plan(plan);
    fftw_free(fft_in);
    fftw_free(fft_out);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    close(fd_in);
    if (fd_out >= 0) close(fd_out);
    if (log_file.is_open()) log_file.close();

    return 0;
}
