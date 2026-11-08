#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace rl {

// Saved selections (wayfinder/site-tabs-redesign): named sets of cameras that a page's
// ☆ Saved menu recalls into its selection. They replace the named mosaics.
//
// Each entry: { id, name, cameras: ["hostId:channel", ...] in order }. Give it the stored
// text first (setSaved); saveRequested(text) says a change is worth storing. Stored text:
// a JSON array of entries.
class SavedSelections : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString saved READ saved WRITE setSaved)
    Q_PROPERTY(QVariantList list READ list NOTIFY changed)

public:
    explicit SavedSelections(QObject *parent = nullptr);

    QString saved() const { return m_saved; }
    void setSaved(const QString &text);
    QVariantList list() const { return m_list; }

    // Returns the new entry's id; "" (nothing saved) for a blank name or no cameras.
    Q_INVOKABLE QString add(const QString &name, const QStringList &cameras);
    Q_INVOKABLE void rename(const QString &id, const QString &name);
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE QVariantMap find(const QString &id) const;
    Q_INVOKABLE QStringList camerasOf(const QString &id) const;
    // "<base> N" with the first N no entry is called.
    Q_INVOKABLE QString nextName(const QString &base) const;

    // First launch: the named mosaics ([mosaics] list) as saved selections, same names,
    // cameras and order.
    Q_INVOKABLE static QVariantList fromMosaics(const QString &mosaicsJson);
    // First launch: the cameras that were HD (or Large) anywhere, for the per-camera set:
    // `gridKeys` (the grid-page mosaic's "k,k,..." list) then each named mosaic's.
    Q_INVOKABLE static QStringList mosaicKeys(const QString &mosaicsJson, const QString &field,
                                              const QString &gridKeys);

signals:
    void changed();
    void saveRequested(const QString &text);

private:
    int indexOf(const QString &id) const;
    void store();

    QString m_saved;
    QVariantList m_list;
};

} // namespace rl
