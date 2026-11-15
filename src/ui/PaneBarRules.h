#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace rl {

// One bottom bar on every video pane (wayfinder/live-replay tickets 10 and 11, layout A): the
// rules of a pane's pill, apart from its looks. Which transport buttons it shows, which can act
// (with a reason when not), which is lit and what the fast / slow buttons say; which tools sit
// on the pill and which go under ⋯; how far it collapses to fit. Speed stepping itself is
// PlaybackGroup's; the QML (components/PaneBar.qml) lays these out and calls the pane's actions.
namespace panebar {

enum class Page { Live, Replay, Playback, Events };   // Replay: a Live pane playing its recording
enum class Camera { Recorder, Frigate, Stream };     // Stream: Bosch / bare stream, nothing recorded

struct State {
    Page page = Page::Live;
    Camera camera = Camera::Recorder;
    int direction = 1;          // the group's: 1 forward, -1 backward
    double speed = 1.0;
    bool paused = false;
    bool speedLocked = false;   // PlaybackGroup::speedLocked (only Frigate panes in Playback)
    bool reverseAvailable = true;
};

struct Button {
    bool shown = false;
    bool enabled = false;
    bool lit = false;
    QString caption;   // under a fast / slow button: "fast", "4×", "slow", "½×"
    QString reason;    // why it can't act (the tooltip), when not enabled
};

// Collapse steps, widest first: each drops something more so the pill fits its pane.
enum Step { Full = 0, NoSkips, FewTools, NoCaptions, AllToolsInMenu, kSteps };

// The transport's buttons, left to right on the pill.
inline const QString kSkipBack = QStringLiteral("skipBack");
inline const QString kFastBack = QStringLiteral("fastBack");
inline const QString kSlowBack = QStringLiteral("slowBack");
inline const QString kBack = QStringLiteral("back");
inline const QString kPause = QStringLiteral("pause");
inline const QString kForward = QStringLiteral("forward");
inline const QString kSlowForward = QStringLiteral("slowForward");
inline const QString kFastForward = QStringLiteral("fastForward");
inline const QString kSkipForward = QStringLiteral("skipForward");

// Whether the pane has a transport and a time line: anything with a recording behind it
// (on Live, a Reolink recorder camera only — Live replay).
bool hasTransport(const State &s);
QHash<QString, Button> transport(const State &s, int step);

// Tools: `available` (the ids the pane's camera has, in the pill's order) split into those on
// the pill (the page's common tools, fewer on narrow steps) and the rest, under ⋯.
struct ToolSplit {
    QStringList shown;
    QStringList menu;
};
ToolSplit tools(Page page, const QStringList &available, int step);

// The first step whose pill width (`widthAtStep`, widest first) fits in `room`; the last when
// none does.
int fitStep(double room, const QList<double> &widthAtStep);

} // namespace panebar

// The same for QML (registered as the PaneBarRules singleton). A state is a map: page
// ("live" | "replay" | "playback" | "events"), camera ("recorder" | "frigate" | "stream"),
// direction, speed, paused, speedLocked, reverseAvailable. Buttons come back as maps of
// shown / enabled / lit / caption / reason.
class PaneBarRules : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int stepCount READ stepCount CONSTANT)   // panebar::kSteps
public:
    int stepCount() const { return panebar::kSteps; }
    using QObject::QObject;
    Q_INVOKABLE bool hasTransport(const QVariantMap &state) const;
    Q_INVOKABLE QVariantMap transport(const QVariantMap &state, int step) const;
    Q_INVOKABLE QVariantMap tools(const QString &page, const QStringList &available, int step) const;
    Q_INVOKABLE int fitStep(double room, const QVariantList &widthAtStep) const;
};

} // namespace rl
