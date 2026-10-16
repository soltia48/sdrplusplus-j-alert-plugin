#pragma once
// Top-level J-ALERT receiver: complex IQ → decoded alerts.
//
// The convolutional code is consumed in symbol pairs, so the decoder has a
// one-symbol pairing ambiguity. Rather than guess, we run two Viterbi+pipeline
// chains in parallel — one starting on each pairing phase — and let the HDLC
// CRC decide: the wrong phase produces only
// garbage that fails the FCS, so only the correctly-aligned chain ever emits a
// packet. (Carrier-phase / bit-polarity ambiguity is already handled by the
// differential decode, so two chains suffice — no need to also try inversions.)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "decode/alert.h"
#include "decode/bit_pipeline.h"
#include "dsp/bpsk_demod.h"
#include "fec/viterbi.h"

namespace jalert::decode {

class Receiver {
public:
    using AlertSink = std::function<void(const DecodedAlert&)>;

    Receiver(double sample_rate_hz, AlertSink sink)
        : demod_(sample_rate_hz),
          pipe0_(sink),
          pipe1_(sink) {}

    void process(dsp::Complex32 z) {
        // A single NaN/Inf sample would poison the loop states.
        if (!std::isfinite(z.re) || !std::isfinite(z.im)) z = dsp::Complex32();
        softs_.clear();
        demod_.process(z, softs_);
        for (float s : softs_) {
            // Watchdog: if the estimated BER of both Viterbi chains stays above
            // kDeadBer for kDeadSymbols, the loops may have drifted/false-locked
            // (e.g. after a signal gap). Restart acquisition; a genuine lock
            // re-forms within ms. Only BER is checked, not HDLC frames, so a
            // low-BER state that yields no valid frames is not detected.
            if (ber() > kDeadBer) {
                if (++dead_symbols_ >= kDeadSymbols) {
                    demod_.reset_loops();
                    dead_symbols_ = 0;
                    loop_resets_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                dead_symbols_ = 0;
            }
            bits_.clear();
            vit0_.push(s, bits_);
            for (uint8_t b : bits_) pipe0_.push_bit(b);

            if (!skipped_) { skipped_ = true; continue; }  // off=1 pairing
            bits_.clear();
            vit1_.push(s, bits_);
            for (uint8_t b : bits_) pipe1_.push_bit(b);
        }
    }

    void reset() {
        demod_.reset();
        vit0_.reset();
        vit1_.reset();
        pipe0_.reset();
        pipe1_.reset();
        skipped_ = false;
        dead_symbols_ = 0;
        loop_resets_.store(0, std::memory_order_relaxed);
    }

    const dsp::BpskDemod& demod() const { return demod_; }

    // Estimated coded BER. The wrong pairing phase decodes noise (elevated BER),
    // so the live chain's quality is the lower of the two.
    float ber() const { return std::min(vit0_.ber(), vit1_.ber()); }

    long long frames_ok()      const { return pipe0_.frames_ok()      + pipe1_.frames_ok(); }
    long long frames_bad()     const { return pipe0_.frames_bad()     + pipe1_.frames_bad(); }
    long long packets()        const { return pipe0_.packets()        + pipe1_.packets(); }
    long long status_packets() const { return pipe0_.status_packets() + pipe1_.status_packets(); }
    long long files()          const { return pipe0_.files()          + pipe1_.files(); }
    long long alerts()         const { return pipe0_.alerts()         + pipe1_.alerts(); }
    long long loop_resets()    const { return loop_resets_.load(std::memory_order_relaxed); }

    // Coded BER above which nothing decodes (noise floor is ~0.2; a usable
    // signal is well under 0.05).
    static constexpr float kDeadBer = 0.10f;
    // ~2 s at 256 ksym/s.
    static constexpr long long kDeadSymbols = 512000;

private:
    dsp::BpskDemod demod_;
    fec::ViterbiDecoder vit0_;
    fec::ViterbiDecoder vit1_;
    BitPipeline pipe0_;
    BitPipeline pipe1_;
    std::vector<float> softs_;
    std::vector<uint8_t> bits_;
    bool skipped_ = false;
    long long dead_symbols_ = 0;
    // Incremented by the DSP thread, read by the UI thread.
    std::atomic<long long> loop_resets_{0};
};

} // namespace jalert::decode
