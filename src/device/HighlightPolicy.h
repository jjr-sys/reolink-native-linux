#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <utility>

// Highlight rules (wayfinder/detection-highlights): which camera panes are highlighted,
// how, and when a detection asks for an Alert sound. Pure logic with an injected clock —
// no Qt objects, no I/O — so timing rules are tested by handing it millisecond timestamps.
// Rules:
//   - one Rule per detection type; it covers all cameras or a chosen set, and a camera can
//     override its colour or sound, or switch the type off for itself
//   - a detection highlights the camera for the rule's seconds; a repeat while highlighted
//     extends it to a full duration from now
//   - two types at once: the higher rank (person > vehicle > pet > motion) sets the look
//   - a sound replays for a camera × type only once its cooldown has passed, and across all
//     cameras only the first sound within kCollapseMs plays
// Cameras are keyed "hostId:channel", as the rest of the app stores them.
namespace rl::highlight {

enum class Kind { None = 0, Motion = 1, Pet = 2, Vehicle = 3, Person = 4 }; // value = rank
enum class Style { Steady, Flashing, FlashThenSteady };

inline size_t qHash(Kind k, size_t seed = 0) noexcept { return ::qHash(int(k), seed); }

Kind kindOf(const QString &type);   // "person", "vehicle", "pet"/"dog_cat", "motion"
QString nameOf(Kind k);
Style styleOf(const QString &name); // "steady", "flashing", "flash-then-steady"
QString nameOf(Style s);

constexpr qint64 kFlashMs = 3'000;    // FlashThenSteady flashes this long
constexpr qint64 kCollapseMs = 2'000; // sounds closer together than this: only the first

struct Override {
    bool off = false;  // this camera is not highlighted for the type
    QString colour;    // empty: the rule's
    QString sound;     // empty: the rule's; "none": silent
};

struct Rule {
    bool on = false;
    bool allCameras = true;
    QList<QString> cameras; // when !allCameras
    QString colour;
    Style style = Style::FlashThenSteady;
    int seconds = 15;
    QString sound;           // empty: silent
    int cooldownSeconds = 60;
    QHash<QString, Override> overrides;
};

using Rules = QHash<Kind, Rule>;
Rules defaultRules();
// Stored as { "person": { on, cameras: "all" | [keys], colour, style, seconds, sound,
// cooldown, overrides: { key: { off, colour, sound } } }, ... }. A missing type or field
// keeps its default.
Rules rulesFromJson(const QJsonObject &json);
QJsonObject rulesToJson(const Rules &rules);

// What a camera's panes show now.
struct Look {
    Kind kind = Kind::None;  // None: not highlighted
    QString colour;
    Style style = Style::Steady;
    qint64 since = 0;        // when this highlight began (a repeat does not move it)
    qint64 until = 0;
    qint64 flashUntil = 0;   // flashing while now < flashUntil
    qint64 last = 0;         // the latest detection behind it
};

class HighlightPolicy
{
public:
    explicit HighlightPolicy(Rules rules = defaultRules()) : m_rules(std::move(rules)) {}

    void setRules(const Rules &rules) { m_rules = rules; }
    const Rules &rules() const { return m_rules; }
    void reset() { m_active.clear(); m_lastSound.clear(); m_lastAnySound = -1; }

    // A detection arrived. Returns the sound to play now, or empty.
    QString detect(const QString &camera, Kind kind, qint64 now);
    // Drops ended highlights; true when any camera's look changed.
    bool expire(qint64 now);

    Look look(const QString &camera, qint64 now) const;
    QList<QString> highlighted(qint64 now) const;

private:
    struct Active {
        qint64 since = 0;
        qint64 until = 0;
        qint64 last = 0;
    };
    static QString key(const QString &camera, Kind k) { return camera + u'/' + QString::number(int(k)); }

    Rules m_rules;
    QHash<QString, QHash<Kind, Active>> m_active; // camera -> type -> highlight
    QHash<QString, qint64> m_lastSound;           // camera/type -> when its sound last played
    qint64 m_lastAnySound = -1;
};

} // namespace rl::highlight
