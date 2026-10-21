#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/widgets/menu.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <signal_path/vfo_manager.h>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/types.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "decode/receiver.h"
#include "sink/alert_json.h"
#include "sink/file_sink.h"
#include "sink/tcp_sink.h"
#include "ui/constellation_view.h"
#include "ui/sparkline.h"

namespace gui {
    extern Menu menu;
}

SDRPP_MOD_INFO{
    /* Name:            */ "j_alert_decoder",
    /* Description:     */ "J-ALERT satellite (BPSK SCPC) decoder",
    /* Author:          */ "ut1808",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

namespace {
// VFO output sample rate fed to the demod. 4 sps × 256 ksym/s = 1.024 MHz, so
// the internal resampler runs near unity ratio.
constexpr double kInputSampleRate = jalert::dsp::BpskDemod::kWorkRate;  // 1.024 MHz
// (1+β)·Rs ≈ 346 kHz occupied; a little margin so off-tuning stays in-band.
constexpr double kVfoBandwidth = 400000.0;
constexpr double kPushIntervalSec = 0.5;
constexpr size_t kRecentAlerts = 32;
// Coded BER under which the link is clean (≈ 0.00x). A higher BER does not
// mean no data: the FEC may still correct it and packets can still arrive.
constexpr float kGoodBer = 0.05f;
} // namespace

// Lightweight per-file summary kept for the panel (no full XML — that goes to
// the sinks / XML folder).
struct AlertSummary {
    long long rx_time_ms = 0;
    bool decoded = false;
    std::string chunk_type;     // wrmx / eprx / …
    std::string packet_time;
    std::string control_title;
    std::string head_title;
    std::string info_type;
    std::string report_time;
    std::string headline;
    size_t xml_bytes = 0;
};

class JAlertDecoderModule : public ModuleManager::Instance {
public:
    explicit JAlertDecoderModule(std::string name)
        : name_(std::move(name)),
          constellation_([this](jalert::dsp::Complex32* dst, int cap) {
              return recv_ ? recv_->demod().pull_constellation(dst, cap) : 0;
          }) {
        // Default output locations under the SDR++ config root (e.g.
        // ~/.config/sdrpp/j_alert/).
        const std::string default_dir = core::args["root"].s() + "/j_alert";
        config.acquire();
        if (!config.conf.contains(name_)) config.conf[name_] = json::object();
        xml_dir_        = config.conf[name_].value("xmlOutputDir", default_dir);
        file_output_enabled_ = config.conf[name_].value("fileOutputEnabled", false);
        file_sink_path_ = config.conf[name_].value("jsonlFilePath", default_dir + "/decoded.jsonl");
        tcp_sink_port_  = config.conf[name_].value("jsonlTcpPort", 7355);
        file_sink_enabled_ = config.conf[name_].value("jsonlFileEnabled", false);
        tcp_sink_enabled_  = config.conf[name_].value("jsonlTcpEnabled", false);
        config.release(true);

        copy_to_buf(file_sink_path_, file_sink_path_buf_, sizeof(file_sink_path_buf_));
        copy_to_buf(xml_dir_, xml_dir_buf_, sizeof(xml_dir_buf_));

        if (file_sink_enabled_ && !file_sink_path_.empty())
            file_sink_ = std::make_unique<jalert::sink::FileJsonlSink>(file_sink_path_);
        if (tcp_sink_enabled_ && tcp_sink_port_ > 0)
            tcp_sink_ = std::make_unique<jalert::sink::TcpJsonlSink>(tcp_sink_port_);

        recv_ = std::make_unique<jalert::decode::Receiver>(
            kInputSampleRate,
            [this](const jalert::decode::DecodedAlert& a) { on_alert(a); });

        spark_coarse_.set_fixed_range(-300000.0, 300000.0);
        spark_costas_.set_fixed_range(-2000.0, 2000.0);
        spark_quality_.set_fixed_range(0.0, 20.0);   // BER %, 0..20 (no-signal ~15%)

        start_time_ = std::chrono::steady_clock::now();
        last_push_time_ = start_time_;

        vfo_ = sigpath::vfoManager.createVFO(name_, ImGui::WaterfallVFO::REF_CENTER,
                                             0, kVfoBandwidth, kInputSampleRate,
                                             kVfoBandwidth, kVfoBandwidth, true);
        sink_.init(vfo_->output, &JAlertDecoderModule::iq_handler, this);
        sink_.start();

        gui::menu.registerEntry(name_, &JAlertDecoderModule::menu_handler, this, this);
    }

    ~JAlertDecoderModule() override {
        gui::menu.removeEntry(name_);
        if (enabled_) {
            sink_.stop();
            sigpath::vfoManager.deleteVFO(vfo_);
        }
        if (file_sink_) file_sink_->stop();
        if (tcp_sink_)  tcp_sink_->stop();
    }

    void postInit() override {}

    void enable() override {
        if (enabled_) return;
        // Fresh acquisition on re-enable (the DSP thread is stopped here), so
        // disable/enable recovers the decoder without restarting SDR++.
        recv_->reset();
        vfo_ = sigpath::vfoManager.createVFO(name_, ImGui::WaterfallVFO::REF_CENTER,
                                             0, kVfoBandwidth, kInputSampleRate,
                                             kVfoBandwidth, kVfoBandwidth, true);
        sink_.setInput(vfo_->output);
        sink_.start();
        enabled_ = true;
    }

    void disable() override {
        if (!enabled_) return;
        sink_.stop();
        sigpath::vfoManager.deleteVFO(vfo_);
        vfo_ = nullptr;
        enabled_ = false;
    }

    bool isEnabled() override { return enabled_; }

private:
    static void copy_to_buf(const std::string& s, char* buf, size_t cap) {
        const size_t n = std::min(s.size(), cap - 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }

    // Filename-safe component from a chunk-type code (keep [a-z0-9], else "dat").
    static std::string safe_name(const std::string& s) {
        std::string out;
        for (char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
            else if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
        }
        return out.empty() ? "dat" : out;
    }

    // ── DSP thread ────────────────────────────────────────────────────────
    static void iq_handler(dsp::complex_t* data, int count, void* ctx) {
        auto* self = static_cast<JAlertDecoderModule*>(ctx);
        if (!self->enabled_) return;
        for (int i = 0; i < count; ++i) {
            self->recv_->process(jalert::dsp::Complex32(data[i].re, data[i].im));
        }
    }

    // Called from the DSP thread when a NowcastPacket is decoded.
    void on_alert(const jalert::decode::DecodedAlert& a) {
        const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Fan out to JSONL sinks. The UI thread swaps these pointers, so hold
        // sinks_mtx_ while using them.
        {
            std::lock_guard<std::mutex> lk(sinks_mtx_);
            if (file_sink_ || tcp_sink_) {
                const std::string line = jalert::sink::serialize_alert(a, now_ms);
                if (file_sink_) file_sink_->write_line(line);
                if (tcp_sink_)  tcp_sink_->write_line(line);
            }
        }

        // Auto-save every recovered file to the configured folder: decoded JMA
        // telegrams as <timestamp>.xml, other (non-XML) telegrams as the raw
        // recovered bytes in <timestamp>.<chunk_type>.bin.
        if (!a.timestamp.empty()) {
            std::string dir;
            bool file_out;
            { std::lock_guard<std::mutex> lk(ui_mtx_); dir = xml_dir_; file_out = file_output_enabled_; }
            if (file_out && !dir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                std::string path;
                const uint8_t* bytes = nullptr;
                size_t n = 0;
                if (a.ok) {
                    path = dir + "/" + a.timestamp + ".xml";
                    bytes = a.xml.data();
                    n = a.xml.size();
                } else if (!a.data.empty()) {
                    path = dir + "/" + a.timestamp + "." + safe_name(a.chunk_type) + ".bin";
                    bytes = a.data.data();
                    n = a.data.size();
                }
                if (bytes) {
                    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
                        std::fwrite(bytes, 1, n, fp);
                        std::fclose(fp);
                    }
                }
            }
        }

        AlertSummary s;
        s.rx_time_ms = now_ms;
        s.decoded = a.ok;
        s.chunk_type = a.chunk_type;
        s.packet_time = a.timestamp;
        s.control_title = a.control_title;
        s.head_title = a.head_title;
        s.info_type = a.info_type;
        s.report_time = a.report_datetime;
        s.headline = a.headline_text;
        s.xml_bytes = a.xml.size();
        {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            recent_.push_front(std::move(s));
            while (recent_.size() > kRecentAlerts) recent_.pop_back();
        }
    }

    // ── UI ──────────────────────────────────────────────────────────────────
    static void menu_handler(void* ctx) {
        static_cast<JAlertDecoderModule*>(ctx)->draw_menu();
    }

    void draw_menu() {
        if (!enabled_) style::beginDisabled();

        const auto& demod = recv_->demod();
        const bool locked = demod.locked();
        const double coarse = demod.coarse_offset_hz();
        const double costas = demod.costas_offset_hz();
        const long long syms = demod.symbols();
        const long long fok = recv_->frames_ok();
        const long long fbad = recv_->frames_bad();
        const long long pkts = recv_->packets();

        const auto now = std::chrono::steady_clock::now();
        const double since_push = std::chrono::duration<double>(now - last_push_time_).count();
        if (since_push >= kPushIntervalSec) {
            spark_coarse_.push(coarse);
            spark_costas_.push(costas);
            spark_quality_.push(static_cast<double>(recv_->ber()) * 100.0);
            last_push_time_ = now;
        }

        // Status lamp + label. "Locked" also needs a low coded BER; a carrier
        // lock with a high BER is shown separately (it may still decode).
        const bool low_ber = recv_->ber() < kGoodBer;
        ImU32 lamp;
        const char* label;
        if (!enabled_)      { lamp = IM_COL32(120,120,120,255); label = "Disabled"; }
        else if (locked && low_ber) { lamp = IM_COL32(70,180,85,255);  label = "Locked"; }
        else if (locked)    { lamp = IM_COL32(220,170,40,255); label = "Carrier locked, high BER"; }
        else if (demod.sym_mag() > 0.3f) { lamp = IM_COL32(220,170,40,255); label = "Signal, acquiring"; }
        else                { lamp = IM_COL32(200,70,70,255);  label = "Searching"; }
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(ImGui::GetCursorScreenPos().x + 7, ImGui::GetCursorScreenPos().y + 9),
            5.0f, lamp);
        ImGui::Dummy(ImVec2(16, 0)); ImGui::SameLine();
        ImGui::TextUnformatted(label);

        ImGui::Separator();
        const float content_w = ImGui::GetContentRegionAvail().x;
        constellation_.draw(std::min(content_w, 240.0f));

        char buf[80];
        const float cap_w = ImGui::CalcTextSize("Carrier offset").x + 8.0f;
        std::snprintf(buf, sizeof(buf), "%+.0f Hz", coarse);
        spark_coarse_.draw("Carrier offset", buf, IM_COL32(70,180,85,255), cap_w);
        std::snprintf(buf, sizeof(buf), "%+.0f Hz", costas);
        spark_costas_.draw("Costas resid.", buf, IM_COL32(70,180,85,255), cap_w);
        const double ber_pct = static_cast<double>(recv_->ber()) * 100.0;
        const ImU32 ber_col = ber_pct < 1.0  ? IM_COL32(70,180,85,255)   // green
                            : ber_pct < 5.0  ? IM_COL32(220,170,40,255)  // amber
                                             : IM_COL32(200,70,70,255);  // red
        std::snprintf(buf, sizeof(buf), "%.2f%%", ber_pct);
        spark_quality_.draw("Bit error rate", buf, ber_col, cap_w);

        ImGui::Separator();
        ImGui::Text("Symbols: %lld", syms);
        ImGui::Text("HDLC frames: %lld ok / %lld rejected", fok, fbad);
        ImGui::Text("NowcastPackets: %lld  (status %lld)", pkts, recv_->status_packets());
        ImGui::Text("Files: %lld  decoded %lld", recv_->files(), recv_->alerts());
        ImGui::Text("Re-acquisitions: %lld", recv_->loop_resets());

        ImGui::Separator();
        ImGui::TextUnformatted("Latest alert");
        draw_latest();

        ImGui::Separator();
        ImGui::TextUnformatted("Output");
        draw_output();

        if (!enabled_) style::endDisabled();
    }

    void draw_latest() {
        AlertSummary s;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            for (const auto& r : recent_) {
                if (r.decoded) { s = r; have = true; break; }
            }
        }
        if (!have) { ImGui::TextDisabled("(none yet)"); return; }
        ImGui::PushTextWrapPos(0.0f);
        if (!s.head_title.empty())   ImGui::Text("%s", s.head_title.c_str());
        else if (!s.control_title.empty()) ImGui::Text("%s", s.control_title.c_str());
        if (!s.info_type.empty() || !s.report_time.empty())
            ImGui::TextDisabled("%s  %s", s.info_type.c_str(), s.report_time.c_str());
        if (!s.headline.empty()) ImGui::TextWrapped("%s", s.headline.c_str());
        ImGui::TextDisabled("[%s] packet %s  (%zu B XML)",
                            s.chunk_type.c_str(), s.packet_time.c_str(), s.xml_bytes);
        ImGui::PopTextWrapPos();

        if (ImGui::TreeNode((std::string("Recent (")
                             + std::to_string(recent_size()) + ")##rec_" + name_).c_str())) {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            for (const auto& r : recent_) {
                ImGui::TextWrapped("[%s] %s  %s", r.chunk_type.c_str(),
                                   r.packet_time.c_str(),
                                   r.head_title.empty() ? r.control_title.c_str()
                                                        : r.head_title.c_str());
            }
            ImGui::TreePop();
        }
    }

    size_t recent_size() {
        std::lock_guard<std::mutex> lk(ui_mtx_);
        return recent_.size();
    }

    void draw_output() {
        // File output: save every recovered file (XML or raw) to a folder.
        if (ImGui::Checkbox(("File output##fileout_" + name_).c_str(),
                            &file_output_enabled_)) {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            save_field("fileOutputEnabled", file_output_enabled_);
        }
        ImGui::TextUnformatted("Output folder");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText(("##xmldir_" + name_).c_str(), xml_dir_buf_,
                             sizeof(xml_dir_buf_), ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            xml_dir_ = xml_dir_buf_;
            save_field("xmlOutputDir", xml_dir_);
        }

        // JSONL file sink. The checkbox reflects whether the sink is running;
        // toggling it starts (path non-empty) or stops the sink.
        bool file_on = static_cast<bool>(file_sink_);
        if (ImGui::Checkbox(("JSONL file output##filesink_" + name_).c_str(), &file_on)) {
            std::unique_ptr<jalert::sink::FileJsonlSink> next;
            if (file_on && !file_sink_path_.empty())
                next = std::make_unique<jalert::sink::FileJsonlSink>(file_sink_path_);
            file_sink_enabled_ = static_cast<bool>(next);
            swap_sink(file_sink_, std::move(next));
            save_field("jsonlFileEnabled", file_sink_enabled_);
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText(("##fpath_" + name_).c_str(), file_sink_path_buf_,
                             sizeof(file_sink_path_buf_), ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            file_sink_path_ = file_sink_path_buf_;
            save_field("jsonlFilePath", file_sink_path_);
        }
        if (file_sink_) {
            const auto snap = file_sink_->snapshot();
            if (snap.error_message.empty())
                ImGui::TextDisabled("%lld records", snap.records_written);
            else
                ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f), "Error: %s",
                                   snap.error_message.c_str());
        }

        // JSONL TCP sink.
        bool tcp_on = static_cast<bool>(tcp_sink_);
        if (ImGui::Checkbox(("JSONL TCP output##tcpsink_" + name_).c_str(), &tcp_on)) {
            std::unique_ptr<jalert::sink::TcpJsonlSink> next;
            if (tcp_on) next = std::make_unique<jalert::sink::TcpJsonlSink>(tcp_sink_port_);
            tcp_sink_enabled_ = static_cast<bool>(next);
            swap_sink(tcp_sink_, std::move(next));
            save_field("jsonlTcpEnabled", tcp_sink_enabled_);
        }
        ImGui::TextUnformatted("TCP port");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt(("##tport_" + name_).c_str(), &tcp_sink_port_, 0)) {
            tcp_sink_port_ = std::clamp(tcp_sink_port_, 1, 65535);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) save_field("jsonlTcpPort", tcp_sink_port_);
        if (tcp_sink_) {
            const auto snap = tcp_sink_->snapshot();
            if (snap.error_message.empty())
                ImGui::TextDisabled("%d client(s)", snap.client_count);
            else
                ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f), "Error: %s",
                                   snap.error_message.c_str());
        }
    }

    // Replace a sink the DSP thread may be writing to. The old one is stopped
    // outside the lock (TcpJsonlSink::stop joins threads).
    template <class Sink>
    void swap_sink(std::unique_ptr<Sink>& slot, std::unique_ptr<Sink> next) {
        std::unique_ptr<Sink> old;
        {
            std::lock_guard<std::mutex> lk(sinks_mtx_);
            old = std::move(slot);
            slot = std::move(next);
        }
        if (old) old->stop();
    }

    template <class T>
    void save_field(const char* key, const T& value) {
        config.acquire();
        config.conf[name_][key] = value;
        config.release(true);
    }

    // ── Members ─────────────────────────────────────────────────────────────
    std::string name_;
    bool enabled_ = true;

    VFOManager::VFO* vfo_ = nullptr;
    dsp::sink::Handler<dsp::complex_t> sink_;

    std::unique_ptr<jalert::decode::Receiver> recv_;
    std::unique_ptr<jalert::sink::FileJsonlSink> file_sink_;
    std::unique_ptr<jalert::sink::TcpJsonlSink> tcp_sink_;

    jalert::ui::ConstellationView constellation_;
    jalert::ui::Sparkline spark_coarse_;
    jalert::ui::Sparkline spark_costas_;
    jalert::ui::Sparkline spark_quality_;

    std::string xml_dir_;
    std::string file_sink_path_;
    char xml_dir_buf_[512] = {};
    char file_sink_path_buf_[512] = {};
    int tcp_sink_port_ = 7355;
    bool file_output_enabled_ = false;
    bool file_sink_enabled_ = false;
    bool tcp_sink_enabled_ = false;

    std::mutex ui_mtx_;
    std::mutex sinks_mtx_;   // guards file_sink_/tcp_sink_ swaps vs. on_alert
    std::deque<AlertSummary> recent_;

    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point last_push_time_{};
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/j_alert_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new JAlertDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete static_cast<JAlertDecoderModule*>(instance);
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
