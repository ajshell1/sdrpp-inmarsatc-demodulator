#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/networking.h>

#include <dsp/demod/psk.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/buffer/packer.h>
#include <dsp/routing/splitter.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/convert/complex_to_real.h>
#include <dsp/digital/binary_slicer.h>

#include <gui/widgets/constellation_diagram.h>

#define ENABLE_SYNC_DETECT

#include "symbol_extractor.h"
#include "gui_widgets.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO {
    /* Name:            */ "inmarsatc_demodulator",
    /* Description:     */ "Inmarsat-c demodulator for SDR++",
    /* Author:          */ "cropinghigh",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ -1
};

ConfigManager config;

class InmarsatcDemodulatorModule : public ModuleManager::Instance {
public:
    InmarsatcDemodulatorModule(std::string name) {
        this->name = name;

        // Load config
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["hostname"] = hostname;
            config.conf[name]["port"] = port;
            config.conf[name]["listening"] = false;
        }
        std::string host = config.conf[name]["hostname"];
        strcpy(hostname, host.c_str());
        port = config.conf[name]["port"];
        config.release(true);

        float recov_bandwidth = 0.09f;
        float recov_dampningFactor = 0.71f;
        float recov_denominator = (1.0f + 2.0*recov_dampningFactor*recov_bandwidth + recov_bandwidth*recov_bandwidth);
        float recov_mu = (4.0f * recov_dampningFactor * recov_bandwidth) / recov_denominator;
        float recov_omega = (4.0f * recov_bandwidth * recov_bandwidth) / recov_denominator;
        mainDemodulator.init(nullptr, 1200, 3600, 33, 1.0f, 0.5f, 0.05f, recov_omega, recov_mu, 0.01f);
        constDiagSplitter.init(&mainDemodulator.out);
        constDiagSplitter.bindStream(&constDiagStream);
        constDiagSplitter.bindStream(&demodStream);
        constDiagReshaper.init(&constDiagStream, 1024, 0);
        constDiagSink.init(&constDiagReshaper.out, _constDiagSinkHandler, this);

        symbolExtractor.init(&demodStream);
        symbolPacker.init(&symbolExtractor.out, 5000); //pack symbols to groups of 5000, as inmarsat-c decoder requires
        demodSink.init(&symbolPacker.out, _demodSinkHandler, this);

        enable();

        if(config.conf[name]["listening"]) {
            startServer();
        }

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~InmarsatcDemodulatorModule() {
        if(isEnabled()) {
            disable();
        }
        gui::menu.removeEntry(name);
    }

    void postInit() {}

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 2500, 3600, 2500, 2500, true);
        mainDemodulator.setInput(vfo->output);
        mainDemodulator.start();
        constDiagSplitter.start();
        constDiagReshaper.start();
        constDiagSink.start();
        symbolExtractor.start();
        symbolPacker.start();
        demodSink.start();
        enabled = true;
    }

    void disable() {
        mainDemodulator.stop();
        constDiagSplitter.stop();
        constDiagReshaper.stop();
        constDiagSink.stop();
        symbolExtractor.stop();
        symbolPacker.stop();
        demodSink.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:

    static void menuHandler(void* ctx) {
        InmarsatcDemodulatorModule* _this = (InmarsatcDemodulatorModule*)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) {
            style::beginDisabled();
        }
        ImGui::Text("Signal constellation: ");
        ImGui::SetNextItemWidth(menuWidth);
        _this->constDiag.draw();
#ifdef ENABLE_SYNC_DETECT
        float avg = 1.0f - _this->symbolExtractor.stderr;
        ImGui::Text("Signal quality: ");
        ImGui::SameLine();
        ImGui::SigQualityMeter(avg, 0.5f, 1.0f);
        ImGui::Text("Sync ");
        ImGui::SameLine();
        ImGui::BoxIndicator(_this->symbolExtractor.sync ? IM_COL32(5, 230, 5, 255) : IM_COL32(230, 5, 5, 255));
#endif
        bool listening = _this->conn && _this->conn->isOpen();

        if (listening && _this->enabled) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_inmarsatc_demodulator_host_", _this->name), _this->hostname, 1023) && _this->enabled) {
            config.acquire();
            config.conf[_this->name]["hostname"] = _this->hostname;
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_inmarsatc_demodulator_port_", _this->name), &(_this->port), 0, 0) && _this->enabled) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }
        if (listening && _this->enabled) { style::endDisabled(); }
        if (listening && ImGui::Button(CONCAT("Stop##_network_sink_stop_", _this->name), ImVec2(menuWidth, 0)) && _this->enabled) {
            _this->stopServer();
            config.acquire();
            config.conf[_this->name]["listening"] = false;
            config.release(true);
        } else if (!listening && ImGui::Button(CONCAT("Start##_network_sink_stop_", _this->name), ImVec2(menuWidth, 0)) && _this->enabled) {
            _this->startServer();
            config.acquire();
            config.conf[_this->name]["listening"] = true;
            config.release(true);
        }
        ImGui::Text("Status:");
        ImGui::SameLine();
        if (_this->conn && _this->conn->isOpen()) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Sending");
        } else if (listening) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Listening");
        } else {
            ImGui::Text("Idle");
        }
        if (!_this->enabled) {
            style::endDisabled();
        }
    }

    void startServer() {
        conn = net::openUDP("0.0.0.0", port, hostname, port, false);
    }

    void stopServer() {
        if (conn) { conn->close(); }
    }

    static void _constDiagSinkHandler(dsp::complex_t* data, int count, void* ctx) {
        InmarsatcDemodulatorModule* _this = (InmarsatcDemodulatorModule*)ctx;

        dsp::complex_t* cdBuff = _this->constDiag.acquireBuffer();
        if(count == 1024) {
            memcpy(cdBuff, data, count * sizeof(dsp::complex_t));
        }
        _this->constDiag.releaseBuffer();
    }

    static void _demodSinkHandler(uint8_t* data, int count, void* ctx) {
        InmarsatcDemodulatorModule* _this = (InmarsatcDemodulatorModule*)ctx;
        std::lock_guard lck(_this->connMtx);
        if (!_this->conn || !_this->conn->isOpen()) { return; }

        _this->conn->write(count, data);

    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo;

    dsp::demod::InmarsatCDemod mainDemodulator;

    dsp::routing::Splitter<dsp::complex_t> constDiagSplitter;

    dsp::stream<dsp::complex_t> constDiagStream;
    dsp::buffer::Reshaper<dsp::complex_t> constDiagReshaper;
    dsp::sink::Handler<dsp::complex_t> constDiagSink;
    ImGui::ConstellationDiagram constDiag;

    dsp::stream<dsp::complex_t> demodStream;

    dsp::BPSKSymbolExtractor symbolExtractor;
    dsp::buffer::Packer<uint8_t> symbolPacker;
    dsp::sink::Handler<uint8_t> demodSink;
    net::Conn conn;
    std::mutex connMtx;
    char hostname[1024] = "localhost\0";
    int port = 15003;

};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    json def = json({});
    config.setPath(root + "/inmarsatc_demodulator_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new InmarsatcDemodulatorModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (InmarsatcDemodulatorModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
