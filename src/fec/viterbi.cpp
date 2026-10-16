#include "viterbi.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jalert::fec {

namespace {
// EMA smoothing for the re-encode BER estimate. At 256 ksym/s (128 k pairs/s)
// this averages over roughly a tenth of a second — responsive yet stable.
constexpr float kBerAlpha = 1.0e-4f;
} // namespace

void ViterbiDecoder::build_tables() {
    for (int ns = 0; ns < kNumStates; ++ns) {
        // Input bit consumed entering this state is the high (memory) bit of ns
        // (encoder shifts the input in at the MSB).
        const unsigned b = static_cast<unsigned>(ns >> (kMemory - 1)) & 1u;
        // The two predecessors share s>>1 == ns & (2^(K-2)-1).
        const int lo = ns & ((1 << (kMemory - 1)) - 1);
        const int p0 = lo << 1;
        const int p1 = p0 | 1;
        pred_[ns][0] = static_cast<uint8_t>(p0);
        pred_[ns][1] = static_cast<uint8_t>(p1);
        for (int k = 0; k < 2; ++k) {
            const unsigned p = static_cast<unsigned>(pred_[ns][k]);
            const unsigned reg = (b << kMemory) | p;
            const int o0 = parity(reg & kG1);
            const int o1 = parity(reg & kG2);
            sym0_[ns][k] = o0 ? -1.0f : 1.0f;
            sym1_[ns][k] = o1 ? -1.0f : 1.0f;
        }
    }
}

void ViterbiDecoder::reset() {
    build_tables();
    pm_.fill(0.0f);
    dec_.assign(kRingLen, std::array<uint8_t, kNumStates>{});
    hard_ring_.fill(0);
    reenc_state_ = 0;
    ber_ema_ = 0.5f;
    step_ = 0;
    pending_ = 0.0f;
    have_pending_ = false;
}

void ViterbiDecoder::acs_step(float r0, float r1, std::vector<uint8_t>& out) {
    auto& dec = dec_[static_cast<size_t>(step_ % kRingLen)];
    // Record the received hard decisions for this pair (coded bit = soft < 0).
    hard_ring_[static_cast<size_t>(step_ % kRingLen)] =
        static_cast<uint8_t>(((r0 < 0.0f) ? 2 : 0) | ((r1 < 0.0f) ? 1 : 0));

    float best_pm = -std::numeric_limits<float>::infinity();
    for (int ns = 0; ns < kNumStates; ++ns) {
        const int p0 = pred_[ns][0];
        const int p1 = pred_[ns][1];
        const float m0 = pm_[p0] + r0 * sym0_[ns][0] + r1 * sym1_[ns][0];
        const float m1 = pm_[p1] + r0 * sym0_[ns][1] + r1 * sym1_[ns][1];
        if (m1 > m0) {
            npm_[ns] = m1;
            dec[ns] = 1;
        } else {
            npm_[ns] = m0;
            dec[ns] = 0;
        }
        if (npm_[ns] > best_pm) best_pm = npm_[ns];
    }
    // Normalise to keep metrics bounded over an unbounded stream.
    for (int ns = 0; ns < kNumStates; ++ns) pm_[ns] = npm_[ns] - best_pm;

    ++step_;

    if (step_ >= kTracebackDepth) {
        // Best current state, then walk back kTracebackDepth steps to recover the
        // info bit emitted kTracebackDepth steps ago.
        int cur = 0;
        float top = -std::numeric_limits<float>::infinity();
        for (int ns = 0; ns < kNumStates; ++ns) {
            if (pm_[ns] > top) { top = pm_[ns]; cur = ns; }
        }
        // Walk back kTracebackDepth-1 steps so cur lands on the state *after*
        // the pair whose info bit we emit (its high bit is that info bit).
        const long long head = step_ - 1;  // step index just written
        for (int i = 0; i < kTracebackDepth - 1; ++i) {
            const auto& d = dec_[static_cast<size_t>((head - i) % kRingLen)];
            cur = pred_[cur][d[cur]];
        }
        const uint8_t bit = static_cast<uint8_t>((cur >> (kMemory - 1)) & 1);
        out.push_back(bit);

        // Re-encode BER: encode the emitted bit and compare its two coded bits
        // to the received hard decisions of the pair it came from.
        const long long p = step_ - kTracebackDepth;       // emitted pair index
        const unsigned reg = (static_cast<unsigned>(bit) << kMemory) | reenc_state_;
        const int c0 = parity(reg & kG1);
        const int c1 = parity(reg & kG2);
        reenc_state_ = (reg >> 1) & (kNumStates - 1);
        const uint8_t h = hard_ring_[static_cast<size_t>(p % kRingLen)];
        const int errs = (c0 != ((h >> 1) & 1)) + (c1 != (h & 1));
        ber_ema_ += kBerAlpha * (static_cast<float>(errs) * 0.5f - ber_ema_);
    }
}

void ViterbiDecoder::push(float soft, std::vector<uint8_t>& out) {
    // NaN path metrics never recover (every comparison is false); treat a
    // non-finite soft value as an erasure.
    if (!std::isfinite(soft)) soft = 0.0f;
    if (!have_pending_) {
        pending_ = soft;
        have_pending_ = true;
        return;
    }
    have_pending_ = false;
    acs_step(pending_, soft, out);
}

} // namespace jalert::fec
