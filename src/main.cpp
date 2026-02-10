#include <cstdio>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <unordered_set>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include "imgui_internal.h"
#include "implot.h"
#include "telemetry_protocol.h"

// ---------------- Command log only ----------------
static std::deque<std::string> g_cmdlog;
static size_t g_cmdlog_cap = 200;
static void cmdlog_push(const std::string& s) {
    g_cmdlog.push_back(s);
    while (g_cmdlog.size() > g_cmdlog_cap) g_cmdlog.pop_front();
}

// Three independent plot sets
static std::unordered_set<std::string> g_plot_set[3];

static const char* GuessYLabel(const std::string& name) {
    if (name.rfind("V_", 0) == 0) return "Volts (V)";
    if (name.rfind("I_", 0) == 0) return "Amps (A)";
    if (name.find("ROTOR") != std::string::npos) return "Degrees (deg)";
    if (name.find("RATE") != std::string::npos)  return "kHz";
    return "Value";
}

// Splitter helper
static bool Splitter(bool vertical, float thickness, float* size1, float* size2,
                     float min_size1, float min_size2, const char* id) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f,0.20f,0.20f,0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f,0.30f,0.30f,0.55f));

    bool changed = false;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 bb = vertical ? ImVec2(thickness, avail.y) : ImVec2(avail.x, thickness);

    ImGui::InvisibleButton(id, bb, ImGuiButtonFlags_MouseButtonLeft);

    if (ImGui::IsItemActive()) {
        float delta = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        if (delta != 0.0f) {
            float total = *size1 + *size2;
            float s1 = *size1 + delta;
            float s2 = total - s1;

            if (s1 < min_size1) { s1 = min_size1; s2 = total - s1; }
            if (s2 < min_size2) { s2 = min_size2; s1 = total - s2; }

            if (s1 != *size1) {
                *size1 = s1;
                *size2 = s2;
                changed = true;
            }
        }
    }

    ImGui::PopStyleColor(3);

    // subtle visible line so it’s obvious you can drag
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pmin = ImGui::GetItemRectMin();
    ImVec2 pmax = ImGui::GetItemRectMax();
    ImU32 col = IM_COL32(80, 80, 80, 140);
    if (vertical) {
        float x = (pmin.x + pmax.x) * 0.5f;
        dl->AddLine(ImVec2(x, pmin.y), ImVec2(x, pmax.y), col, 1.0f);
    } else {
        float y = (pmin.y + pmax.y) * 0.5f;
        dl->AddLine(ImVec2(pmin.x, y), ImVec2(pmax.x, y), col, 1.0f);
    }

    return changed;
}

static void PlotSet(const char* plot_id,
                    const TelemetryState& st,
                    const std::unordered_set<std::string>& plot_set,
                    float view_seconds,
                    float height_px)
{
    if (plot_set.empty()) {
        ImGui::TextDisabled("No signals selected.");
        return;
    }

    float newest = -1.0f;
    for (const auto& name : plot_set) {
        auto it = st.hist.find(name);
        if (it != st.hist.end() && !it->second.t.empty())
            newest = std::max(newest, it->second.t.back());
    }
    if (newest < 0.0f) {
        ImGui::TextDisabled("No history yet.");
        return;
    }

    float oldest_allowed = newest - view_seconds;

    std::string y_label = "Mixed units";
    if (plot_set.size() == 1) y_label = GuessYLabel(*plot_set.begin());

    if (ImPlot::BeginPlot(plot_id, ImVec2(-1, height_px))) {
        ImPlot::SetupAxes("Time (s)", y_label.c_str(),
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, oldest_allowed, newest, ImGuiCond_Always);

        for (const auto& name : plot_set) {
            auto it = st.hist.find(name);
            if (it == st.hist.end()) continue;
            const auto& H = it->second;
            if (H.t.size() < 2) continue;

            static thread_local std::vector<double> xt;
            static thread_local std::vector<double> yt;
            xt.clear(); yt.clear();

            size_t start = 0;
            while (start < H.t.size() && H.t[start] < oldest_allowed) start++;
            size_t n = H.t.size() - start;
            if (n < 2) continue;

            xt.reserve(n);
            yt.reserve(n);
            for (size_t i = start; i < H.t.size(); ++i) {
                xt.push_back((double)H.t[i]);
                yt.push_back((double)H.y[i]);
            }

            ImPlot::PlotLine(name.c_str(), xt.data(), yt.data(), (int)xt.size());
        }

        ImPlot::EndPlot();
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    std::string port = (argc >= 2) ? argv[1] : "COM5";
#else
    std::string port = (argc >= 2) ? argv[1] : "/dev/ttyACM0";
#endif

    TelemetryClient client;
    client.start(port);

    if (!glfwInit()) return 1;
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1280, 820, "RTE", nullptr, nullptr);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    char filter[128] = {0};
    static char cmd_buf[256] = {0};
    static bool focus_cmd = true;
    float view_seconds = 10.0f;

    // Adjustable sizes (persist across frames)
    static float left_w = 520.0f;           // selection area width
    static float left_console_h = 120.0f;   // console height inside left
    static float g1_h = 220.0f;             // graph1 height
    static float g2_h = 220.0f;             // graph2 height (graph3 is remainder)

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        TelemetryState st = client.snapshot();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove;

        ImGui::Begin("##RTE_ROOT", nullptr, flags);

        // header
        ImGui::Text("Port: %s | RX: %.1f Hz | Seq: %u | Good: %llu | Bad: %llu | Reject: dec=%llu crc=%llu hdr=%llu len=%llu",
                    port.c_str(), st.rx_hz, st.last_seq,
                    (unsigned long long)st.good_frames,
                    (unsigned long long)st.bad_frames,
                    (unsigned long long)st.reject_decode,
                    (unsigned long long)st.reject_crc,
                    (unsigned long long)st.reject_hdr,
                    (unsigned long long)st.reject_len);
        ImGui::Separator();

        // --- main split: selection vs graphs ---
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float vsplit = 6.0f;
        const float min_left = 320.0f;
        const float min_right = 420.0f;

        left_w = std::clamp(left_w, min_left, std::max(min_left, avail.x - min_right - vsplit));
        float right_w = std::max(min_right, avail.x - left_w - vsplit);

        // LEFT (selection + console)
        ImGui::BeginChild("##left", ImVec2(left_w, 0), true);

        // controls
        ImGui::SliderFloat("Plot view (sec)", &view_seconds, 0.5f, 60.0f, "%.1f");
        ImGui::InputTextWithHint("Filter", "type to filter signals...", filter, sizeof(filter));
        ImGui::Separator();

        // inside-left split: signals vs console (adjustable)
        ImVec2 left_av = ImGui::GetContentRegionAvail();
        const float hsplit = 6.0f;
        const float min_sig_h = 140.0f;
        const float min_con_h = 80.0f;

        left_console_h = std::clamp(left_console_h, min_con_h, std::max(min_con_h, left_av.y - min_sig_h - hsplit));
        float sig_h = std::max(min_sig_h, left_av.y - left_console_h - hsplit);

        // SIGNAL LIST (table without vertical separators)
        ImGui::BeginChild("##signals", ImVec2(0, sig_h), false);

        std::vector<std::string> keys;
        keys.reserve(st.latest.size());
        for (auto& kv : st.latest) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());

        // NOTE: no border flags => no vertical separators between G1/G2/G3
        ImGuiTableFlags tflags =
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit;

        if (ImGui::BeginTable("##sig_table", 5, tflags, ImVec2(0, 0))) {
            ImGui::TableSetupColumn("G1", ImGuiTableColumnFlags_WidthFixed, 28.0f);
            ImGui::TableSetupColumn("G2", ImGuiTableColumnFlags_WidthFixed, 28.0f);
            ImGui::TableSetupColumn("G3", ImGuiTableColumnFlags_WidthFixed, 28.0f);
            ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();

            for (auto& k : keys) {
                if (filter[0] && k.find(filter) == std::string::npos) continue;

                float v = st.latest[k];
                bool p1 = (g_plot_set[0].find(k) != g_plot_set[0].end());
                bool p2 = (g_plot_set[1].find(k) != g_plot_set[1].end());
                bool p3 = (g_plot_set[2].find(k) != g_plot_set[2].end());

                ImGui::PushID(k.c_str());
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (ImGui::Checkbox("##g1", &p1)) { if (p1) g_plot_set[0].insert(k); else g_plot_set[0].erase(k); }

                ImGui::TableSetColumnIndex(1);
                if (ImGui::Checkbox("##g2", &p2)) { if (p2) g_plot_set[1].insert(k); else g_plot_set[1].erase(k); }

                ImGui::TableSetColumnIndex(2);
                if (ImGui::Checkbox("##g3", &p3)) { if (p3) g_plot_set[2].insert(k); else g_plot_set[2].erase(k); }

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(k.c_str());

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("% .6f", v);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::EndChild(); // signals

        // draggable splitter signals/console
        Splitter(false, hsplit, &sig_h, &left_console_h, min_sig_h, min_con_h, "##split_left_h");

        // CONSOLE (formatted + input aligned consistently)
        ImGui::BeginChild("##console_left", ImVec2(0, 0), false);

        // Header row
        if (ImGui::SmallButton("Clear")) g_cmdlog.clear();
        ImGui::SameLine();
        ImGui::TextUnformatted("Commands");
        ImGui::Separator();

        // compute bottom input row height so log fills above it
        float frame_h = ImGui::GetFrameHeight();
        float spacing_y = ImGui::GetStyle().ItemSpacing.y;
        float input_row_h = frame_h + spacing_y; // one row

        float log_h = ImGui::GetContentRegionAvail().y - input_row_h - spacing_y;
        log_h = std::max(20.0f, log_h);

        // Log (top of console)
        ImGui::BeginChild("##cmdlog", ImVec2(0, log_h), false);
        for (auto& line : g_cmdlog) ImGui::TextUnformatted(line.c_str());
        ImGui::EndChild();

        // Input row pinned near bottom, with same padding as the rest
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Send:");
        ImGui::SameLine();

        if (focus_cmd) {
            ImGui::SetKeyboardFocusHere();
            focus_cmd = false;
        }

        // Input width fills remaining, button fixed
        ImGui::SetNextItemWidth(-60.0f);
        bool enter = ImGui::InputText("##cmd", cmd_buf, sizeof(cmd_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool clicked = ImGui::SmallButton("Send");

        if (enter || clicked) {
            std::string cmd(cmd_buf);
            if (!cmd.empty()) {
                bool ok = client.sendLine(cmd);
                cmdlog_push(std::string("> ") + cmd);
                cmdlog_push(ok ? "  (sent)" : "  (FAILED to send)");
                cmd_buf[0] = 0;
            }
            focus_cmd = true;
        }

        ImGui::EndChild(); // console_left
        ImGui::EndChild(); // left

        ImGui::SameLine();

        // vertical splitter selection/graphs (adjustable)
        Splitter(true, vsplit, &left_w, &right_w, min_left, min_right, "##split_main_v");

        ImGui::SameLine();

        // RIGHT (graphs, with adjustable heights)
        ImGui::BeginChild("##right", ImVec2(0, 0), true);

        ImVec2 r_av = ImGui::GetContentRegionAvail();
        const float gsplit = 6.0f;
        const float title_h = ImGui::GetTextLineHeightWithSpacing();
        const float min_plot_h = 120.0f;

        // total available for plots + splitters + titles
        // We'll treat each graph block as: title + plot
        float total_h = r_av.y;

        // initial clamp of g1_h/g2_h based on current right region
        // Graph3 takes remainder.
        float fixed_overhead = (title_h * 3.0f) + (gsplit * 2.0f) + (ImGui::GetStyle().ItemSpacing.y * 4.0f);
        float usable = std::max(0.0f, total_h - fixed_overhead);

        // ensure g1+g2 <= usable - min_plot_h
        g1_h = std::clamp(g1_h, min_plot_h, std::max(min_plot_h, usable - min_plot_h - min_plot_h));
        g2_h = std::clamp(g2_h, min_plot_h, std::max(min_plot_h, usable - g1_h - min_plot_h));
        float g3_h = std::max(min_plot_h, usable - g1_h - g2_h);

        // --- Graph 1 ---
        ImGui::TextUnformatted("Graph 1");
        PlotSet("##telemetry_plot_1", st, g_plot_set[0], view_seconds, g1_h);

        // splitter between graph1 and graph2 (drag down on line)
        float dummy2 = g2_h; // will be overwritten by Splitter
        Splitter(false, gsplit, &g1_h, &dummy2, min_plot_h, min_plot_h, "##split_g12");
        g2_h = dummy2;

        // --- Graph 2 ---
        ImGui::TextUnformatted("Graph 2");
        PlotSet("##telemetry_plot_2", st, g_plot_set[1], view_seconds, g2_h);

        // splitter between graph2 and graph3
        float dummy3 = g3_h;
        Splitter(false, gsplit, &g2_h, &dummy3, min_plot_h, min_plot_h, "##split_g23");
        g3_h = dummy3;

        // --- Graph 3 (remainder) ---
        ImGui::TextUnformatted("Graph 3");
        PlotSet("##telemetry_plot_3", st, g_plot_set[2], view_seconds, g3_h);

        ImGui::EndChild(); // right

        ImGui::End(); // root

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    client.stop();

    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
