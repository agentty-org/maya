// hacker.cpp — Cyberpunk hacker terminal (simple run() API)
//
// A movie-style "hacking" terminal: rapid scrolling data, flashing alerts,
// network intrusion simulation, hex dumps, progress bars, sparklines,
// heatmaps. Pure eye candy.
//
// Controls:
//   space       initiate breach sequence
//   e           extract data (rapid hex dump)
//   c           cover tracks (dim + delete log entries)
//   1           green theme
//   2           amber theme
//   3           cyan theme
//   q/Esc       quit
//
// Usage:  ./maya_hacker

#include <maya/host/run.hpp>
#include <maya/maya.hpp>

#include <chrono>
#include <variant>
#include <maya/widget/badge.hpp>
#include <maya/widget/heatmap.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/agentty/sparkline.hpp>
#include <maya/widget/toast.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace maya::dsl;

// ── RNG ────────────────────────────────────────────────────────────────────


// ── Theme ──────────────────────────────────────────────────────────────────

struct Theme {
    const char* name;
    uint8_t primary[3];
    uint8_t bright[3];
    uint8_t dim[3];
    uint8_t accent[3];
    uint8_t alert[3];
    uint8_t border[3];
};

static const Theme themes[] = {
    {"PHOSPHOR",
     {0, 255, 65},    {0, 255, 136},   {0, 100, 30},
     {0, 200, 100},   {255, 50, 50},   {0, 60, 20}},
    {"AMBER",
     {255, 176, 0},   {255, 220, 80},  {140, 90, 0},
     {255, 200, 60},  {255, 60, 60},   {80, 55, 0}},
    {"ICE",
     {0, 200, 255},   {100, 220, 255}, {0, 80, 130},
     {0, 160, 220},   {255, 50, 80},   {0, 40, 70}},
};

static maya::Style fg_t(const uint8_t c[3]) {
    return maya::Style{}.with_fg(maya::Color::rgb(c[0], c[1], c[2]));
}
static maya::Color col_t(const uint8_t c[3]) {
    return maya::Color::rgb(c[0], c[1], c[2]);
}

struct Target {
    std::string ip;
    std::string hostname;
    int port;
    std::string service;
    std::string status;   // "SCANNING", "OPEN", "EXPLOITED", "LOCKED"
    int vuln;             // 0=none, 1=MED, 2=HIGH, 3=CRITICAL
    float scan_progress;
    float age;
};


struct LogEntry {
    std::string timestamp;
    std::string message;
    int level;       // 0=info, 1=success, 2=warning, 3=error
    float opacity;   // 1.0=normal, fading to 0
};


// ── Hex helpers ────────────────────────────────────────────────────────────

// Everything the screen shows, and the RNG that drives it.
struct Model {
    int theme_idx = 0;
    std::vector<Target> targets;
    std::vector<LogEntry> terminal_log;
    std::array<float, 20> inbound_spark{};
    std::array<float, 20> outbound_spark{};
    int spark_idx = 0;
    float crack_progress = 0.0f;
    std::vector<std::vector<float>> heatmap_data;
    float cpu_load = 0.45f;
    float mem_load = 0.62f;
    float net_load = 0.38f;
    float disk_io = 0.25f;
    std::string hex_dump_line;
    maya::ToastManager toasts{{.duration = 3.5f, .fade_time = 0.8f, .max_visible = 3}};
    float elapsed = 0;
    int frame = 0;
    float next_target_time = 2.0f;
    float next_toast_time = 4.0f;
    bool breaching = false;
    float breach_timer = 0;
    int breach_phase = 0;
    bool covering = false;
    float cover_timer = 0;
    bool extracting = false;
    float extract_timer = 0;

    // Per-frame noise the view shows; rolled in tick() so view() stays pure.
    std::string noise_ip, noise_hash, noise_addr, noise_ascii;
    int noise_lat = 0;

    std::mt19937 rng{std::random_device{}()};
    int   randi(int lo, int hi)     { return std::uniform_int_distribution<int>(lo, hi)(rng); }
    float randf(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
};

static char rand_hex(Model& m) {
    static const char hx[] = "0123456789abcdef";
    return hx[m.rng() % 16];
}

static std::string rand_hex_str(Model& m, int n) {
    std::string s;
    s.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) s += rand_hex(m);
    return s;
}

static std::string rand_ip(Model& m) {
    return std::to_string(m.randi(10, 223)) + "." +
           std::to_string(m.randi(0, 255)) + "." +
           std::to_string(m.randi(0, 255)) + "." +
           std::to_string(m.randi(1, 254));
}

static std::string rand_hostname(Model& m) {
    static const char* prefixes[] = {"srv","node","db","proxy","gw","vpn","fw","core","edge","cache"};
    static const char* suffixes[] = {".corp.net",".darknet.io",".shadow.sys",".zero.lan",".ghost.onion"};
    return std::string(prefixes[m.randi(0, 9)]) + "-" +
           std::to_string(m.randi(1, 99)) + suffixes[m.randi(0, 4)];
}

// ── Block bar ──────────────────────────────────────────────────────────────

static std::string block_bar(float v, int width) {
    int filled = std::clamp(static_cast<int>(v * static_cast<float>(width)), 0, width);
    std::string s;
    for (int i = 0; i < filled; ++i) s += "\xe2\x96\x88";  // █
    for (int i = filled; i < width; ++i) s += "\xe2\x96\x91"; // ░
    return s;
}

// ── Spinners ───────────────────────────────────────────────────────────────

static const char* dot_spin(int frame) {
    static const char* frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    return frames[frame % 10];
}

// ── Data Model ─────────────────────────────────────────────────────────────

// ── State ──────────────────────────────────────────────────────────────────

static constexpr int MAX_LOG = 18;

// Right panel data

// Hex dump for bottom

// Toast manager

// Timing

// Breach state

// Cover tracks state

// Extract state

// ── Log helpers ────────────────────────────────────────────────────────────

static std::string timestamp(const Model& m) {
    int total = static_cast<int>(m.elapsed);
    int hh = (total / 3600) % 24;
    int mm = (total / 60) % 60;
    int ss = total % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mm, ss);
    return buf;
}

static void add_log(Model& m, const std::string& msg, int level = 0) {
    m.terminal_log.push_back({timestamp(m), msg, level, 1.0f});
    if (m.terminal_log.size() > MAX_LOG)
        m.terminal_log.erase(m.terminal_log.begin());
}

// ── Init ───────────────────────────────────────────────────────────────────

static const char* services[] = {"ssh","http","https","mysql","redis","postgres","ftp","smtp","dns","telnet"};
static const int ports[] = {22, 80, 443, 3306, 6379, 5432, 21, 25, 53, 23};

static void init_state(Model& m) {
    // Initial m.targets
    for (int i = 0; i < 5; ++i) {
        int si = m.randi(0, 9);
        m.targets.push_back({
            rand_ip(m), rand_hostname(m), ports[si], services[si],
            i < 3 ? "OPEN" : "SCANNING",
            m.randi(0, 3),
            i < 3 ? 1.0f : m.randf(0.1f, 0.7f),
            m.randf(10.0f, 120.0f)
        });
    }

    // Initial log
    add_log(m, "NEXUS://BREACH v4.2.0 initialized", 1);
    add_log(m, "Loading exploit database... 2,847 modules", 0);
    add_log(m, "Establishing encrypted tunnel via TOR", 0);
    add_log(m, "Proxy chain: 3 hops active", 1);
    add_log(m, "Target acquisition mode: ACTIVE", 2);

    // Init heatmap (6x8 grid)
    m.heatmap_data.resize(6);
    for (auto& row : m.heatmap_data) {
        row.resize(8);
        for (auto& v : row) v = m.randf(0.0f, 0.6f);
    }

    // Init hex dump
    m.hex_dump_line.reserve(80);
    for (int i = 0; i < 48; ++i) m.hex_dump_line += rand_hex(m);
}

// ── Tick ───────────────────────────────────────────────────────────────────

static const std::array<std::string, 24> log_templates = {
    "Scanning port %PORT%... OPEN",
    "Injecting payload 0x%HEX8%...",
    "Decrypting RSA-4096 block %HEX4%",
    "Brute forcing %SVC% credentials",
    "Intercepted packet from %IP%",
    "Tunneling through proxy node %N%",
    "Buffer overflow at 0x%HEX8%",
    "Shellcode deployed: %N% bytes",
    "Privilege escalation: uid=0(root)",
    "Dumping /etc/shadow... %N% entries",
    "Cracking hash: %HEX8%%HEX4%",
    "SQL injection on port %PORT%",
    "Reverse shell established %IP%:%PORT%",
    "ARP spoofing gateway %IP%",
    "DNS rebinding attack active",
    "Extracting certificates from %SVC%",
    "Patching kernel module 0x%HEX8%",
    "Keylogger installed PID %N%",
    "Exfiltrating %N%MB via covert channel",
    "Side-channel timing attack on AES",
    "Race condition exploit running...",
    "Heap spray: %N% allocations",
    "ROP chain: %N% gadgets linked",
    "Zero-day CVE-2026-%HEX4% triggered",
};

static std::string expand_template(Model& m, const std::string& tmpl) {
    std::string out;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '%' && i + 1 < tmpl.size()) {
            size_t end = tmpl.find('%', i + 1);
            if (end != std::string::npos) {
                std::string tag = tmpl.substr(i + 1, end - i - 1);
                if (tag == "PORT") out += std::to_string(ports[m.randi(0, 9)]);
                else if (tag == "HEX8") out += rand_hex_str(m, 8);
                else if (tag == "HEX4") out += rand_hex_str(m, 4);
                else if (tag == "IP") out += rand_ip(m);
                else if (tag == "SVC") out += services[m.randi(0, 9)];
                else if (tag == "N") out += std::to_string(m.randi(1, 9999));
                else { out += '%'; out += tag; out += '%'; }
                i = end;
                continue;
            }
        }
        out += tmpl[i];
    }
    return out;
}

static const std::array<std::string, 10> toast_messages = {
    "FIREWALL DETECTED",
    "ENCRYPTING CHANNEL",
    "BACKDOOR INSTALLED",
    "IDS ALERT BYPASSED",
    "PAYLOAD DELIVERED",
    "ROOT ACCESS OBTAINED",
    "EVIDENCE DESTROYED",
    "PROXY CHAIN ROTATED",
    "MEMORY WIPED",
    "TRACE ELIMINATED",
};

static void tick(Model& m, float dt) {
    m.elapsed += dt;
    m.frame++;
    m.noise_ip   = rand_ip(m);
    m.noise_lat  = m.randi(12, 350);
    m.noise_hash = rand_hex_str(m, 6);
    m.noise_addr = rand_hex_str(m, 4);
    m.noise_ascii.clear();
    for (int i = 0; i < 16; ++i) m.noise_ascii += static_cast<char>(m.randi(33, 126));

    float fdt = dt;

    // Update sparklines
    if (m.frame % 3 == 0) {
        m.inbound_spark[static_cast<size_t>(m.spark_idx)] = m.randf(0.1f, 1.0f);
        m.outbound_spark[static_cast<size_t>(m.spark_idx)] = m.randf(0.05f, 0.7f);
        m.spark_idx = (m.spark_idx + 1) % 20;
    }

    // Update crack progress
    m.crack_progress += m.randf(0.001f, 0.004f) * dt * 15.0f;
    if (m.crack_progress > 1.0f) m.crack_progress = m.randf(0.0f, 0.15f);

    // Update heatmap
    for (auto& row : m.heatmap_data) {
        for (auto& v : row) {
            v += m.randf(-0.05f, 0.06f);
            v = std::clamp(v, 0.0f, 1.0f);
        }
    }
    // Hot spots that drift
    int hr = m.randi(0, 5);
    int hc = m.randi(0, 7);
    m.heatmap_data[static_cast<size_t>(hr)][static_cast<size_t>(hc)] =
        std::clamp(m.heatmap_data[static_cast<size_t>(hr)][static_cast<size_t>(hc)] + 0.15f, 0.0f, 1.0f);

    // System loads
    m.cpu_load += m.randf(-0.03f, 0.04f); m.cpu_load = std::clamp(m.cpu_load, 0.1f, 0.99f);
    m.mem_load += m.randf(-0.02f, 0.02f); m.mem_load = std::clamp(m.mem_load, 0.3f, 0.95f);
    m.net_load += m.randf(-0.04f, 0.05f); m.net_load = std::clamp(m.net_load, 0.05f, 0.95f);
    m.disk_io  += m.randf(-0.03f, 0.03f); m.disk_io  = std::clamp(m.disk_io, 0.05f, 0.80f);

    // Rotate hex dump
    m.hex_dump_line.clear();
    for (int i = 0; i < 64; ++i) m.hex_dump_line += rand_hex(m);

    // Add log entries
    if (m.randi(0, 4) == 0) {
        int idx = m.randi(0, 23);
        int lvl = (m.randi(0, 6) == 0) ? 1 : (m.randi(0, 8) == 0 ? 2 : 0);
        add_log(m, expand_template(m, log_templates[static_cast<size_t>(idx)]), lvl);
    }

    // Occasionally add a progress bar log
    if (m.randi(0, 20) == 0) {
        float pct = m.randf(0.2f, 0.98f);
        int filled = static_cast<int>(pct * 20);
        std::string bar;
        for (int i = 0; i < 20; ++i) bar += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";
        char buf[16];
        std::snprintf(buf, sizeof(buf), " %d%%", static_cast<int>(pct * 100));
        add_log(m, bar + buf + " Decrypting...", 0);
    }

    // Occasionally add hex dump log
    if (m.randi(0, 15) == 0) {
        std::string hex = "0x" + rand_hex_str(m, 4) + ": ";
        for (int i = 0; i < 8; ++i) {
            hex += rand_hex_str(m, 2);
            if (i < 7) hex += " ";
        }
        add_log(m, hex, 0);
    }

    // Discover new m.targets
    if (m.elapsed > m.next_target_time) {
        m.next_target_time = m.elapsed + m.randf(3.0f, 8.0f);
        if (m.targets.size() < 12) {
            int si = m.randi(0, 9);
            m.targets.push_back({
                rand_ip(m), rand_hostname(m), ports[si], services[si],
                "SCANNING", m.randi(0, 3), 0.0f, 0.0f
            });
            add_log(m, "New target discovered: " + m.targets.back().ip + " (" + m.targets.back().hostname + ")", 1);
        }
    }

    // Advance scanning m.targets
    for (auto& t : m.targets) {
        t.age += dt;
        if (t.status == "SCANNING") {
            t.scan_progress += m.randf(0.01f, 0.05f);
            if (t.scan_progress >= 1.0f) {
                t.scan_progress = 1.0f;
                t.status = "OPEN";
                t.vuln = m.randi(1, 3);
                add_log(m, "Port " + std::to_string(t.port) + "/" + t.service +
                        " OPEN on " + t.ip, 1);
            }
        }
    }

    // Toast notifications
    if (m.elapsed > m.next_toast_time) {
        m.next_toast_time = m.elapsed + m.randf(5.0f, 12.0f);
        auto lvl = m.randi(0, 3) == 0 ? maya::ToastLevel::Error :
                   m.randi(0, 2) == 0 ? maya::ToastLevel::Warning :
                                      maya::ToastLevel::Success;
        m.toasts.push(toast_messages[static_cast<size_t>(m.randi(0, 9))], lvl);
    }
    // Toast expiry is clock-driven — nothing to advance.

    // Breach sequence
    if (m.breaching) {
        m.breach_timer -= dt;
        if (m.breach_timer <= 0) {
            m.breach_phase++;
            m.breach_timer = m.randf(0.5f, 1.5f);
            if (m.breach_phase == 1) {
                add_log(m, ">>> BREACH SEQUENCE INITIATED <<<", 3);
                add_log(m, "Probing target defenses...", 2);
            } else if (m.breach_phase == 2) {
                add_log(m, "Firewall rule injection: COMPLETE", 1);
                add_log(m, "Escalating privileges...", 2);
            } else if (m.breach_phase == 3) {
                add_log(m, "Root shell obtained on " + (m.targets.empty() ? "unknown" : m.targets[0].ip), 1);
                m.toasts.push("ACCESS GRANTED", maya::ToastLevel::Success);
            } else if (m.breach_phase == 4) {
                add_log(m, "Installing persistent backdoor...", 0);
                add_log(m, "Modifying syslog to hide traces", 0);
            } else if (m.breach_phase >= 5) {
                add_log(m, ">>> BREACH COMPLETE <<<", 1);
                if (!m.targets.empty()) m.targets[0].status = "EXPLOITED";
                m.breaching = false;
                m.breach_phase = 0;
            }
        }
    }

    // Cover tracks
    if (m.covering) {
        m.cover_timer -= dt;
        if (m.cover_timer <= 0) {
            m.covering = false;
        }
        // Fade out log entries
        for (auto& e : m.terminal_log) {
            e.opacity -= dt * 0.8f;
            if (e.opacity < 0.1f) e.opacity = 0.1f;
        }
        if (!m.covering) {
            // Remove faded entries
            int to_remove = std::min(static_cast<int>(m.terminal_log.size()), m.randi(3, 8));
            for (int i = 0; i < to_remove; ++i) {
                if (!m.terminal_log.empty())
                    m.terminal_log.erase(m.terminal_log.begin());
            }
            add_log(m, "Tracks covered. " + std::to_string(to_remove) + " log entries purged.", 1);
            m.toasts.push("EVIDENCE DESTROYED", maya::ToastLevel::Warning);
        }
    }

    // Extract data
    if (m.extracting) {
        m.extract_timer -= dt;
        if (m.extract_timer <= 0) {
            m.extracting = false;
            add_log(m, "Extraction complete: " + std::to_string(m.randi(128, 4096)) + " MB exfiltrated", 1);
            m.toasts.push("DATA EXFILTRATED", maya::ToastLevel::Success);
        } else {
            // Rapid hex dump lines
            for (int i = 0; i < 3; ++i) {
                std::string line = "0x" + rand_hex_str(m, 8) + ": ";
                for (int j = 0; j < 8; ++j) {
                    line += rand_hex_str(m, 4) + " ";
                }
                add_log(m, line, 0);
            }
        }
    }
}

// ── UI Builders ────────────────────────────────────────────────────────────

static maya::Element build_header(const Model& m) {
    auto spin = std::string(dot_spin(m.frame));
    bool blink = (m.frame / 8) % 2 == 0;

    std::string status_dot = blink ? "●" : "○";
    std::string conn = "TOR x3 | PROXY: " + m.noise_ip + " | LAT: " +
                       std::to_string(m.noise_lat) + "ms";

    return (h(
        text(spin, fg_t(themes[m.theme_idx].primary)) | w_<2>,
        text("NEXUS://BREACH", fg_t(themes[m.theme_idx].bright).with_bold()) | w_<16>,
        text("v4.2.0") | Dim | w_<7>,
        text(status_dot, blink ? fg_t(themes[m.theme_idx].primary) : fg_t(themes[m.theme_idx].dim)),
        text(" CONNECTED", fg_t(themes[m.theme_idx].primary)) | w_<11>,
        space,
        text(conn, fg_t(themes[m.theme_idx].dim)) | clip,
        space,
        text(themes[m.theme_idx].name, fg_t(themes[m.theme_idx].accent).with_bold()) | w_<10>,
        text("0x" + m.noise_hash, fg_t(themes[m.theme_idx].dim))
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_targets_panel(const Model& m) {
    std::vector<maya::Element> rows;

    // Header
    rows.push_back((h(
        t<"IP/HOST"> | Bold | Dim | w_<20>,
        t<"PORT"> | Bold | Dim | w_<6>,
        t<"STATUS"> | Bold | Dim | w_<10>,
        t<"VULN"> | Bold | Dim
    ) | gap_<1>).build());

    for (auto& t : m.targets) {
        // Status color
        maya::Style status_style;
        if (t.status == "EXPLOITED")
            status_style = fg_t(themes[m.theme_idx].primary).with_bold();
        else if (t.status == "OPEN")
            status_style = maya::Style{}.with_fg(maya::Color::rgb(100, 200, 255));
        else if (t.status == "SCANNING")
            status_style = fg_t(themes[m.theme_idx].dim);
        else
            status_style = maya::Style{}.with_fg(maya::Color::rgb(255, 60, 60));

        std::string status_str = t.status;
        if (t.status == "SCANNING") {
            int pct = static_cast<int>(t.scan_progress * 100);
            status_str += " " + std::to_string(pct) + "%";
        }

        // Vulnerability badge
        maya::Element vuln_elem = text("").build();
        if (t.vuln == 3) {
            vuln_elem = maya::Badge::error("CRIT").build();
        } else if (t.vuln == 2) {
            vuln_elem = maya::Badge::warning("HIGH").build();
        } else if (t.vuln == 1) {
            maya::Badge::Config cfg;
            cfg.style = maya::Style{}.with_fg(maya::Color::rgb(229, 192, 123));
            vuln_elem = maya::Badge("MED", cfg).build();
        }

        // Truncate hostname if needed
        std::string display = t.ip;
        if (display.size() < 18) {
            display = t.hostname;
            if (display.size() > 18) display = display.substr(0, 18);
        }

        rows.push_back((h(
            text(display, fg_t(themes[m.theme_idx].accent)) | w_<20>,
            text(std::to_string(t.port), fg_t(themes[m.theme_idx].dim)) | w_<6>,
            text(status_str, status_style) | w_<10>,
            std::move(vuln_elem)
        ) | gap_<1>).build());
    }

    // Fill empty slots
    while (rows.size() < 10)
        rows.push_back(text("").build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(col_t(themes[m.theme_idx].border))
        .border_text(" TARGETS ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_terminal_panel(const Model& m) {
    std::vector<maya::Element> rows;

    for (auto& e : m.terminal_log) {
        maya::Style ts_style = fg_t(themes[m.theme_idx].dim);
        maya::Style msg_style;

        if (e.level == 1) msg_style = fg_t(themes[m.theme_idx].primary).with_bold();
        else if (e.level == 2) msg_style = maya::Style{}.with_fg(maya::Color::rgb(255, 200, 60)).with_bold();
        else if (e.level == 3) msg_style = maya::Style{}.with_fg(maya::Color::rgb(255, 50, 50)).with_bold();
        else msg_style = fg_t(themes[m.theme_idx].primary);

        if (e.opacity < 0.5f) {
            ts_style = ts_style.with_dim();
            msg_style = msg_style.with_dim();
        }

        rows.push_back((h(
            text("[" + e.timestamp + "]", ts_style) | w_<12>,
            text(e.message, msg_style) | clip
        ) | gap_<1>).build());
    }

    // Fill remaining with empty
    while (rows.size() < MAX_LOG)
        rows.push_back(text("").build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(col_t(themes[m.theme_idx].border))
        .border_text(" TERMINAL ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_intel_panel(const Model& m) {
    std::vector<maya::Element> rows;

    // --- Network traffic sparklines ---
    rows.push_back(text("NETWORK TRAFFIC", fg_t(themes[m.theme_idx].bright).with_bold()).build());

    // Reorder sparkline data from ring buffer
    auto reorder_spark = [](const std::array<float, 20>& arr, int idx) {
        std::vector<float> out(20);
        for (int i = 0; i < 20; ++i)
            out[static_cast<size_t>(i)] = arr[static_cast<size_t>((idx + i) % 20)];
        return out;
    };

    auto in_data = reorder_spark(m.inbound_spark, m.spark_idx);
    maya::Sparkline in_spark(in_data, {.color = col_t(themes[m.theme_idx].primary)});
    in_spark.set_label("IN ");
    in_spark.set_show_last(true);
    rows.push_back(in_spark.build());

    auto out_data = reorder_spark(m.outbound_spark, m.spark_idx);
    maya::Sparkline out_spark(out_data, {.color = col_t(themes[m.theme_idx].accent)});
    out_spark.set_label("OUT");
    out_spark.set_show_last(true);
    rows.push_back(out_spark.build());

    rows.push_back(text("").build());

    // --- Password cracking progress ---
    rows.push_back(text("PASSWORD CRACK", fg_t(themes[m.theme_idx].bright).with_bold()).build());
    maya::ProgressBar crack_bar({
        .width = 24,
        .fill_color = col_t(themes[m.theme_idx].primary),
        .bg_color = col_t(themes[m.theme_idx].border),
    });
    crack_bar.set(m.crack_progress);
    crack_bar.set_label("bcrypt");
    rows.push_back(crack_bar.build());

    rows.push_back(text("").build());

    // --- Network topology heatmap ---
    rows.push_back(text("NET TOPOLOGY", fg_t(themes[m.theme_idx].bright).with_bold()).build());
    maya::Heatmap hm(m.heatmap_data);
    hm.set_low_color(col_t(themes[m.theme_idx].border));
    hm.set_high_color(col_t(themes[m.theme_idx].primary));
    rows.push_back(hm.build());

    rows.push_back(text("").build());

    // --- System load gauges ---
    rows.push_back(text("SYSTEM LOAD", fg_t(themes[m.theme_idx].bright).with_bold()).build());

    auto gauge_line = [](const char* label, float val, const uint8_t color[3]) {
        auto bar = block_bar(val, 12);
        char pct[8];
        std::snprintf(pct, sizeof(pct), "%3d%%", static_cast<int>(val * 100));
        maya::Style bar_style;
        if (val > 0.8f) bar_style = maya::Style{}.with_fg(maya::Color::rgb(255, 60, 60));
        else if (val > 0.5f) bar_style = maya::Style{}.with_fg(maya::Color::rgb(255, 200, 60));
        else bar_style = maya::Style{}.with_fg(maya::Color::rgb(color[0], color[1], color[2]));
        return (h(
            text(label) | Dim | w_<5>,
            text(bar, bar_style) | w_<12>,
            text(pct) | Dim
        ) | gap_<1>).build();
    };

    rows.push_back(gauge_line("CPU", m.cpu_load, themes[m.theme_idx].primary));
    rows.push_back(gauge_line("MEM", m.mem_load, themes[m.theme_idx].primary));
    rows.push_back(gauge_line("NET", m.net_load, themes[m.theme_idx].primary));
    rows.push_back(gauge_line("I/O", m.disk_io, themes[m.theme_idx].primary));

    return vstack().border(maya::BorderStyle::Round)
        .border_color(col_t(themes[m.theme_idx].border))
        .border_text(" INTEL ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_hex_footer(const Model& m) {
    // Format hex dump as "0000: XX XX XX XX XX XX XX XX  |ascii...|"
    std::string addr = "0x" + m.noise_addr + ": ";
    std::string hex_part;
    std::string ascii_part = "|";
    for (int i = 0; i < 16; ++i) {
        size_t idx = static_cast<size_t>(i * 2);
        if (idx + 1 < m.hex_dump_line.size()) {
            hex_part += m.hex_dump_line.substr(idx, 2);
            if (i < 15) hex_part += " ";
            // Fake ASCII
            if (static_cast<size_t>(i) < m.noise_ascii.size()) ascii_part += m.noise_ascii[static_cast<size_t>(i)];
        }
    }
    ascii_part += "|";

    return (h(
        text(addr + hex_part + "  " + ascii_part, fg_t(themes[m.theme_idx].dim)) | clip,
        space,
        text(" SPC", fg_t(themes[m.theme_idx].bright).with_bold()) | w_<4>, text(":breach") | Fg<120, 120, 140> | w_<8>,
        text("e", fg_t(themes[m.theme_idx].bright).with_bold()) | w_<2>, text(":extract") | Fg<120, 120, 140> | w_<9>,
        text("c", fg_t(themes[m.theme_idx].bright).with_bold()) | w_<2>, text(":cover") | Fg<120, 120, 140> | w_<7>,
        text("1-3", fg_t(themes[m.theme_idx].bright).with_bold()) | w_<4>, text(":theme") | Fg<120, 120, 140> | w_<7>,
        text("q", fg_t(themes[m.theme_idx].bright).with_bold()) | w_<2>, text(":quit ") | Fg<120, 120, 140>
    ) | pad<0, 1, 0, 1> | Bg<20, 20, 30>).build();
}

// ── Main ───────────────────────────────────────────────────────────────────

// ── Program ────────────────────────────────────────────────────────────────

struct Tick {};
struct SetTheme { int idx; };
struct Breach {};
struct Extract {};
struct Cover {};
struct Quit {};
using Msg = std::variant<Tick, SetTheme, Breach, Extract, Cover, Quit>;

struct Hacker {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg>;

    static Cmd init(Model& m)               { init_state(m); tick(m, 0.f); return {}; }
    static Cmd update(Model& m, Tick)       { tick(m, 1.0f / 15.0f); return {}; }
    static Cmd update(Model& m, SetTheme t) { m.theme_idx = t.idx; return {}; }
    static Cmd update(Model&, Quit)         { return Cmd::quit(0); }
    static Cmd update(Model& m, Breach) {
        if (m.breaching) return {};
        m.breaching = true;
        m.breach_timer = 0.3f;
        m.breach_phase = 0;
        m.toasts.push("BREACH SEQUENCE INITIATED", maya::ToastLevel::Error);
        return {};
    }
    static Cmd update(Model& m, Extract) {
        if (m.extracting) return {};
        m.extracting = true;
        m.extract_timer = 3.0f;
        add_log(m, ">>> DATA EXTRACTION STARTED <<<", 3);
        m.toasts.push("EXTRACTING DATA", maya::ToastLevel::Warning);
        return {};
    }
    static Cmd update(Model& m, Cover) {
        if (m.covering) return {};
        m.covering = true;
        m.cover_timer = 2.0f;
        add_log(m, ">>> COVERING TRACKS <<<", 2);
        m.toasts.push("WIPING EVIDENCE", maya::ToastLevel::Warning);
        return {};
    }

    static maya::Element view(const Model& m) {
        auto main_row = hstack()(
            vstack().grow(1)(build_targets_panel(m)),
            vstack().grow(2)(build_terminal_panel(m)),
            vstack().grow(1)(build_intel_panel(m)));
        return vstack()(build_header(m), std::move(main_row), m.toasts.build(), build_hex_footer(m));
    }

    static Sub subscribe(const Model&) {
        return Sub::every(std::chrono::milliseconds{66}, Tick{});
    }
    static std::optional<Msg> on(const Model&, const maya::KeyEvent& k) {
        using maya::SpecialKey;
        static const maya::keymap<Msg> keys{
            {'q', Quit{}}, {SpecialKey::Escape, Quit{}}, {'1', SetTheme{0}}, {'2', SetTheme{1}},
            {'3', SetTheme{2}}, {' ', Breach{}}, {'e', Extract{}}, {'c', Cover{}},
        };
        return keys(k);
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(maya::Program<Hacker>);

int main() { return maya::run<Hacker>({.title = "NEXUS://BREACH"}); }
