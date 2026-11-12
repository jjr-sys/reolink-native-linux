#pragma once

#include <QObject>

namespace rl {

// When each camera was last opened in the Events page (wayfinder/events-feed step 20):
// the unseen counts, the feed's blue dots and its "Seen up to here" line all come from
// it. Kept per desktop now (LocalSeenStore); a store shared with the Outpost apps can
// replace it later without changing the screen. Times are epoch seconds.
class SeenStore : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // 0 when the camera has never been opened: everything it logged is unseen.
    virtual qint64 seenAt(qint64 hostId, int channel) const = 0;
    // False when it can't be kept.
    virtual bool markSeen(qint64 hostId, int channel, qint64 at) = 0;

signals:
    void changed();
};

} // namespace rl
