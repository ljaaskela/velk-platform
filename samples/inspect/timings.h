#ifndef VELK_INSPECT_TIMINGS_H
#define VELK_INSPECT_TIMINGS_H

#include <velk/string.h>
#include <velk/string_view.h>
#include <velk/vector.h>

namespace inspect {

/// A pass's GPU time, averaged over a view's still frames.
struct PassTime
{
    velk::string name;
    double ms = 0.0;
};

/// What a verification run measured of a view's GPU time: still, once settled; and while
/// something moves (the scene's movers or the view's camera motion), the median and worst
/// frame, negative when nothing moves.
struct ViewTimes
{
    velk::string view;
    double settled_ms = 0.0;
    double moving_median_ms = -1.0;
    double moving_max_ms = -1.0;
    velk::vector<PassTime> passes;
};

/// One line of the history: when, at which commits (velk-ui's and velk's, each marked with *
/// when its code that verify measures had changes not committed), and a view's times.
struct TimingRow
{
    velk::string date;
    velk::string commit;
    velk::string velk_commit;
    bool dirty = false;
    ViewTimes times;
};

/// The commit `dir`'s repository is at, short, and whether `paths` (space separated, relative
/// to the repository) have changes not committed. "unknown" when git cannot tell.
velk::string commit_of(velk::string_view dir, velk::string_view paths, bool& dirty);

/// The history at `path`, oldest first; empty when there is none.
velk::vector<TimingRow> load_timings(velk::string_view path);

/// Adds a run's times to the history at `path`, a line a view, starting the file with its
/// header when it is new.
bool append_timings(velk::string_view path, const velk::string& date, const velk::string& commit,
                    const velk::string& velk_commit, bool dirty, const velk::vector<ViewTimes>& times);

/// What a view's times are measured against: the median of its last `runs` rows on committed
/// code, each time apart, and the passes of the latest of them. False when there are none.
bool baseline_of(const velk::vector<TimingRow>& rows, velk::string_view view, size_t runs, ViewTimes& out,
                 velk::string& since);

/// Whether `now` is slower than `was` by more than verify lets pass: 15% and 0.3 ms both.
bool slower(double now, double was);

/// Prints each view's history, oldest first, its times and the change from the run before,
/// marking where a time stepped past the margin of slower(). Only views whose scene `keep`
/// says, when it is set.
void print_trend(const velk::vector<TimingRow>& rows, bool (*keep)(const char* scene, void* user), void* user);

} // namespace inspect

#endif // VELK_INSPECT_TIMINGS_H
