#pragma once
// Polyphase clock sync. Master RRC at nfilt × sps virtual rate, with a
// derivative arm set driving a Mueller-Müller-style timing-error detector. This
// block is also the chain's matched filter. The TED is modulation-agnostic and
// works for the J-ALERT BPSK.

#include <vector>

#include "complex.h"

namespace jalert::dsp {

class PfbClockSync {
public:
    PfbClockSync(int nfilt, float sps_actual, float loop_bw, float max_dev,
                 int rrc_symbols, float rolloff);

    float error() const { return error_; }
    float rate_f() const { return rate_f_; }

    // Push z; returns true and writes output when the loop emits a symbol.
    bool step(Complex32 z, Complex32& output);

    // Snap the PI-loop state back to the ctor defaults; leaves the sample ring.
    void reset_loop();

    // Back to the ctor state: loop state plus the sample ring and read position.
    void reset();

private:
    bool try_emit(Complex32& output);

    class LinkedListBuf {
    public:
        explicit LinkedListBuf(int initial_capacity);
        int count() const { return count_; }
        Complex32 operator[](int idx) const;
        void push_back(Complex32 z);
        void pop_front();

    private:
        void grow();
        std::vector<Complex32> data_;
        int start_ = 0;
        int count_ = 0;
    };

    int nfilt_;
    int sps_;
    int taps_per_arm_;
    std::vector<std::vector<float>> arms_;
    std::vector<std::vector<float>> diff_arms_;

    float alpha_;
    float beta_;
    float max_dev_;

    float rate_f_ = 0.0f;
    float rate_i_ = 0.0f;
    float k_ = 0.0f;
    float error_ = 0.0f;

    float initial_rate_f_ = 0.0f;
    float initial_rate_i_ = 0.0f;

    LinkedListBuf buf_;
    int read_pos_ = 0;
    int prefix_keep_;
    bool warmed_ = false;
};

} // namespace jalert::dsp
