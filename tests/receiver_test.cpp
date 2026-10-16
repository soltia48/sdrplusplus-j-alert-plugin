// Regression tests for Receiver::reset().
//
// A synthetic J-ALERT downlink (HDLC frames → IESS-308 scrambler → double
// differential encode → K=7 conv code → BPSK → RRC, 4 sps at the 1.024 MHz work
// rate, small carrier offset and noise) is decoded by a fresh receiver and by
// receivers that were reset after processing. reset() must make a receiver
// behave exactly like a newly constructed one, even after non-finite samples
// reached the DSP state.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

#include "decode/receiver.h"
#include "dsp/filter_design.h"
#include "fec/conv_code.h"

using jalert::decode::DecodedAlert;
using jalert::decode::Receiver;
using jalert::dsp::BpskDemod;
using jalert::dsp::Complex32;

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

constexpr int kFrames = 300;
constexpr int kPayloadBytes = 48;
constexpr int kSps = BpskDemod::kSps;
constexpr double kPi = 3.14159265358979323846;

// ── Transmitter ───────────────────────────────────────────────────────────────

uint16_t crc_x25(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408)
                            : static_cast<uint16_t>(crc >> 1);
    }
    return static_cast<uint16_t>(~crc);
}

void push_flags(std::vector<uint8_t>& bits, int n) {
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 8; ++k) bits.push_back((0x7E >> k) & 1);
}

// HDLC bit stream (LSB first, zero-stuffed, flag separated) of kFrames frames.
std::vector<uint8_t> hdlc_bits(std::mt19937& rng) {
    std::vector<uint8_t> bits;
    push_flags(bits, 64);   // idle while the receiver acquires
    for (int f = 0; f < kFrames; ++f) {
        std::vector<uint8_t> frame(kPayloadBytes);
        for (auto& b : frame) b = static_cast<uint8_t>(rng());
        const uint16_t fcs = crc_x25(frame);
        frame.push_back(static_cast<uint8_t>(fcs & 0xFF));
        frame.push_back(static_cast<uint8_t>(fcs >> 8));
        int ones = 0;
        for (uint8_t byte : frame) {
            for (int k = 0; k < 8; ++k) {
                const uint8_t b = (byte >> k) & 1;
                bits.push_back(b);
                ones = b ? ones + 1 : 0;
                if (ones == 5) { bits.push_back(0); ones = 0; }
            }
        }
        push_flags(bits, 2);
    }
    push_flags(bits, 64);   // flush the Viterbi traceback
    return bits;
}

// Inverse of the receive chain's descrambler and double differential decode.
std::vector<uint8_t> scramble_and_diff_encode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> s(in.size()), x(in.size());
    for (size_t n = 0; n < in.size(); ++n) {
        uint8_t v = in[n];
        if (n >= 3)  v ^= s[n - 3];
        if (n >= 20) v ^= s[n - 20];
        s[n] = v;
        x[n] = static_cast<uint8_t>(s[n] ^ 1u ^ (n >= 2 ? x[n - 2] : 0u));
    }
    return x;
}

std::vector<Complex32> modulate(const std::vector<uint8_t>& coded, std::mt19937& rng) {
    const auto taps = jalert::dsp::rrc_taps(static_cast<float>(kSps),
                                           static_cast<float>(BpskDemod::kWorkRate),
                                           BpskDemod::kSymRate, 0.35f, 12 * kSps + 1);
    std::vector<float> up(coded.size() * kSps, 0.0f);
    for (size_t i = 0; i < coded.size(); ++i) up[i * kSps] = coded[i] ? -1.0f : 1.0f;

    std::normal_distribution<float> noise(0.0f, 0.05f);
    const double w = 2.0 * kPi * 1500.0 / BpskDemod::kWorkRate;   // 1.5 kHz offset
    std::vector<Complex32> out(up.size());
    for (size_t n = 0; n < up.size(); ++n) {
        float acc = 0.0f;
        for (size_t t = 0; t < taps.size() && t <= n; ++t) acc += taps[t] * up[n - t];
        const double ph = w * static_cast<double>(n) + 0.7;
        out[n] = Complex32(static_cast<float>(0.5 * acc * std::cos(ph)) + noise(rng),
                           static_cast<float>(0.5 * acc * std::sin(ph)) + noise(rng));
    }
    return out;
}

std::vector<Complex32> make_signal() {
    std::mt19937 rng(1234);
    const auto coded = jalert::fec::conv_encode(scramble_and_diff_encode(hdlc_bits(rng)));
    return modulate(coded, rng);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

struct Result {
    long long frames_ok;
    long long frames_bad;
    float ber;
    float sym_mag;
};

Result run(Receiver& r, const std::vector<Complex32>& sig) {
    for (const Complex32& z : sig) r.process(z);
    return {r.frames_ok(), r.frames_bad(), r.ber(), r.demod().sym_mag()};
}

bool same(const Result& a, const Result& b) {
    return a.frames_ok == b.frames_ok && a.frames_bad == b.frames_bad &&
           a.ber == b.ber && a.sym_mag == b.sym_mag;
}

void print(const char* what, const Result& r) {
    std::printf("%-24s frames ok %lld bad %lld  ber %.5f  sym_mag %.4f\n", what,
                r.frames_ok, r.frames_bad, r.ber, r.sym_mag);
}

} // namespace

int main() {
    const auto sig = make_signal();

    Receiver fresh(BpskDemod::kWorkRate, [](const DecodedAlert&) {});
    const Result want = run(fresh, sig);
    print("fresh receiver", want);
    CHECK(want.frames_ok >= kFrames * 9 / 10);

    // Reset after normal operation.
    {
        Receiver r(BpskDemod::kWorkRate, [](const DecodedAlert&) {});
        run(r, sig);
        r.reset();
        const Result got = run(r, sig);
        print("reset after decoding", got);
        CHECK(same(got, want));
    }

    // Reset after non-finite samples reached the DSP state. Receiver::process
    // sanitises its input, so feed the demodulator directly, as a NaN produced
    // inside the chain would.
    {
        Receiver r(BpskDemod::kWorkRate, [](const DecodedAlert&) {});
        const std::vector<Complex32> head(sig.begin(), sig.begin() + sig.size() / 4);
        run(r, head);
        auto& demod = const_cast<BpskDemod&>(r.demod());
        std::vector<float> softs;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (int i = 0; i < 8; ++i) demod.process(Complex32(nan, nan), softs);
        r.reset();
        const Result got = run(r, sig);
        print("reset after NaN", got);
        CHECK(std::isfinite(got.sym_mag));
        CHECK(same(got, want));
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all receiver tests passed");
    return 0;
}
