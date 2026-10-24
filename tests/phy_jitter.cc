// OFDM PHY through a timing-jitter channel.
//
// Encodes frames, passes the audio through a time-varying delay tau(t) (a measured trace
// from tools/jitter_trace.py, and/or a synthetic sine wobble and clock offset) plus AWGN,
// decodes, and counts intact frames. Used to compare PHY parameter sets against the
// clock wobble of crystal-less USB interfaces (design proposal Section 7.2).
//
// Usage: tests/phy_jitter [options]
//   --mode "QPSK 1/2 normal"   modulation, code rate, frame size (short/normal/long)
//   --trace FILE               tau(t) in microseconds, float32 at 1 kHz
//   --trace-start SEC          where in the trace to start (default 0)
//   --sine AMP_US,HZ           add a sine wobble
//   --ppm PPM                  add a constant clock offset
//   --step US,SEC              add a delay step of US microseconds every SEC seconds
//                              (first at SEC/2), like a sample slip in the interface
//   --snr DB                   AWGN, relative to the mean signal power (default 40)
//   --frames N                 (default 20)
//   --seed N
//   --hash                     print a hash of the transmitted audio and exit
//   --tx-wav FILE              write the transmitted audio (16-bit mono 48 kHz) and exit
//   --rx-wav FILE              decode FILE (e.g. the tx audio recorded through real hardware)
//                              instead of the simulated channel; use the same mode, profile,
//                              frames and seed as the --tx-wav run
//   --profile NAME             tone layout: legacy (default), n30, n60 (phy/common.hh)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "modem.hh"

static const int RATE = 48000;

struct Args {
    std::string mode = "QPSK 1/2 normal";
    std::string trace;
    double trace_start = 0, sine_amp_us = 0, sine_hz = 0, ppm = 0, snr = 40;
    double step_us = 0, step_every = 0;
    int frames = 20;
    unsigned seed = 1;
    bool hash_only = false;
    std::string profile = "legacy";
    std::string tx_wav, rx_wav;
};

static bool write_wav(const std::string& path, const std::vector<float>& x) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t n = (uint32_t)x.size(), rate = RATE, byte_rate = RATE * 2, data = n * 2, riff = 36 + data;
    uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    uint32_t fmt_len = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (float v : x) {
        int16_t q = (int16_t)std::lrint(std::max(-1.0f, std::min(1.0f, v)) * 32767);
        fwrite(&q, 2, 1, f);
    }
    return fclose(f) == 0;
}

// 16-bit mono PCM; skips any extra chunks
static bool read_wav(const std::string& path, std::vector<float>& x) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char id[4];
    uint32_t len;
    if (fread(id, 1, 4, f) != 4 || fread(&len, 4, 1, f) != 1 || fread(id, 1, 4, f) != 4) { fclose(f); return false; }
    while (fread(id, 1, 4, f) == 4 && fread(&len, 4, 1, f) == 1) {
        if (!memcmp(id, "data", 4)) {
            std::vector<int16_t> q(len / 2);
            size_t got = fread(q.data(), 2, q.size(), f);
            x.resize(got);
            for (size_t i = 0; i < got; ++i) x[i] = q[i] / 32768.0f;
            fclose(f);
            return true;
        }
        fseek(f, len + (len & 1), SEEK_CUR);
    }
    fclose(f);
    return false;
}

static int parse_mode(const std::string& s) {
    char mod[16] = {0}, rate[8] = {0}, size[8] = {0};
    if (sscanf(s.c_str(), "%15s %7s %7s", mod, rate, size) < 2) return -1;
    int fs = 1;
    if (!strcmp(size, "short")) fs = 0;
    else if (!strcmp(size, "long")) fs = 2;
    else if (!strcmp(size, "micro")) fs = 3;
    return ModemConfig::encode_mode(mod, rate, fs);
}

template <typename P> int run(const Args& a);

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", k.c_str()); exit(2); }
            return argv[++i];
        };
        if (k == "--mode") a.mode = next();
        else if (k == "--trace") a.trace = next();
        else if (k == "--trace-start") a.trace_start = atof(next());
        else if (k == "--sine") { if (sscanf(next(), "%lf,%lf", &a.sine_amp_us, &a.sine_hz) != 2) return 2; }
        else if (k == "--ppm") a.ppm = atof(next());
        else if (k == "--step") { if (sscanf(next(), "%lf,%lf", &a.step_us, &a.step_every) != 2) return 2; }
        else if (k == "--snr") a.snr = atof(next());
        else if (k == "--frames") a.frames = atoi(next());
        else if (k == "--seed") a.seed = (unsigned)atoi(next());
        else if (k == "--hash") a.hash_only = true;
        else if (k == "--profile") a.profile = next();
        else if (k == "--tx-wav") a.tx_wav = next();
        else if (k == "--rx-wav") a.rx_wav = next();
        else { fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }
    if (a.profile == "legacy") return run<OfdmLegacy>(a);
    if (a.profile == "n30") return run<OfdmNarrow30>(a);
    if (a.profile == "n60") return run<OfdmNarrow60>(a);
    fprintf(stderr, "unknown profile %s\n", a.profile.c_str());
    return 2;
}

template <typename P>
int run(const Args& a) {
    typedef ModemEncoder<float, DSP::Complex<float>, RATE, P> Encoder;
    typedef ModemDecoder<float, DSP::Complex<float>, RATE, P> Decoder;
    int oper_mode = parse_mode(a.mode);
    if (oper_mode < 0) { fprintf(stderr, "bad mode '%s'\n", a.mode.c_str()); return 2; }

    std::vector<float> trace;
    if (!a.trace.empty()) {
        FILE* f = fopen(a.trace.c_str(), "rb");
        if (!f) { perror(a.trace.c_str()); return 2; }
        float v;
        while (fread(&v, sizeof v, 1, f) == 1) trace.push_back(v);
        fclose(f);
        if (trace.size() < 2) { fprintf(stderr, "trace too short\n"); return 2; }
    }

    // Transmit: frames with random payloads separated by silence
    std::mt19937 rng(a.seed);
    auto enc = std::make_unique<Encoder>();
    int payload = Encoder::get_payload_size(oper_mode);
    int64_t call = ModemConfig::encode_callsign("N0SIM");
    std::vector<std::vector<uint8_t>> sent;
    std::vector<float> tx(RATE / 2, 0.0f);
    double sig_pow = 0;
    size_t sig_n = 0;
    for (int f = 0; f < a.frames; ++f) {
        std::vector<uint8_t> d(payload);
        for (auto& b : d) b = (uint8_t)rng();
        auto s = enc->encode(d.data(), d.size(), 1500, call, oper_mode);
        for (float v : s) { sig_pow += (double)v * v; ++sig_n; }
        tx.insert(tx.end(), s.begin(), s.end());
        tx.insert(tx.end(), RATE * 3 / 10, 0.0f);
        sent.push_back(std::move(d));
    }
    tx.insert(tx.end(), RATE, 0.0f);
    sig_pow /= std::max<size_t>(sig_n, 1);
    size_t bad = 0;
    for (float v : tx) bad += !std::isfinite(v);
    if (bad || sig_n == 0) {
        fprintf(stderr, "encoder produced %zu non-finite samples (%zu signal samples)\n", bad, sig_n);
        return 3;
    }
    if (a.hash_only) {
        // FNV-1a over the transmitted samples: proves a refactor leaves the waveform unchanged
        uint64_t h = 1469598103934665603ULL;
        for (float v : tx) {
            uint32_t u;
            memcpy(&u, &v, sizeof u);
            for (int b = 0; b < 4; ++b) { h ^= (u >> (8 * b)) & 0xFF; h *= 1099511628211ULL; }
        }
        printf("%-22s %zu samples  %016llx\n", a.mode.c_str(), tx.size(), (unsigned long long)h);
        return 0;
    }

    if (!a.tx_wav.empty()) {
        if (!write_wav(a.tx_wav, tx)) { perror(a.tx_wav.c_str()); return 2; }
        printf("%s: %zu samples, %.1f s\n", a.tx_wav.c_str(), tx.size(), (double)tx.size() / RATE);
        return 0;
    }

    // Channel: y(t) = x(t - tau(t)) + noise, cubic Lagrange interpolation
    auto tau_s = [&](double t) {
        double us = a.sine_amp_us * std::sin(2 * M_PI * a.sine_hz * t) + a.ppm * t;
        if (a.step_every > 0)
            us += a.step_us * std::floor(t / a.step_every + 0.5);
        if (!trace.empty()) {
            double p = (t + a.trace_start) * 1000.0;
            double n = (double)trace.size() - 1;
            p = std::fmod(p, n);
            size_t i = (size_t)p;
            double fr = p - i;
            us += trace[i] * (1 - fr) + trace[i + 1] * fr;
        }
        return us * 1e-6;
    };
    std::normal_distribution<double> gauss(0.0, std::sqrt(sig_pow / std::pow(10.0, a.snr / 10)));
    std::vector<float> rx(a.rx_wav.empty() ? tx.size() : 0);
    if (!a.rx_wav.empty() && !read_wav(a.rx_wav, rx)) { fprintf(stderr, "cannot read %s\n", a.rx_wav.c_str()); return 2; }
    for (size_t n = 0; a.rx_wav.empty() && n < rx.size(); ++n) {
        double t = (double)n / RATE;
        double x = n - tau_s(t) * RATE;
        long i = (long)std::floor(x);
        double u = x - i;
        auto at = [&](long k) { return (k >= 0 && k < (long)tx.size()) ? (double)tx[k] : 0.0; };
        double ym1 = at(i - 1), y0 = at(i), y1 = at(i + 1), y2 = at(i + 2);
        double v = y0 + u * (0.5 * (y1 - ym1) + u * (ym1 - 2.5 * y0 + 2 * y1 - 0.5 * y2 +
                   u * (1.5 * (y0 - y1) + 0.5 * (y2 - ym1))));
        rx[n] = (float)(v + gauss(rng));
    }

    // Receive
    auto dec = std::make_unique<Decoder>();
    int good = 0, other = 0;
    std::vector<bool> got(sent.size(), false);
    auto cb = [&](const uint8_t* d, size_t len) {
        for (size_t f = 0; f < sent.size(); ++f)
            if (!got[f] && len >= sent[f].size() && !memcmp(d, sent[f].data(), sent[f].size())) {
                got[f] = true; ++good; return;
            }
        ++other;
    };
    for (size_t n = 0; n < rx.size(); n += 480)
        dec->process(rx.data() + n, std::min<size_t>(480, rx.size() - n), cb);

    printf("%-6s %-22s snr %4.0f dB  %3d/%d frames intact  %d other  (payload %d B, %.2f s/frame)\n",
           a.profile.c_str(), a.mode.c_str(), a.snr, good, a.frames, other, payload,
           (double)(tx.size() - RATE / 2 - RATE) / RATE / a.frames - 0.3);
    return good == a.frames ? 0 : 1;
}
