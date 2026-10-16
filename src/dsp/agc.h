#pragma once
// Feed-forward AGC. Buffers nsamples of look-ahead and emits the oldest sample
// scaled by reference / peak-envelope-of-the-window.

#include <algorithm>
#include <cmath>
#include <vector>

#include "complex.h"

namespace jalert::dsp {

class Agc {
public:
    Agc(int nsamples, float reference)
        : window_(nsamples), nsamples_(nsamples), reference_(reference) {}

    // Pushes z; returns true and writes output when the look-ahead window is
    // full; returns false during the initial fill.
    bool step(Complex32 z, Complex32& output) {
        window_[head_] = z;
        head_ = (head_ + 1) % nsamples_;
        if (fill_ < nsamples_) ++fill_;
        if (fill_ < nsamples_) return false;

        const Complex32 oldest = window_[head_];
        float max_env = 1e-4f;
        for (int i = 0; i < nsamples_; ++i) {
            const float e = envelope(window_[i]);
            if (e > max_env) max_env = e;
        }
        const float gain = reference_ / max_env;
        output = Complex32(oldest.re * gain, oldest.im * gain);
        return true;
    }

    // Back to the ctor state (empty window).
    void reset() {
        std::fill(window_.begin(), window_.end(), Complex32());
        head_ = 0;
        fill_ = 0;
    }

private:
    // max(|I|,|Q|) + 0.4·min(|I|,|Q|) — ~0.2 dB approximation of √(I²+Q²).
    static float envelope(Complex32 z) {
        const float r = std::fabs(z.re);
        const float i = std::fabs(z.im);
        return r > i ? r + 0.4f * i : i + 0.4f * r;
    }

    std::vector<Complex32> window_;
    int nsamples_;
    float reference_;
    int head_ = 0;
    int fill_ = 0;
};

} // namespace jalert::dsp
