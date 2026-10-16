#include "dsp/bpsk_demod.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "dsp/fft.h"

namespace jalert::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;


// Costas loop (decision-directed BPSK), per symbol.
constexpr double kCostasFreqGain  = 2.5e-5;
constexpr double kCostasPhaseGain = 1.0e-2;

// Resampler anti-alias / channel cutoff (one-sided). Passes the (1+β)·Rs/2 ≈
// 173 kHz occupied band with margin, blocks the resampler images.
constexpr double kResampCutoffHz = 250'000.0;
constexpr int    kResampPhases   = 32;

constexpr int   kAgcWindow = 64;
constexpr int   kPfbNFilt  = 32;
constexpr float kPfbLoopBw = 0.01f;
// Limit on the timing loop's fractional-rate integrator. Each symbol advances
// the filter-arm index by ~5·rate_f, i.e. a symbol-rate error of about
// 5·rate_f / (nfilt·sps). 0.05 allows ±0.2 % — orders of magnitude more than
// real modem/SDR clock error — while 1.5 (±5.9 %) let the integrator random-walk
// far away during a signal gap and false-lock there after the signal returned.
constexpr float kPfbMaxDev = 0.05f;
constexpr int   kPfbRrcSym = 12;
constexpr float kRolloff   = 0.35f;

inline double wrap(double p) {
    p = std::fmod(p, kTwoPi);
    if (p > M_PI) p -= kTwoPi;
    else if (p < -M_PI) p += kTwoPi;
    return p;
}

// Coarse carrier offset (per-sample removal phase increment) from a block of
// input samples: FFT the DC-removed squared signal, take the peak bin (a tone
// at 2·fc), halve. Returns omega = 2π·fc/Fs.
double estimate_coarse_omega(std::vector<std::complex<double>>& blk) {
    const size_t n = blk.size();
    std::complex<double> mean(0.0, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const std::complex<double>& z = blk[i];
        blk[i] = z * z;                 // square in place
        mean += blk[i];
    }
    mean /= static_cast<double>(n);
    for (size_t i = 0; i < n; ++i) blk[i] -= mean;   // remove DC of squared

    fft_radix2(blk);

    size_t kmax = 0;
    double best = -1.0;
    for (size_t k = 0; k < n; ++k) {
        const double p = std::norm(blk[k]);
        if (p > best) { best = p; kmax = k; }
    }
    const long signed_bin = (kmax < n / 2)
        ? static_cast<long>(kmax)
        : static_cast<long>(kmax) - static_cast<long>(n);
    const double nu = static_cast<double>(signed_bin) / static_cast<double>(n); // 2fc/Fs
    return M_PI * nu;                    // 2π·fc/Fs
}
} // namespace

BpskDemod::BpskDemod(double sample_rate_hz)
    : sample_rate_(sample_rate_hz),
      resamp_(sample_rate_hz, kWorkRate, kResampCutoffHz, kResampPhases),
      agc_(kAgcWindow, 1.0f),
      sync_(kPfbNFilt, static_cast<float>(kSps), kPfbLoopBw, kPfbMaxDev,
            kPfbRrcSym, kRolloff) {
    rbuf_.reserve(8);
    c_blk_.reserve(kCoarseBlock);
}

Complex32 BpskDemod::rotate(Complex32 z, double phase) {
    const float c = static_cast<float>(std::cos(phase));
    const float s = static_cast<float>(std::sin(phase));
    // z · e^{-jφ}
    return Complex32(z.re * c + z.im * s, z.im * c - z.re * s);
}

void BpskDemod::reset_loops() {
    sync_.reset_loop();
    costas_phase_ = costas_freq_ = 0.0;
    lock_re_ = lock_im_ = 0.0f;
    locked_ = false;
}

// Back to the ctor state, filter histories included, so a receiver poisoned by
// non-finite input recovers. (A manual coarse offset is cleared too.)
void BpskDemod::reset() {
    c_blk_.clear();
    c_phase_ = c_omega_ = 0.0;
    c_manual_ = false;
    resamp_.reset();
    agc_.reset();
    sync_.reset();
    costas_phase_ = costas_freq_ = 0.0;
    last_sym_ = Complex32();
    sym_mag_ema_ = lock_re_ = lock_im_ = 0.0f;
    locked_ = false;
    symbols_ = 0;

    std::lock_guard<std::mutex> lk(const_mtx_);
    std::fill(std::begin(const_ring_), std::end(const_ring_), Complex32());
    const_idx_ = 0;
    const_count_ = 0;
}

void BpskDemod::set_manual_coarse(double hz) {
    c_manual_ = true;
    c_omega_ = kTwoPi * hz / sample_rate_;
}

void BpskDemod::process(Complex32 z, std::vector<float>& soft_out) {
    // ── Coarse carrier (block FFT-argmax on the squared signal) ──
    // Each full block re-estimates the offset, applied to the following block;
    // the SCPC carrier is stable so this settles to a fixed value after one
    // block (~4 ms at 2 MHz).
    if (!c_manual_) {
        c_blk_.emplace_back(z.re, z.im);
        if (static_cast<int>(c_blk_.size()) >= kCoarseBlock) {
            c_omega_ = estimate_coarse_omega(c_blk_);
            c_blk_.clear();
        }
    }
    if (!std::isfinite(c_omega_)) c_omega_ = 0.0;
    c_phase_ = wrap(c_phase_ + c_omega_);
    if (!std::isfinite(c_phase_)) c_phase_ = 0.0;
    const Complex32 zc = rotate(z, c_phase_);
    if (debug_tap_in_) debug_tap_in_->push_back(zc);

    // ── Resample to the 1.024 MHz work rate ──
    rbuf_.clear();
    resamp_.step(zc, rbuf_);
    for (Complex32 zr : rbuf_) {
        if (debug_tap_rs_) debug_tap_rs_->push_back(zr);
        // No standalone fine-carrier stage: at 4 sps the squared signal's
        // symbol-rate spectral lines bias a lag-1 frequency estimate, so the
        // coarse stage (run at the well-oversampled input rate) plus the Costas
        // loop's own frequency integrator carry residual correction instead.
        if (debug_tap_) debug_tap_->push_back(zr);

        // ── AGC ──
        Complex32 za;
        if (!agc_.step(zr, za)) continue;

        // ── Matched filter + symbol timing ──
        Complex32 sym;
        if (!sync_.step(za, sym)) continue;

        // ── Costas (decision-directed BPSK) ──
        const Complex32 y = rotate(sym, costas_phase_);
        const double dec = (y.re >= 0.0f) ? 1.0 : -1.0;
        const double e = static_cast<double>(y.im) * dec;
        costas_freq_ += kCostasFreqGain * e;
        costas_phase_ = wrap(costas_phase_ + costas_freq_ + kCostasPhaseGain * e);
        if (!std::isfinite(costas_phase_) || !std::isfinite(costas_freq_)) {
            // A non-finite value would otherwise stick in the loop forever.
            costas_phase_ = costas_freq_ = 0.0;
            continue;
        }

        last_sym_ = y;
        ++symbols_;
        {
            std::lock_guard<std::mutex> lk(const_mtx_);
            const_ring_[const_idx_] = y;
            const_idx_ = (const_idx_ + 1) % kConstCap;
            ++const_count_;
        }

        const float mag = std::sqrt(y.re * y.re + y.im * y.im);
        sym_mag_ema_ += 0.01f * (mag - sym_mag_ema_);
        lock_re_ += 0.01f * (std::fabs(y.re) - lock_re_);
        lock_im_ += 0.01f * (std::fabs(y.im) - lock_im_);
        locked_ = (lock_re_ > 0.3f) && (lock_im_ < 0.5f * lock_re_);

        soft_out.push_back(y.re);
    }
}

double BpskDemod::coarse_offset_hz() const {
    return c_omega_ / kTwoPi * sample_rate_;
}

double BpskDemod::costas_offset_hz() const {
    return costas_freq_ / kTwoPi * static_cast<double>(kSymRate);
}

int BpskDemod::pull_constellation(Complex32* dst, int capacity) const {
    if (!dst || capacity <= 0) return 0;
    std::lock_guard<std::mutex> lk(const_mtx_);
    const int n = static_cast<int>(std::min<long long>(const_count_, capacity));
    const int ring_n = std::min(n, kConstCap);
    int idx = const_idx_ - 1;
    if (idx < 0) idx += kConstCap;
    for (int i = 0; i < ring_n; ++i) {
        dst[i] = const_ring_[idx];
        idx = idx == 0 ? kConstCap - 1 : idx - 1;
    }
    return ring_n;
}

} // namespace jalert::dsp
