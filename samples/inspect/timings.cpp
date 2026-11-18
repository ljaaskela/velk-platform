#include "timings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define INSPECT_POPEN _popen
#define INSPECT_PCLOSE _pclose
#else
#define INSPECT_POPEN popen
#define INSPECT_PCLOSE pclose
#endif

namespace inspect {

namespace {

constexpr const char* kHeader = "date\tcommit\tvelk\tdirty\tview\tsettled_ms\tmoving_median_ms\tmoving_max_ms\tpasses\n";

/// What `command` prints, its first line or all of it.
velk::string run(const velk::string& command)
{
    velk::string out;
    std::FILE* p = INSPECT_POPEN(command.c_str(), "r");
    if (!p) {
        return out;
    }
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p)) {
        out.append(buf);
    }
    INSPECT_PCLOSE(p);
    return out;
}

/// `s` split at `sep`.
velk::vector<velk::string> split(velk::string_view s, char sep)
{
    velk::vector<velk::string> parts;
    size_t from = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            parts.push_back(velk::string(s.substr(from, i - from)));
            from = i + 1;
        }
    }
    return parts;
}

double median(velk::vector<double> values)
{
    if (values.empty()) {
        return -1.0;
    }
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

} // namespace

velk::string commit_of(velk::string_view dir, velk::string_view paths, bool& dirty)
{
    velk::string git("git -C \"");
    git.append(dir);
    git.append("\" ");
    velk::string head = run(velk::string(git).append("rev-parse --short HEAD 2>nul"));
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r' || head.back() == ' ')) {
        head = head.substr(0, head.size() - 1);
    }
    if (head.empty()) {
        dirty = false;
        return velk::string("unknown");
    }
    velk::string status = git;
    status.append("status --porcelain --untracked-files=no -- ");
    status.append(paths);
    status.append(" 2>nul");
    dirty = !run(status).empty();
    return head;
}

velk::vector<TimingRow> load_timings(velk::string_view path)
{
    velk::vector<TimingRow> rows;
    const velk::string file(path);
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) {
        return rows;
    }
    velk::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) != 0) {
        text.append(velk::string_view(buf, n));
    }
    std::fclose(f);
    bool first = true;
    for (velk::string line : split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') {
            line = line.substr(0, line.size() - 1);
        }
        if (first || line.empty()) {
            first = false;
            continue;
        }
        // The passes may be left out, as for runs from before they were kept.
        const auto fields = split(line, '\t');
        if (fields.size() < 8) {
            continue;
        }
        TimingRow row;
        row.date = fields[0];
        row.commit = fields[1];
        row.velk_commit = fields[2];
        row.dirty = fields[3] == "1";
        row.times.view = fields[4];
        row.times.settled_ms = std::atof(fields[5].c_str());
        row.times.moving_median_ms = std::atof(fields[6].c_str());
        row.times.moving_max_ms = std::atof(fields[7].c_str());
        for (const velk::string& pass : split(fields.size() > 8 ? velk::string_view(fields[8].c_str(), fields[8].size()) : velk::string_view(), ';')) {
            const size_t eq = pass.find(velk::string_view("=", 1));
            if (eq == velk::string::npos) {
                continue;
            }
            row.times.passes.push_back({pass.substr(0, eq), std::atof(pass.c_str() + eq + 1)});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool append_timings(velk::string_view path, const velk::string& date, const velk::string& commit,
                    const velk::string& velk_commit, bool dirty, const velk::vector<ViewTimes>& times)
{
    const velk::string file(path);
    std::FILE* existing = std::fopen(file.c_str(), "rb");
    const bool fresh = existing == nullptr;
    if (existing) {
        std::fclose(existing);
    }
    std::FILE* f = std::fopen(file.c_str(), "ab");
    if (!f) {
        return false;
    }
    if (fresh) {
        std::fputs(kHeader, f);
    }
    for (const ViewTimes& t : times) {
        std::fprintf(f, "%s\t%s\t%s\t%d\t%s\t%.3f\t%.3f\t%.3f\t", date.c_str(), commit.c_str(), velk_commit.c_str(), dirty ? 1 : 0,
                     t.view.c_str(), t.settled_ms, t.moving_median_ms, t.moving_max_ms);
        bool first = true;
        for (const PassTime& p : t.passes) {
            std::fprintf(f, "%s%s=%.3f", first ? "" : ";", p.name.c_str(), p.ms);
            first = false;
        }
        std::fputs("\n", f);
    }
    std::fclose(f);
    return true;
}

bool baseline_of(const velk::vector<TimingRow>& rows, velk::string_view view, size_t runs, ViewTimes& out,
                 velk::string& since)
{
    velk::vector<double> settled;
    velk::vector<double> moving;
    const TimingRow* latest = nullptr;
    for (size_t i = rows.size(); i-- > 0 && settled.size() < runs;) {
        const TimingRow& row = rows[i];
        if (row.dirty || row.times.view != view) {
            continue;
        }
        if (!latest) {
            latest = &row;
        }
        settled.push_back(row.times.settled_ms);
        if (row.times.moving_median_ms >= 0.0) {
            moving.push_back(row.times.moving_median_ms);
        }
    }
    if (!latest) {
        return false;
    }
    out.view = velk::string(view);
    out.settled_ms = median(settled);
    out.moving_median_ms = median(moving);
    out.passes = latest->times.passes;
    since = latest->commit;
    return true;
}

bool slower(double now, double was)
{
    return was > 0.0 && now > was * 1.15 && now - was > 0.3;
}

void print_trend(const velk::vector<TimingRow>& rows, bool (*keep)(const char* scene, void* user), void* user)
{
    velk::vector<velk::string> views;
    for (const TimingRow& row : rows) {
        if (std::find(views.begin(), views.end(), row.times.view) == views.end()) {
            views.push_back(row.times.view);
        }
    }
    for (const velk::string& view : views) {
        const size_t slash = view.find(velk::string_view("/", 1));
        const velk::string scene = slash == velk::string::npos ? view : view.substr(0, slash);
        if (keep && !keep(scene.c_str(), user)) {
            continue;
        }
        std::printf("%s\n", view.c_str());
        std::printf("  %-16s %-9s %-9s %9s %9s %9s\n", "date", "velk-ui", "velk", "still ms", "move med", "move max");
        const TimingRow* before = nullptr;
        for (const TimingRow& row : rows) {
            if (row.times.view != view) {
                continue;
            }
            char moving[2][16] = {"-", "-"};
            if (row.times.moving_median_ms >= 0.0) {
                std::snprintf(moving[0], sizeof(moving[0]), "%.2f", row.times.moving_median_ms);
                std::snprintf(moving[1], sizeof(moving[1]), "%.2f", row.times.moving_max_ms);
            }
            const char* mark = "";
            if (before && slower(row.times.settled_ms, before->times.settled_ms)) {
                mark = "  <- slower";
            } else if (before && slower(before->times.settled_ms, row.times.settled_ms)) {
                mark = "  <- faster";
            }
            std::printf("  %-16s %-8s%s %-8s%s %9.2f %9s %9s%s\n", row.date.c_str(), row.commit.c_str(), row.dirty ? "*" : " ",
                        row.velk_commit.c_str(), " ", row.times.settled_ms, moving[0], moving[1], mark);
            before = &row;
        }
    }
}

} // namespace inspect
