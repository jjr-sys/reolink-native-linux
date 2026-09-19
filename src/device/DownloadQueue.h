#pragma once

#include "DownloadPlan.h"
#include "DownloadSource.h"

#include <QAbstractListModel>
#include <QThreadPool>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace rl {

// The session's download queue: one item per camera per time range, shown in a list with
// status and progress. Items to the same NVR run one at a time, different NVRs in
// parallel. The list lasts for the session; the files on disk are the lasting record.
class DownloadQueue : public QAbstractListModel
{
    Q_OBJECT
    // Where clips are saved (remembered). Defaults to ~/Videos/Reolink.
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    // Items waiting or running, for a badge on the Downloads button.
    Q_PROPERTY(int activeCount READ activeCount NOTIFY countsChanged)
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        SiteRole,
        CameraRole,
        StartRole,    // Unix seconds
        EndRole,
        StateRole,    // "queued" | "preparing" | "downloading" | "done" | "failed" | "cancelled"
        ProgressRole, // 0..100, or -1 when there is nothing to measure yet
        DetailRole,   // the reason for a failure, or what is happening now
        PathRole,     // the saved file (the first, if the range was split)
    };

    // `sourceFor(row)` describes the camera at a device row (DeviceManager::downloadSource);
    // taken as a function so the queue does not depend on the whole device model.
    using SourceFn = std::function<DownloadSource(int row)>;
    explicit DownloadQueue(SourceFn sourceFor, QObject *parent = nullptr);
    ~DownloadQueue() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString folder() const { return m_folder; }
    void setFolder(const QString &path);
    int activeCount() const;

    // Queue the range [startEpoch, endEpoch) (Unix seconds) of the camera at `deviceRow`.
    // Returns the item id, or -1 if the range or camera is unusable.
    Q_INVOKABLE int enqueue(int deviceRow, qint64 startEpoch, qint64 endEpoch);
    // Stop an item; whatever it had saved so far is removed. A waiting item is dropped.
    Q_INVOKABLE void cancel(int id);
    // Put a failed or cancelled item back in the queue.
    Q_INVOKABLE void retry(int id);
    // Take a finished item off the list (the file stays on disk).
    Q_INVOKABLE void remove(int id);
    Q_INVOKABLE void clearFinished();
    // Where to save a snapshot of this camera at this recorded moment: in the download
    // folder, named like a clip (site, camera, time) and never over an existing file.
    // Empty if the camera is not ready.
    Q_INVOKABLE QString snapshotPath(int deviceRow, qint64 epoch);
    // Show an item's folder in the file manager (the download folder when it has no file).
    Q_INVOKABLE void openFolder(int id);

signals:
    void folderChanged();
    void countsChanged();
    // An item reached a final state; `message` is the file name or the reason.
    void itemFinished(int id, const QString &state, const QString &message);

private:
    struct Item {
        int id = 0;
        DownloadSource src;
        qint64 start = 0, end = 0;
        DlState state = DlState::Queued;
        int progress = -1;
        QString detail;
        QStringList paths;
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    };

    int indexOf(int id) const;
    void pump();
    void run(const Item &item, const QString &folder);
    void update(int id, DlState state, int progress, const QString &detail,
                const QStringList &paths = {});

    SourceFn m_sourceFor;
    std::vector<Item> m_items;
    int m_nextId = 1;
    QString m_folder;
    QThreadPool m_pool;
};

} // namespace rl
