// deploy.cpp — CI/CD Deployment Pipeline Dashboard
//
// A real-time animated deployment pipeline dashboard showing multiple
// microservices being built, tested, and deployed with rich UI widgets.
//
// Controls:
//   space       trigger new deployment wave
//   r           rollback last deployment
//   f           force-deploy (skip failed stages)
//   1           switch to dev environment
//   2           switch to staging environment
//   3           switch to prod environment
//   q/Esc       quit
//
// Usage:  ./maya_deploy

#include <maya/host/run.hpp>
#include <maya/maya.hpp>

#include <chrono>
#include <variant>
#include <maya/widget/agentty/sparkline.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// -- Helpers -----------------------------------------------------------------


static maya::Style fg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Style{}.with_fg(maya::Color::rgb(r, g, b));
}

// -- Spinner frames ----------------------------------------------------------

static const char* dot_spin(int frame) {
    static const char* frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    return frames[frame % 10];
}

// -- Block bar ---------------------------------------------------------------

static std::string block_bar(float v, int width) {
    int filled = std::clamp(static_cast<int>(v * static_cast<float>(width)), 0, width);
    std::string s;
    for (int i = 0; i < filled; ++i) s += "█";
    for (int i = filled; i < width; ++i) s += "─";
    return s;
}

// -- Pipeline Stage definitions ----------------------------------------------

enum class StageStatus : uint8_t {
    Pending,
    Running,
    Success,
    Failed,
    Skipped,
    RollingBack,
};

struct PipelineStage {
    std::string name;
    StageStatus status = StageStatus::Pending;
    float progress     = 0.0f;
    float elapsed      = 0.0f;
    float duration     = 0.0f;   // total expected duration
    float fail_chance  = 0.0f;   // probability of failure
};

struct Service {
    std::string name;
    std::string icon;
    std::vector<PipelineStage> stages;
    int current_stage      = -1;   // -1 = not started
    bool deploying         = false;
    bool rollback          = false;
    int deploy_count       = 0;
    float total_time       = 0.0f;
    std::string version;
};

struct LogEntry {
    float timestamp;
    std::string msg;
    int level; // 0=info, 1=warn, 2=err, 3=success
};

// -- Environment -------------------------------------------------------------

enum class Env : uint8_t { Dev, Staging, Prod };
static const char* env_names[] = {"DEV", "STAGING", "PROD"};
static const uint8_t env_colors[][3] = {
    {100, 200, 255},  // dev: blue
    {255, 200, 60},   // staging: yellow
    {255, 80, 80},    // prod: red
};

// -- Global State ------------------------------------------------------------

static constexpr int MAX_LOGS = 10;

// Metrics history for sparklines
static constexpr int SPARK_SIZE = 24;

// -- Helpers -----------------------------------------------------------------

// Everything the screen shows, and the RNG that drives it.
struct Model {
    std::vector<Service> services;
    std::vector<LogEntry> logs;
    Env current_env = Env::Staging;
    float uptime = 0.0f;
    int frame_count = 0;
    int total_deploys = 0;
    int total_failures = 0;
    int total_rollbacks = 0;
    bool force_mode = false;
    std::vector<float> deploy_freq_history;
    std::vector<float> test_pass_history;
    std::vector<float> build_time_history;

    std::mt19937 rng{std::random_device{}()};
    int   randi(int lo, int hi)     { return std::uniform_int_distribution<int>(lo, hi)(rng); }
    float randf(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
};

static void add_log(Model& m, int level, const std::string& msg) {
    m.logs.push_back({m.uptime, msg, level});
    if (m.logs.size() > MAX_LOGS)
        m.logs.erase(m.logs.begin());
}

static std::string fmt_time(float secs) {
    if (secs < 0.1f) return "0.0s";
    if (secs < 60.0f) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(secs));
        return buf;
    }
    int mins = static_cast<int>(secs) / 60;
    int s = static_cast<int>(secs) % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%dm%02ds", mins, s);
    return buf;
}

static std::string make_version(Model& m) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "v%d.%d.%d", m.randi(1, 5), m.randi(0, 12), m.randi(0, 99));
    return buf;
}

// -- Stage templates per environment -----------------------------------------

static std::vector<PipelineStage> make_stages(Model& m) {
    float mult = (m.current_env == Env::Prod) ? 1.5f : (m.current_env == Env::Staging) ? 1.0f : 0.6f;
    float fail_mult = (m.current_env == Env::Prod) ? 0.5f : (m.current_env == Env::Dev) ? 1.5f : 1.0f;
    return {
        {"Build",         StageStatus::Pending, 0, 0, m.randf(3.0f, 8.0f) * mult,   0.05f * fail_mult},
        {"Test",          StageStatus::Pending, 0, 0, m.randf(5.0f, 15.0f) * mult,  0.12f * fail_mult},
        {"Security Scan", StageStatus::Pending, 0, 0, m.randf(2.0f, 6.0f) * mult,   0.03f * fail_mult},
        {"Deploy",        StageStatus::Pending, 0, 0, m.randf(4.0f, 10.0f) * mult,  0.08f * fail_mult},
        {"Health Check",  StageStatus::Pending, 0, 0, m.randf(2.0f, 5.0f) * mult,   0.04f * fail_mult},
    };
}

// -- Init --------------------------------------------------------------------

static void init_services(Model& m) {
    m.services = {
        {"api-gateway",   "🌐", {}, -1, false, false, 0, 0, "v2.4.1"},
        {"auth-service",  "🔐", {}, -1, false, false, 0, 0, "v1.8.3"},
        {"data-pipeline", "📊", {}, -1, false, false, 0, 0, "v3.1.0"},
        {"web-frontend",  "🖥 ", {}, -1, false, false, 0, 0, "v4.2.7"},
        {"ml-model",      "🤖", {}, -1, false, false, 0, 0, "v1.0.5"},
    };

    m.deploy_freq_history.resize(SPARK_SIZE, 0.0f);
    m.test_pass_history.resize(SPARK_SIZE, 0.85f);
    m.build_time_history.resize(SPARK_SIZE, 5.0f);
}

// -- Deployment triggers -----------------------------------------------------

static void start_deploy(Model& m, Service& svc) {
    svc.stages = make_stages(m);
    svc.current_stage = 0;
    svc.deploying = true;
    svc.rollback = false;
    svc.total_time = 0.0f;
    svc.version = make_version(m);
    svc.stages[0].status = StageStatus::Running;
    add_log(m, 0, svc.name + " " + svc.version + ": deployment started [" + std::string(env_names[static_cast<int>(m.current_env)]) + "]");
}

static void trigger_deploy_wave(Model& m) {
    // Stagger service starts
    for (size_t i = 0; i < m.services.size(); ++i) {
        auto& svc = m.services[i];
        if (!svc.deploying) {
            start_deploy(m, svc);
        }
    }
    m.total_deploys++;

    // Update sparkline data
    m.deploy_freq_history.erase(m.deploy_freq_history.begin());
    m.deploy_freq_history.push_back(static_cast<float>(m.total_deploys));
}

static void trigger_rollback(Model& m) {
    for (auto& svc : m.services) {
        if (svc.deploying) {
            svc.rollback = true;
            for (auto& stage : svc.stages) {
                if (stage.status == StageStatus::Running) {
                    stage.status = StageStatus::RollingBack;
                }
            }
            add_log(m, 1, svc.name + ": rollback initiated");
        }
    }
    m.total_rollbacks++;
}

// -- Tick (simulation) -------------------------------------------------------

static const std::array<std::string, 20> log_messages = {
    "compiling 247 source files...",
    "linking shared libraries...",
    "running unit tests: 184/184 passed",
    "running integration tests: 42/45 passed",
    "scanning dependencies for vulnerabilities...",
    "CVE check: 0 critical, 2 low severity",
    "building Docker image sha256:a3f2...",
    "pushing to container registry...",
    "updating Kubernetes deployment...",
    "rolling update: 3/3 pods ready",
    "health endpoint /api/health: 200 OK",
    "latency p99: 42ms (threshold: 100ms)",
    "error rate: 0.02% (threshold: 1%)",
    "memory usage: 256MB / 512MB",
    "connection pool: 45/100 active",
    "cache hit ratio: 94.2%",
    "TLS certificate valid: 89 days remaining",
    "load balancer draining old instances...",
    "database migration: 3 pending changes applied",
    "static asset upload: 1.2MB compressed",
};

static void tick(Model& m, float dt) {
    m.uptime += dt;
    m.frame_count++;

    for (auto& svc : m.services) {
        if (!svc.deploying) continue;
        if (svc.current_stage < 0 || svc.current_stage >= static_cast<int>(svc.stages.size())) continue;

        auto& stage = svc.stages[static_cast<size_t>(svc.current_stage)];
        svc.total_time += dt;

        if (stage.status == StageStatus::RollingBack) {
            stage.progress -= dt / (stage.duration * 0.5f);
            if (stage.progress <= 0.0f) {
                stage.progress = 0.0f;
                stage.status = StageStatus::Skipped;
                svc.deploying = false;
                svc.rollback = false;
                add_log(m, 1, svc.name + ": rollback complete");
            }
            continue;
        }

        if (stage.status == StageStatus::Running) {
            stage.elapsed += dt;
            stage.progress = std::clamp(stage.elapsed / stage.duration, 0.0f, 1.0f);

            // Emit log messages occasionally
            if (m.randi(0, 40) == 0) {
                add_log(m, 0, svc.name + "/" + stage.name + ": " +
                    log_messages[static_cast<size_t>(m.randi(0, 19))]);
            }

            // Check for completion
            if (stage.elapsed >= stage.duration) {
                // Check for failure
                bool failed = m.randf(0.0f, 1.0f) < stage.fail_chance && !m.force_mode;
                if (failed) {
                    stage.status = StageStatus::Failed;
                    stage.progress = stage.elapsed / stage.duration;
                    svc.deploying = false;
                    m.total_failures++;
                    add_log(m, 2, svc.name + "/" + stage.name + ": FAILED - " +
                        (stage.name == "Test" ? "3 tests failed" :
                         stage.name == "Security Scan" ? "critical vulnerability detected" :
                         stage.name == "Deploy" ? "pod crash loop detected" :
                         stage.name == "Health Check" ? "endpoint returned 503" :
                         "build error in module"));

                    // Update test pass sparkline
                    m.test_pass_history.erase(m.test_pass_history.begin());
                    m.test_pass_history.push_back(m.randf(0.5f, 0.8f));
                } else {
                    stage.status = StageStatus::Success;
                    stage.progress = 1.0f;
                    add_log(m, 3, svc.name + "/" + stage.name + ": completed in " + fmt_time(stage.elapsed));

                    // Advance to next stage
                    int next = svc.current_stage + 1;
                    if (next < static_cast<int>(svc.stages.size())) {
                        svc.current_stage = next;
                        svc.stages[static_cast<size_t>(next)].status = StageStatus::Running;
                    } else {
                        svc.deploying = false;
                        svc.deploy_count++;
                        add_log(m, 3, svc.name + " " + svc.version + ": deployment successful! (" + fmt_time(svc.total_time) + ")");

                        // Update sparklines
                        m.test_pass_history.erase(m.test_pass_history.begin());
                        m.test_pass_history.push_back(m.randf(0.85f, 1.0f));
                        m.build_time_history.erase(m.build_time_history.begin());
                        m.build_time_history.push_back(svc.total_time);
                    }
                }
            }
        }
    }

    // Force mode auto-disables
    if (m.force_mode && m.frame_count % 150 == 0) m.force_mode = false;
}

// -- UI Builders -------------------------------------------------------------

static maya::Style status_color(StageStatus s) {
    switch (s) {
        case StageStatus::Pending:     return fg_rgb(80, 80, 100);
        case StageStatus::Running:     return fg_rgb(255, 200, 60);
        case StageStatus::Success:     return fg_rgb(0, 230, 118);
        case StageStatus::Failed:      return fg_rgb(255, 60, 80);
        case StageStatus::Skipped:     return fg_rgb(120, 120, 140);
        case StageStatus::RollingBack: return fg_rgb(255, 140, 50);
    }
    return fg_rgb(80, 80, 100);
}

static const char* status_icon(StageStatus s, int frame) {
    switch (s) {
        case StageStatus::Pending:     return "○";
        case StageStatus::Running:     return dot_spin(frame);
        case StageStatus::Success:     return "✓";
        case StageStatus::Failed:      return "✗";
        case StageStatus::Skipped:     return "⊘";
        case StageStatus::RollingBack: return "↺";
    }
    return "?";
}

static maya::Element build_header(const Model& m) {
    auto spin = std::string(dot_spin(m.frame_count));

    auto& ec = env_colors[static_cast<int>(m.current_env)];
    auto env_style = fg_rgb(ec[0], ec[1], ec[2]);

    // Animated gradient accent
    int phase = m.frame_count % 12;
    const char* blocks[] = {"░","▒","▓","█","▓","▒"};
    std::string grad;
    for (int i = 0; i < 6; ++i) grad += blocks[(i + phase) % 6];

    std::string force_str = m.force_mode ? "  FORCE" : "";

    return (h(
        text(spin) | Fg<0, 200, 255>,
        text(" DEPLOY CONTROL") | Bold | Fg<0, 200, 255>,
        text(" " + grad, fg_rgb(0, 150, 200)),
        space,
        text("ENV:") | Dim,
        text(std::string(" ") + env_names[static_cast<int>(m.current_env)], env_style.with_bold()),
        text(force_str) | Bold | Fg<255, 100, 50>,
        space,
        text("deploys:") | Dim,
        text(std::to_string(m.total_deploys)) | Fg<100, 200, 255>,
        text("  failures:") | Dim,
        text(std::to_string(m.total_failures)) | Fg<255, 60, 80>,
        text("  rollbacks:") | Dim,
        text(std::to_string(m.total_rollbacks)) | Fg<255, 200, 60>
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_stage_cell(const PipelineStage& stage, int frame) {
    auto col = status_color(stage.status);
    auto icon = std::string(status_icon(stage.status, frame));

    std::string time_str;
    if (stage.status == StageStatus::Running || stage.status == StageStatus::Success ||
        stage.status == StageStatus::Failed || stage.status == StageStatus::RollingBack) {
        time_str = " " + fmt_time(stage.elapsed);
    }

    std::string bar;
    if (stage.status == StageStatus::Running || stage.status == StageStatus::RollingBack) {
        bar = block_bar(stage.progress, 10);
    } else if (stage.status == StageStatus::Success) {
        bar = block_bar(1.0f, 10);
    } else if (stage.status == StageStatus::Failed) {
        bar = block_bar(stage.progress, 10);
    }

    std::vector<maya::Element> parts;
    parts.push_back((h(
        text(icon, col),
        text(" " + stage.name, col.with_bold())
    )).build());

    if (!bar.empty()) {
        auto bar_col = (stage.status == StageStatus::Failed) ? fg_rgb(255, 60, 80) :
                        (stage.status == StageStatus::RollingBack) ? fg_rgb(255, 140, 50) :
                        (stage.status == StageStatus::Success) ? fg_rgb(0, 230, 118) :
                        fg_rgb(255, 200, 60);
        parts.push_back(text(bar, bar_col).build());
    }

    if (!time_str.empty()) {
        parts.push_back((text(time_str) | Dim).build());
    }

    return vstack().padding(0, 1, 0, 0)(std::move(parts));
}

static maya::Element build_pipeline_panel(const Model& m) {
    std::vector<maya::Element> rows;

    for (auto& svc : m.services) {
        // Service name + version
        std::vector<maya::Element> stage_cells;

        // Service label
        auto svc_label = (h(
            text(svc.icon),
            text(" " + svc.name) | Bold | Fg<180, 190, 220>,
            text(" " + svc.version) | Dim
        )).build();

        // Stage chain
        std::vector<maya::Element> chain;
        for (size_t i = 0; i < svc.stages.size(); ++i) {
            chain.push_back(build_stage_cell(svc.stages[i], m.frame_count));
            if (i + 1 < svc.stages.size()) {
                auto arrow_col = (svc.stages[i].status == StageStatus::Success)
                    ? fg_rgb(0, 230, 118) : fg_rgb(60, 60, 80);
                chain.push_back(text(" → ", arrow_col).build());
            }
        }

        auto chain_elem = hstack()(std::move(chain));

        // Status badge on the right
        std::string badge_text;
        maya::Style badge_col;
        bool any_running = false;
        bool any_failed = false;
        bool all_success = true;
        bool any_rollback = false;

        for (auto& s : svc.stages) {
            if (s.status == StageStatus::Running) any_running = true;
            if (s.status == StageStatus::Failed) any_failed = true;
            if (s.status != StageStatus::Success) all_success = false;
            if (s.status == StageStatus::RollingBack) any_rollback = true;
        }

        if (any_rollback) { badge_text = "ROLLING BACK"; badge_col = fg_rgb(255, 140, 50); }
        else if (any_failed) { badge_text = "FAILED"; badge_col = fg_rgb(255, 60, 80); }
        else if (any_running) { badge_text = "DEPLOYING"; badge_col = fg_rgb(255, 200, 60); }
        else if (all_success && !svc.stages.empty()) { badge_text = "LIVE"; badge_col = fg_rgb(0, 230, 118); }
        else { badge_text = "IDLE"; badge_col = fg_rgb(80, 80, 100); }

        auto status_badge = text(" [" + badge_text + "]", badge_col.with_bold()).build();

        // Deploy count
        auto deploy_info = (h(
            text(" #" + std::to_string(svc.deploy_count)) | Dim
        )).build();

        auto svc_row = (h(
            std::move(svc_label),
            std::move(status_badge),
            std::move(deploy_info)
        )).build();

        rows.push_back(std::move(svc_row));
        rows.push_back(std::move(chain_elem));

        // Separator between services (thin line)
        if (&svc != &m.services.back()) {
            rows.push_back((text("") | Dim).build());
        }
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(0, 100, 140))
        .border_text(" PIPELINE ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_log_panel(const Model& m) {
    std::vector<maya::Element> rows;

    for (auto& entry : m.logs) {
        int mins = static_cast<int>(entry.timestamp) / 60;
        int secs = static_cast<int>(entry.timestamp) % 60;
        char ts[16];
        std::snprintf(ts, sizeof(ts), "%02d:%02d", mins, secs);

        maya::Style lvl_style;
        const char* lvl_tag;
        switch (entry.level) {
            case 0:  lvl_tag = "INFO"; lvl_style = fg_rgb(80, 80, 100); break;
            case 1:  lvl_tag = "WARN"; lvl_style = fg_rgb(255, 200, 60).with_bold(); break;
            case 2:  lvl_tag = "ERR "; lvl_style = fg_rgb(255, 60, 80).with_bold(); break;
            case 3:  lvl_tag = " OK "; lvl_style = fg_rgb(0, 230, 118).with_bold(); break;
            default: lvl_tag = "INFO"; lvl_style = fg_rgb(80, 80, 100); break;
        }

        rows.push_back((h(
            text(std::string(ts)) | Dim | w_<6>,
            text(std::string(lvl_tag), lvl_style) | w_<5>,
            text(entry.msg) | Dim | clip
        ) | gap_<1>).build());
    }

    while (rows.size() < MAX_LOGS)
        rows.push_back((text("") | Dim).build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(40, 50, 65))
        .border_text(" BUILD LOG ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_metrics_panel(const Model& m) {
    // Deploy frequency sparkline
    auto deploy_spark = maya::Sparkline(m.deploy_freq_history,
        maya::SparklineConfig{.color = maya::Color::rgb(0, 200, 255)});
    deploy_spark.set_label("Deploy Freq");
    deploy_spark.set_show_last(true);

    // Test pass rate sparkline
    auto test_spark = maya::Sparkline(m.test_pass_history,
        maya::SparklineConfig{.color = maya::Color::rgb(0, 230, 118)});
    test_spark.set_label("Test Pass %");
    test_spark.set_min(0.0f);
    test_spark.set_max(1.0f);
    test_spark.set_show_last(true);

    // Build time sparkline
    auto build_spark = maya::Sparkline(m.build_time_history,
        maya::SparklineConfig{.color = maya::Color::rgb(255, 200, 60)});
    build_spark.set_label("Build Time ");
    build_spark.set_show_last(true);

    // Overall pipeline health
    int active = 0, succeeded = 0, failed = 0;
    for (auto& svc : m.services) {
        for (auto& stage : svc.stages) {
            if (stage.status == StageStatus::Running) active++;
            if (stage.status == StageStatus::Success) succeeded++;
            if (stage.status == StageStatus::Failed) failed++;
        }
    }
    float health = (succeeded + active > 0)
        ? static_cast<float>(succeeded) / static_cast<float>(succeeded + failed + active)
        : 1.0f;

    std::string health_bar = block_bar(health, 16);
    int health_pct = static_cast<int>(health * 100.0f);

    auto health_col = (health > 0.8f) ? fg_rgb(0, 230, 118)
                    : (health > 0.5f) ? fg_rgb(255, 200, 60)
                    : fg_rgb(255, 60, 80);

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(40, 50, 65))
        .border_text(" METRICS ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(
            deploy_spark.build(),
            test_spark.build(),
            build_spark.build(),
            (h(
                text("Health     ") | Fg<200, 204, 212>,
                text(health_bar, health_col),
                text("  " + std::to_string(health_pct) + "%", health_col.with_bold())
            )).build()
        );
}

static maya::Element build_status_bar(const Model& m) {
    int mins = static_cast<int>(m.uptime) / 60;
    int secs = static_cast<int>(m.uptime) % 60;
    char ts[16];
    std::snprintf(ts, sizeof(ts), "%02d:%02d", mins, secs);

    // Count active deployments
    int active = 0;
    for (auto& svc : m.services) {
        if (svc.deploying) active++;
    }

    // Overall progress
    int total_stages = 0;
    int completed_stages = 0;
    for (auto& svc : m.services) {
        total_stages += static_cast<int>(svc.stages.size());
        for (auto& stage : svc.stages) {
            if (stage.status == StageStatus::Success) completed_stages++;
        }
    }
    float overall = (total_stages > 0)
        ? static_cast<float>(completed_stages) / static_cast<float>(total_stages)
        : 0.0f;
    int overall_pct = static_cast<int>(overall * 100.0f);

    return (h(
        text(" ⏱ " + std::string(ts)) | Fg<100, 180, 255>,
        text("  active:") | Fg<140, 140, 160>,
        text(std::to_string(active), (active > 0) ? fg_rgb(255, 200, 60) : fg_rgb(80, 80, 100)),
        text("  progress:") | Fg<140, 140, 160>,
        text(std::to_string(overall_pct) + "%") | Bold | Fg<0, 200, 255>,
        space,
        text(" ␣") | Bold | Fg<180, 220, 255>, text(":deploy") | Fg<120, 120, 140>,
        text(" r") | Bold | Fg<180, 220, 255>, text(":rollback") | Fg<120, 120, 140>,
        text(" f") | Bold | Fg<180, 220, 255>, text(":force") | Fg<120, 120, 140>,
        text(" 1-3") | Bold | Fg<180, 220, 255>, text(":env") | Fg<120, 120, 140>,
        text(" q") | Bold | Fg<180, 220, 255>, text(":quit ") | Fg<120, 120, 140>
    ) | pad<0, 1, 0, 1> | Bg<30, 30, 42>).build();
}

// -- Render ------------------------------------------------------------------

static maya::Element render(const Model& m) {
    return vstack()(
        build_header(m),
        build_pipeline_panel(m),
        build_log_panel(m),
        build_metrics_panel(m),
        build_status_bar(m)
    );
}

// -- Program ---------------------------------------------------------------

struct Tick {};
struct Wave {};
struct Rollback {};
struct ToggleForce {};
struct SetEnv { Env env; };
struct Quit {};
using Msg = std::variant<Tick, Wave, Rollback, ToggleForce, SetEnv, Quit>;

struct Deploy {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg>;

    static Cmd init(Model& m)              { init_services(m); trigger_deploy_wave(m); return {}; }
    static Cmd update(Model& m, Tick)      { tick(m, 1.0f / 15.0f); return {}; }
    static Cmd update(Model& m, Wave)      { trigger_deploy_wave(m); return {}; }
    static Cmd update(Model& m, Rollback)  { trigger_rollback(m); return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }
    static Cmd update(Model& m, ToggleForce) {
        m.force_mode = !m.force_mode;
        if (m.force_mode) add_log(m, 1, "FORCE MODE enabled - skipping failure checks");
        else              add_log(m, 0, "FORCE MODE disabled");
        return {};
    }
    static Cmd update(Model& m, SetEnv e) {
        m.current_env = e.env;
        switch (e.env) {
            case Env::Dev:     add_log(m, 0, "Switched to DEV environment"); break;
            case Env::Staging: add_log(m, 0, "Switched to STAGING environment"); break;
            case Env::Prod:    add_log(m, 1, "Switched to PROD environment - caution!"); break;
        }
        return {};
    }

    static Element view(const Model& m) { return render(m); }

    static Sub subscribe(const Model&) {
        return Sub::every(std::chrono::milliseconds{66}, Tick{});
    }
    static bool subs_key(const Model&) { return true; }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{
            {'q', Quit{}}, {SpecialKey::Escape, Quit{}}, {' ', Wave{}}, {'r', Rollback{}},
            {'f', ToggleForce{}}, {'1', SetEnv{Env::Dev}}, {'2', SetEnv{Env::Staging}}, {'3', SetEnv{Env::Prod}},
        };
        return keys(k);
    }
};

static_assert(Program<Deploy>);

int main() { return run<Deploy>({.title = "deploy"}); }
