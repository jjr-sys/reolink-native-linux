#pragma once

#include "core/SeenStore.h"

#include <QHash>
#include <QPair>

namespace rl {

class Database;

// The seen times of this desktop, in the app's database (camera_seen), read once and
// kept in memory.
class LocalSeenStore : public SeenStore
{
    Q_OBJECT

public:
    explicit LocalSeenStore(Database *db, QObject *parent = nullptr);

    qint64 seenAt(qint64 hostId, int channel) const override;
    bool markSeen(qint64 hostId, int channel, qint64 at) override;

private:
    Database *m_db;
    QHash<QPair<qint64, int>, qint64> m_seen;
};

} // namespace rl
