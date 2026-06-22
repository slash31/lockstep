#include <utils/flog.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <radio_interface.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdio>
// Self-contained rigctl TCP client (POSIX sockets) so lockstep does NOT depend
// on SDR++'s net::rigctl::Client -- that API gained getMode/setMode only in
// recent master, and older installed builds lack those symbols, which would stop
// the whole module from loading. Talking the rigctl wire protocol ourselves keeps
// lockstep loadable across SDR++ versions and gives us full mode sync regardless.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "lockstep",
    /* Description:     */ "Bidirectional rigctl frequency + mode sync (panadapter)",
    /* Author:          */ "KO6NNS",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

static constexpr double FREQ_EPSILON = 1.0;   // Hz; closer than this == "same"

// SDR++ radio module mode -> rigctl mode string (the canonical one to set).
static const std::map<int, std::string> sdrToRigMode = {
    { RADIO_IFACE_MODE_NFM, "FM"  },
    { RADIO_IFACE_MODE_WFM, "WFM" },
    { RADIO_IFACE_MODE_AM,  "AM"  },
    { RADIO_IFACE_MODE_DSB, "DSB" },
    { RADIO_IFACE_MODE_USB, "USB" },
    { RADIO_IFACE_MODE_CW,  "CW"  },
    { RADIO_IFACE_MODE_LSB, "LSB" },
};

// rigctl mode string -> SDR++ mode (folds the rig's variants onto our set).
static int rigModeToSdr(const std::string& m) {
    if (m == "FM" || m == "FMN")    return RADIO_IFACE_MODE_NFM;
    if (m == "WFM")                 return RADIO_IFACE_MODE_WFM;
    if (m == "AM" || m == "AMS")    return RADIO_IFACE_MODE_AM;
    if (m == "USB" || m == "PKTUSB")return RADIO_IFACE_MODE_USB;
    if (m == "LSB" || m == "PKTLSB")return RADIO_IFACE_MODE_LSB;
    if (m == "CW" || m == "CWR")    return RADIO_IFACE_MODE_CW;
    if (m == "DSB")                 return RADIO_IFACE_MODE_DSB;
    return -1;
}

static inline bool almostEqual(double a, double b) { return std::abs(a - b) < FREQ_EPSILON; }

// --- minimal rigctl client ----------------------------------------------------
class RigctlClient {
public:
    bool open(const std::string& host, int port) {
        close();
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) { return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            // Resolve a hostname.
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) { ::close(s); return false; }
            addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        if (::connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) { ::close(s); return false; }
        timeval tv{ 2, 0 };   // 2s recv timeout so a dead link can't hang the worker
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        fd = s;
        rxbuf.clear();
        return true;
    }

    void close() { if (fd >= 0) { ::close(fd); fd = -1; } rxbuf.clear(); }
    bool isOpen() const { return fd >= 0; }

    double getFreq() {                       // Hz, or -1 on error
        if (!sendCmd("f")) { return -1; }
        std::string line;
        if (!readLine(line) || line.rfind("RPRT", 0) == 0) { return -1; }
        try { return std::stod(line); } catch (...) { return -1; }
    }

    bool setFreq(double f) {                  // true on success
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "F %.0f", f);
        return sendCmd(cmd) && readRPRT();
    }

    std::string getMode() {                   // "USB" etc., or "" on error
        if (!sendCmd("m")) { return ""; }
        std::string mode, pb;
        if (!readLine(mode) || mode.rfind("RPRT", 0) == 0) { return ""; }
        readLine(pb);                         // passband line, ignored
        return mode;
    }

    bool setMode(const std::string& mode) {   // passband 0 = rig default for mode
        return sendCmd("M " + mode + " 0") && readRPRT();
    }

private:
    int fd = -1;
    std::string rxbuf;

    bool sendCmd(const std::string& c) {
        if (fd < 0) { return false; }
        std::string s = c + "\n";
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) { return false; }
            off += (size_t)n;
        }
        return true;
    }

    bool readLine(std::string& out) {
        for (;;) {
            auto pos = rxbuf.find('\n');
            if (pos != std::string::npos) {
                out = rxbuf.substr(0, pos);
                rxbuf.erase(0, pos + 1);
                if (!out.empty() && out.back() == '\r') { out.pop_back(); }
                return true;
            }
            char tmp[256];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) { return false; }     // timeout or closed
            rxbuf.append(tmp, (size_t)n);
        }
    }

    bool readRPRT() {
        std::string line;
        return readLine(line) && line.rfind("RPRT 0", 0) == 0;
    }
};

// --- module -------------------------------------------------------------------
class LockstepModule : public ModuleManager::Instance {
public:
    LockstepModule(std::string name) {
        this->name = name;
        strcpy(host, "127.0.0.1");

        config.acquire();
        if (config.conf[name].contains("host")) {
            std::string h = config.conf[name]["host"];
            strcpy(host, h.c_str());
        }
        if (config.conf[name].contains("port")) {
            port = std::clamp<int>(config.conf[name]["port"], 1, 65535);
        }
        if (config.conf[name].contains("pollInterval")) {
            pollInterval = std::clamp<int>(config.conf[name]["pollInterval"], 20, 5000);
        }
        if (config.conf[name].contains("syncMode")) { syncMode = config.conf[name]["syncMode"]; }
        if (config.conf[name].contains("controlRadio")) { controlRadio = config.conf[name]["controlRadio"]; }
        if (config.conf[name].contains("autoStart")) { autoStart = config.conf[name]["autoStart"]; }
        config.release();

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~LockstepModule() {
        stop();
        gui::menu.removeEntry(name);
    }

    void postInit() { if (autoStart) { start(); } }

    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (running) { return; }
        if (worker.joinable()) { worker.join(); }

        if (!rig.open(host, port)) {
            flog::error("[lockstep] Could not connect to {}:{}", host, port);
            return;
        }
        linked = true;

        // Pull the radio's current state into SDR++ FIRST (radio wins at startup),
        // seeding state to the rig's value so the worker never pushes SDR++'s stale
        // freq/mode back onto the radio. A stale mode push in particular would flip
        // an Icom from memory mode to VFO (set_freq alone just retunes the VFO and
        // stays in memory). This runs on the GUI thread (Start button), so tune()
        // applies synchronously here.
        workerErrors = 0;
        initialPull();

        running = true;
        worker = std::thread(&LockstepModule::workerLoop, this);
    }

    void stop() {
        {
            std::lock_guard<std::recursive_mutex> lck(mtx);
            if (!running && !worker.joinable()) { return; }
            running = false;
        }
        cv.notify_all();
        if (worker.joinable()) { worker.join(); }
        std::lock_guard<std::recursive_mutex> lck(mtx);
        rig.close();
        linked = false;
    }

private:
    // Absolute frequency the user is tuned to = SDR center + VFO offset. Using the
    // VFO (not just the center) is what makes "click a signal -> QSY the radio
    // there" work: clicking moves the VFO offset.
    static double currentWaterfallFreq() {
        double f = gui::waterfall.getCenterFrequency();
        std::string vfo = gui::waterfall.selectedVFO;
        if (!vfo.empty() && sigpath::vfoManager.vfoExists(vfo)) {
            f += sigpath::vfoManager.getOffset(vfo);
        }
        return f;
    }

    bool selectedVfoIsRadio() {
        std::string vfo = gui::waterfall.selectedVFO;
        return !vfo.empty() && core::modComManager.getModuleName(vfo) == "radio";
    }

    // One-shot: adopt the radio's freq + mode into SDR++ and seed our state to
    // the rig's values. Read-only w.r.t. the radio (never writes to it), so it
    // can't knock the rig out of memory mode.
    void initialPull() {
        double rf = rig.getFreq();
        std::string vfo = gui::waterfall.selectedVFO;
        if (rf >= 0 && !vfo.empty()) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, vfo, rf);
            rigFreq = rf;
            wfFreq = rf;                 // target; SDR++ settles here, so no push next tick
        }
        else {
            rigFreq = rf;                // may be -1; first tick will just no-op
            wfFreq = currentWaterfallFreq();
        }
        rigMode = -1;
        wfMode = -1;
        if (syncMode && selectedVfoIsRadio()) {
            int sm = rigModeToSdr(rig.getMode());
            if (sm >= 0) {
                core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_SET_MODE, &sm, NULL);
                rigMode = sm;
                wfMode = sm;
            }
        }
    }

    // ---- rig -> SDR++ -------------------------------------------------------
    // Sets `pulled` if it changed SDR++ this tick (freq or mode). The worker uses
    // that to skip the SDR++ -> rig direction this tick: tune()/set-mode applied
    // here land on the GUI thread asynchronously, so reading SDR++ back in the
    // same tick would see a STALE value and wrongly push it onto the radio.
    bool syncFromRig(bool& pulled) {
        double rf = rig.getFreq();
        if (rf < 0) { flog::error("[lockstep] getFreq failed"); return false; }

        if (!almostEqual(rf, rigFreq)) {        // radio moved since last tick
            rigFreq = rf;
            if (!almostEqual(rf, wfFreq)) {
                std::string vfo = gui::waterfall.selectedVFO;
                double oldCenter = gui::waterfall.getCenterFrequency();
                tuner::tune(tuner::TUNER_MODE_NORMAL, vfo, rf);
                if (gui::waterfall.getCenterFrequency() != oldCenter) {
                    tuner::tune(tuner::TUNER_MODE_CENTER, vfo, rf);
                }
                wfFreq = rf;
                pulled = true;
            }
        }

        if (syncMode && selectedVfoIsRadio()) {
            int sm = rigModeToSdr(rig.getMode());
            if (sm >= 0 && sm != rigMode) {
                rigMode = sm;
                if (sm != wfMode) {
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &sm, NULL);
                    wfMode = sm;
                    pulled = true;
                }
            }
        }
        return true;
    }

    // ---- SDR++ -> rig -------------------------------------------------------
    bool syncFromWaterfall() {
        double wf = currentWaterfallFreq();
        if (!almostEqual(wf, wfFreq)) {         // user moved the SDR++ dial
            wfFreq = wf;
            if (!almostEqual(wf, rigFreq)) {
                if (!rig.setFreq(wf)) { flog::error("[lockstep] setFreq({}) failed", wf); return false; }
                rigFreq = wf;
            }
        }

        if (syncMode && selectedVfoIsRadio()) {
            int radioMode = -1;
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &radioMode);
            auto it = sdrToRigMode.find(radioMode);
            if (it != sdrToRigMode.end() && radioMode != wfMode) {
                wfMode = radioMode;
                if (radioMode != rigMode) {
                    rig.setMode(it->second);
                    rigMode = radioMode;
                }
            }
        }
        return true;
    }

    // Pure dual-polling: read both ends each tick, reconcile against shared
    // last-known state. Whichever side changed wins; the freshly-synced value is
    // already "current" on the other read, so no feedback loop forms.
    void workerLoop() {
        std::unique_lock<std::mutex> cvlk(cvMtx);
        while (running) {
            {
                std::lock_guard<std::recursive_mutex> lck(mtx);
                if (!rig.isOpen()) { running = false; linked = false; break; }
                bool pulled = false;
                bool ok = syncFromRig(pulled);
                // Only push SDR++ -> rig when the user is allowed to drive the
                // radio AND we didn't just pull this tick (avoids the stale-read
                // race that would force the rig to VFO mode).
                if (ok && controlRadio && !pulled) { ok = syncFromWaterfall(); }
                if (!ok) {
                    if (++workerErrors > MAX_WORKER_ERRORS) {
                        flog::error("[lockstep] too many rig I/O errors, stopping");
                        running = false;
                        linked = false;
                        break;
                    }
                }
                else { workerErrors = 0; }
            }
            cv.wait_for(cvlk, std::chrono::milliseconds(pollInterval), [this] { return !running; });
        }
    }

    // ---- UI ----------------------------------------------------------------
    static void menuHandler(void* ctx) {
        LockstepModule* _this = (LockstepModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;
        bool running = _this->running;

        if (running) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_lockstep_host_", _this->name), _this->host, 1023)) {
            config.acquire();
            config.conf[_this->name]["host"] = std::string(_this->host);
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_lockstep_port_", _this->name), &_this->port, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }
        ImGui::LeftLabel("Poll (ms)");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##_lockstep_poll_", _this->name), &_this->pollInterval, 10, 100)) {
            _this->pollInterval = std::clamp<int>(_this->pollInterval, 20, 5000);
            config.acquire();
            config.conf[_this->name]["pollInterval"] = _this->pollInterval;
            config.release(true);
        }
        if (running) { style::endDisabled(); }

        if (ImGui::Checkbox(CONCAT("Tune radio from SDR++##_lockstep_ctl_", _this->name), &_this->controlRadio)) {
            config.acquire();
            config.conf[_this->name]["controlRadio"] = _this->controlRadio;
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(off = read-only; never write to the rig)");
        if (ImGui::Checkbox(CONCAT("Sync mode (USB/LSB/CW/FM...)##_lockstep_sm_", _this->name), &_this->syncMode)) {
            config.acquire();
            config.conf[_this->name]["syncMode"] = _this->syncMode;
            config.release(true);
        }
        if (ImGui::Checkbox(CONCAT("Start on launch##_lockstep_auto_", _this->name), &_this->autoStart)) {
            config.acquire();
            config.conf[_this->name]["autoStart"] = _this->autoStart;
            config.release(true);
        }

        ImGui::FillWidth();
        if (running && ImGui::Button(CONCAT("Stop##_lockstep_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!running && ImGui::Button(CONCAT("Start##_lockstep_start_", _this->name), ImVec2(menuWidth, 0))) {
            _this->start();
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (_this->running && _this->linked) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Locked");
        }
        else if (_this->running) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Connecting");
        }
        else {
            ImGui::TextUnformatted("Idle");
        }
    }

    std::string name;
    bool enabled = true;
    std::atomic<bool> running = { false };
    std::atomic<bool> linked = { false };

    // Config
    char host[1024];
    int port = 4532;
    int pollInterval = 250;
    bool syncMode = true;
    bool controlRadio = true;   // false = follow only (never write to the rig)
    bool autoStart = false;

    // Runtime
    RigctlClient rig;
    std::recursive_mutex mtx;
    std::thread worker;
    std::condition_variable cv;
    std::mutex cvMtx;

    // Last-known reconciled state (heart of the echo-free dual poll)
    double rigFreq = -1.0;
    double wfFreq = -1.0;
    int rigMode = -1;            // SDR++ mode space (RADIO_IFACE_MODE_*), -1 = unknown
    int wfMode = -1;

    int workerErrors = 0;
    static constexpr int MAX_WORKER_ERRORS = 16;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/lockstep_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new LockstepModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (LockstepModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
