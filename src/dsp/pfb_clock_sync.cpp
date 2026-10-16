#include "pfb_clock_sync.h"

#include <algorithm>
#include <cmath>

#include "filter_design.h"

namespace jalert::dsp {

PfbClockSync::LinkedListBuf::LinkedListBuf(int initial_capacity)
    : data_(std::max(initial_capacity, 16)) {}

Complex32 PfbClockSync::LinkedListBuf::operator[](int idx) const {
    const int n = static_cast<int>(data_.size());
    return data_[(start_ + idx) % n];
}

void PfbClockSync::LinkedListBuf::push_back(Complex32 z) {
    const int n = static_cast<int>(data_.size());
    if (count_ == n) grow();
    const int slot = (start_ + count_) % static_cast<int>(data_.size());
    data_[slot] = z;
    ++count_;
}

void PfbClockSync::LinkedListBuf::pop_front() {
    if (count_ == 0) return;
    start_ = (start_ + 1) % static_cast<int>(data_.size());
    --count_;
}

void PfbClockSync::LinkedListBuf::grow() {
    const int old_n = static_cast<int>(data_.size());
    std::vector<Complex32> bigger(old_n * 2);
    for (int i = 0; i < count_; ++i) bigger[i] = data_[(start_ + i) % old_n];
    data_ = std::move(bigger);
    start_ = 0;
}

PfbClockSync::PfbClockSync(int nfilt, float sps_actual, float loop_bw, float max_dev,
                           int rrc_symbols, float rolloff)
    : nfilt_(nfilt),
      sps_(static_cast<int>(std::max(std::floor(sps_actual), 1.0f))),
      buf_(16) {
    constexpr int kPfbMasterSymbols = 8;
    const int total_taps = nfilt * std::max(rrc_symbols, kPfbMasterSymbols * sps_);
    auto master = rrc_taps(static_cast<float>(nfilt),
                            static_cast<float>(nfilt * sps_),
                            1.0f, rolloff, total_taps);
    arms_ = polyphase_decompose(master, nfilt);
    auto dtaps = create_diff_taps(master, nfilt);
    diff_arms_ = polyphase_decompose(dtaps, nfilt);
    taps_per_arm_ = static_cast<int>(arms_[0].size());

    // Critically damped: damping = 2 · nfilt; α/β derived from loop_bw.
    const float damping = 2.0f * nfilt;
    const float denom = 1.0f + 2.0f * damping * loop_bw + loop_bw * loop_bw;
    alpha_ = 4.0f * damping * loop_bw / denom;
    beta_  = 4.0f * loop_bw * loop_bw / denom;
    max_dev_ = max_dev;

    const float frac_sps = sps_actual - sps_;
    const float total = frac_sps * nfilt;
    rate_i_ = std::floor(total);
    rate_f_ = (total - rate_i_) / static_cast<float>(sps_ + 1);
    initial_rate_i_ = rate_i_;
    initial_rate_f_ = rate_f_;

    k_ = nfilt / 2.0f;
    prefix_keep_ = sps_ + 2;

    buf_ = LinkedListBuf(taps_per_arm_ + 4 * sps_);
}

bool PfbClockSync::step(Complex32 z, Complex32& output) {
    buf_.push_back(z);
    return try_emit(output);
}

void PfbClockSync::reset_loop() {
    k_ = nfilt_ / 2.0f;
    rate_f_ = initial_rate_f_;
    rate_i_ = initial_rate_i_;
    error_ = 0.0f;
}

bool PfbClockSync::try_emit(Complex32& output) {
    const int m = taps_per_arm_;
    const int prefix = prefix_keep_;

    if (!warmed_) {
        const int needed = prefix + m + sps_;
        if (buf_.count() < needed) return false;
        read_pos_ = prefix;
        warmed_ = true;
    }

    // A non-finite loop state would make floor(k_) undefined and the arm
    // normalisation below loop ~2^26 times per sample, stalling the decoder
    // permanently. Re-initialise instead.
    if (!std::isfinite(k_) || !std::isfinite(rate_f_)) reset_loop();

    const float k_save = k_;
    int count = 0;
    int filtnum = static_cast<int>(std::floor(k_));
    while (filtnum >= nfilt_) { k_ -= nfilt_; filtnum -= nfilt_; ++count; }
    while (filtnum < 0)       { k_ += nfilt_; filtnum += nfilt_; --count; }

    const int start = read_pos_ + count;
    const int end = start + m;
    if (start < 0 || end > buf_.count()) {
        k_ = k_save;
        return false;
    }

    const auto& arm = arms_[filtnum];
    const auto& darm = diff_arms_[filtnum];
    float out_re = 0.0f, out_im = 0.0f;
    float diff_re = 0.0f, diff_im = 0.0f;
    for (int j = 0; j < m; ++j) {
        const Complex32 s = buf_[start + j];
        out_re  += s.re * arm[j];
        out_im  += s.im * arm[j];
        diff_re += s.re * darm[j];
        diff_im += s.im * darm[j];
    }

    k_ += rate_i_ + rate_f_;

    float err = (out_re * diff_re + out_im * diff_im) * 0.5f;
    if (!std::isfinite(err)) err = 0.0f;
    error_ = err;

    for (int i = 0; i < sps_; ++i) {
        rate_f_ += beta_ * err;
        k_ += rate_f_ + alpha_ * err;
    }
    if (rate_f_ > max_dev_) rate_f_ = max_dev_;
    else if (rate_f_ < -max_dev_) rate_f_ = -max_dev_;

    const int advance = count + sps_;
    if (advance >= 0) {
        read_pos_ += advance;
    } else {
        const int back = -advance;
        read_pos_ = read_pos_ > back ? read_pos_ - back : 0;
    }

    while (read_pos_ > prefix * 2) {
        buf_.pop_front();
        --read_pos_;
    }

    output = Complex32(out_re, out_im);
    return true;
}

} // namespace jalert::dsp
