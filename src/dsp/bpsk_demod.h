#pragma once
// J-ALERT BPSK front end: complex IQ → soft symbols (one float per symbol).
//
// Chain:
//   coarse squaring carrier removal (input rate, handles a large offset)  →
//   polyphase resample to 1.024 MHz (4 sps × 256 ksym/s)  →  AGC  →
//   PfbClockSync (RRC matched filter + Müller-Müller timing)  →
//   decision-directed Costas loop  →  soft = Re{y}.
//
// BPSK squared is a pure tone at 2·fc (d²=1), so a block FFT peak of the
// squared signal gives the carrier offset directly. The 180° phase ambiguity it
// leaves is absorbed downstream by the differential decode.

#include <cstdint>
#include <vector>

#include <complex>
#include <mutex>

#include "dsp/agc.h"
#include "dsp/complex.h"
#include "dsp/pfb_clock_sync.h"
#include "dsp/polyphase_resampler.h"

namespace jalert::dsp {

class BpskDemod {
public:
    static constexpr double kWorkRate = 1'024'000.0;     // 4 sps × 256 ksym/s
    static constexpr float  kSymRate  = 256'000.0f;
    static constexpr int    kSps      = 4;

    explicit BpskDemod(double sample_rate_hz);

    // Feed one input sample; append zero or more soft symbols to soft_out.
    void process(Complex32 z, std::vector<float>& soft_out);

    void reset();

    // Re-acquire: snap the timing and Costas loops back to their initial state
    // (keeps the coarse estimate and filter histories).
    void reset_loops();

    // Debug / manual tuning: pin the coarse carrier offset (Hz) and disable the
    // squaring estimator. Used by the offline harness to isolate stages.
    void set_manual_coarse(double hz);

    // Debug tap: if set, post-(coarse+resample+fine) complex samples at the
    // 1.024 MHz work rate are appended here (one vector growing across calls).
    void set_debug_tap(std::vector<Complex32>* tap) { debug_tap_ = tap; }
    // Tap the coarse-derotated stream at the INPUT rate (pre-resample).
    void set_debug_tap_in(std::vector<Complex32>* tap) { debug_tap_in_ = tap; }
    // Tap the resampled stream at the work rate (post-resample, pre-fine).
    void set_debug_tap_rs(std::vector<Complex32>* tap) { debug_tap_rs_ = tap; }

    // ── Diagnostics (UI) ──
    double coarse_offset_hz() const;     // estimated carrier offset, input rate
    double costas_offset_hz() const;     // residual tracked by the Costas loop
    bool   locked() const { return locked_; }
    float  sym_mag() const { return sym_mag_ema_; }
    Complex32 last_symbol() const { return last_sym_; }
    long long symbols() const { return symbols_; }

    // Pull up to capacity of the most-recent Costas-derotated symbols (newest
    // first) for the constellation view. Thread-safe.
    int pull_constellation(Complex32* dst, int capacity) const;

private:
    static Complex32 rotate(Complex32 z, double phase);  // z · e^{-jφ}

    double sample_rate_;

    // Coarse carrier (input rate): block FFT-argmax on the (DC-removed) squared
    // signal — a tone at 2·fc — estimates the offset for the next block. Robust
    // to large offsets and to the spectral aliasing that biases a lag-1
    // estimator near Nyquist.
    static constexpr int kCoarseBlock = 8192;
    std::vector<std::complex<double>> c_blk_;
    double c_phase_ = 0.0;
    double c_omega_ = 0.0;       // per-sample removal increment (rad)
    bool c_manual_ = false;

    PolyphaseResampler resamp_;
    std::vector<Complex32> rbuf_;

    Agc agc_;
    PfbClockSync sync_;

    // Costas (per symbol).
    double costas_phase_ = 0.0;
    double costas_freq_ = 0.0;

    Complex32 last_sym_{};
    float sym_mag_ema_ = 0.0f;
    float lock_re_ = 0.0f;
    float lock_im_ = 0.0f;
    bool locked_ = false;
    long long symbols_ = 0;

    std::vector<Complex32>* debug_tap_ = nullptr;
    std::vector<Complex32>* debug_tap_in_ = nullptr;
    std::vector<Complex32>* debug_tap_rs_ = nullptr;

    // Constellation ring (DSP thread writes, UI thread pulls).
    static constexpr int kConstCap = 2048;
    mutable std::mutex const_mtx_;
    Complex32 const_ring_[kConstCap] = {};
    int const_idx_ = 0;
    long long const_count_ = 0;
};

} // namespace jalert::dsp
