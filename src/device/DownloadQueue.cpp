#include "DownloadQueue.h"

#include "core/Log.h"
#include "core/Paths.h"

#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStorageInfo>
#include <QUrl>

#include <algorithm>

namespace rl {

namespace {
constexpr auto kFolderKey = "downloads/folder";
// Seconds without data before a running download is given up on.
constexpr long kStallSec = 90;
constexpr qint64 kMaxRangeSecs = 6 * 3600;
constexpr qint64 kMinRangeSecs = 2;

QString stateName(DlState s)
{
    switch (s) {
    case DlState::Queued: return QStringLiteral("queued");
    case DlState::Preparing: return QStringLiteral("preparing");
    case DlState::Downloading: return QStringLiteral("downloading");
    case DlState::Done: return QStringLiteral("done");
    case DlState::Failed: return QStringLiteral("failed");
    case DlState::Cancelled: return QStringLiteral("cancelled");
    }
    return {};
}

// A name that does not overwrite an earlier download of the same moment.
QString uniquePath(const QString &dir, const QString &name)
{
    QString path = dir + u'/' + name;
    if (!QFileInfo::exists(path))
        return path;
    const QFileInfo fi(name);
    for (int n = 2; n < 1000; ++n) {
        path = QStringLiteral("%1/%2_%3.%4").arg(dir, fi.completeBaseName()).arg(n).arg(fi.suffix());
        if (!QFileInfo::exists(path))
            return path;
    }
    return dir + u'/' + name;
}
} // namespace

DownloadQueue::DownloadQueue(SourceFn sourceFor, QObject *parent)
    : QAbstractListModel(parent), m_sourceFor(std::move(sourceFor))
{
    m_pool.setMaxThreadCount(4);
    m_folder = QSettings().value(kFolderKey).toString();
    if (m_folder.isEmpty())
        m_folder = Paths::recordingsDir();
}

DownloadQueue::~DownloadQueue()
{
    for (Item &it : m_items)
        it.cancel->store(true);
    m_pool.waitForDone();
}

int DownloadQueue::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QHash<int, QByteArray> DownloadQueue::roleNames() const
{
    return {{IdRole, "id"},         {SiteRole, "site"},         {CameraRole, "camera"},
            {StartRole, "start"},   {EndRole, "end"},           {StateRole, "state"},
            {ProgressRole, "progress"}, {DetailRole, "detail"}, {PathRole, "path"}};
}

QVariant DownloadQueue::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const Item &it = m_items[size_t(index.row())];
    switch (role) {
    case IdRole: return it.id;
    case SiteRole: return it.src.site;
    case CameraRole: return it.src.camera;
    case StartRole: return it.start;
    case EndRole: return it.end;
    case StateRole: return stateName(it.state);
    case ProgressRole: return it.progress;
    case DetailRole: return it.detail;
    case PathRole: return it.paths.isEmpty() ? QString() : it.paths.first();
    }
    return {};
}

void DownloadQueue::setFolder(const QString &path)
{
    if (path.isEmpty() || path == m_folder)
        return;
    m_folder = path;
    QSettings().setValue(kFolderKey, m_folder);
    emit folderChanged();
}

int DownloadQueue::activeCount() const
{
    return int(std::count_if(m_items.begin(), m_items.end(), [](const Item &i) {
        return i.state == DlState::Queued || dlActive(i.state);
    }));
}

int DownloadQueue::indexOf(int id) const
{
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].id == id)
            return int(i);
    return -1;
}

int DownloadQueue::enqueue(int deviceRow, qint64 startEpoch, qint64 endEpoch)
{
    if (endEpoch - startEpoch < kMinRangeSecs || startEpoch <= 0)
        return -1;
    endEpoch = std::min(endEpoch, startEpoch + kMaxRangeSecs);
    DownloadSource src = m_sourceFor(deviceRow);
    if (!src.valid())
        return -1;

    Item it;
    it.id = m_nextId++;
    it.src = std::move(src);
    it.start = startEpoch;
    it.end = endEpoch;
    it.detail = tr("Waiting");
    beginInsertRows({}, int(m_items.size()), int(m_items.size()));
    m_items.push_back(std::move(it));
    endInsertRows();
    emit countsChanged();
    pump();
    return m_items.back().id;
}

void DownloadQueue::cancel(int id)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    Item &it = m_items[size_t(i)];
    if (it.state == DlState::Queued) {
        update(id, DlState::Cancelled, -1, tr("Cancelled"));
    } else if (dlActive(it.state)) {
        it.cancel->store(true); // the worker notices, removes its files and reports back
        update(id, it.state, it.progress, tr("Cancelling…"));
    }
}

void DownloadQueue::retry(int id)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    Item &it = m_items[size_t(i)];
    if (it.state != DlState::Failed && it.state != DlState::Cancelled)
        return;
    it.cancel = std::make_shared<std::atomic<bool>>(false);
    it.paths.clear();
    update(id, DlState::Queued, -1, tr("Waiting"));
    pump();
}

void DownloadQueue::remove(int id)
{
    const int i = indexOf(id);
    if (i < 0 || m_items[size_t(i)].state == DlState::Queued || dlActive(m_items[size_t(i)].state))
        return;
    beginRemoveRows({}, i, i);
    m_items.erase(m_items.begin() + i);
    endRemoveRows();
    emit countsChanged();
}

void DownloadQueue::clearFinished()
{
    for (int i = int(m_items.size()) - 1; i >= 0; --i) {
        const DlState s = m_items[size_t(i)].state;
        if (s == DlState::Done || s == DlState::Failed || s == DlState::Cancelled) {
            beginRemoveRows({}, i, i);
            m_items.erase(m_items.begin() + i);
            endRemoveRows();
        }
    }
    emit countsChanged();
}

QString DownloadQueue::snapshotPath(int deviceRow, qint64 epoch)
{
    const DownloadSource src = m_sourceFor(deviceRow);
    if (src.site.isEmpty() && src.camera.isEmpty())
        return {};
    if (!QDir().mkpath(m_folder))
        return {};
    QString name = downloadFileName(src.site, src.camera, QDateTime::fromSecsSinceEpoch(epoch));
    name.chop(4); // ".mp4"
    return uniquePath(m_folder, name + QStringLiteral("_snapshot.jpg"));
}

void DownloadQueue::openFolder(int id)
{
    QString dir = m_folder;
    const int i = indexOf(id);
    if (i >= 0 && !m_items[size_t(i)].paths.isEmpty())
        dir = QFileInfo(m_items[size_t(i)].paths.first()).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void DownloadQueue::update(int id, DlState state, int progress, const QString &detail,
                           const QStringList &paths)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    Item &it = m_items[size_t(i)];
    const bool wasFinal = it.state == DlState::Done || it.state == DlState::Failed
                          || it.state == DlState::Cancelled;
    it.state = state;
    it.progress = progress;
    it.detail = detail;
    if (!paths.isEmpty())
        it.paths = paths;
    const QModelIndex mi = index(i);
    emit dataChanged(mi, mi);
    emit countsChanged();
    const bool isFinal = state == DlState::Done || state == DlState::Failed || state == DlState::Cancelled;
    if (isFinal && !wasFinal) {
        emit itemFinished(id, stateName(state),
                          state == DlState::Done && !it.paths.isEmpty()
                              ? QFileInfo(it.paths.first()).fileName() : detail);
        pump(); // the NVR is free for the next item
    }
}

void DownloadQueue::pump()
{
    for (;;) {
        std::vector<DlSlot> view;
        view.reserve(m_items.size());
        for (const Item &it : m_items)
            view.push_back({it.src.hostId, it.state});
        const int next = nextRunnable(view);
        if (next < 0)
            return;
        Item &it = m_items[size_t(next)];
        it.state = DlState::Preparing; // claimed now, so this loop cannot start it twice
        update(it.id, DlState::Preparing, -1, tr("Preparing: the NVR is cutting the clip"));
        const Item snapshot = it;
        const QString folder = m_folder;
        m_pool.start([this, snapshot, folder] { run(snapshot, folder); });
    }
}

// Worker thread. Reports back through update(), always on the GUI thread.
void DownloadQueue::run(const Item &item, const QString &folder)
{
    const int id = item.id;
    const std::atomic<bool> *cancel = item.cancel.get();
    const DownloadSource &src = item.src;
    QStringList made;

    auto post = [this, id](DlState st, int progress, const QString &detail, const QStringList &paths) {
        QMetaObject::invokeMethod(
            this, [this, id, st, progress, detail, paths] { update(id, st, progress, detail, paths); },
            Qt::QueuedConnection);
    };
    auto fail = [&](const QString &why) {
        for (const QString &p : made)
            QFile::remove(p);
        if (cancel->load())
            post(DlState::Cancelled, -1, tr("Cancelled"), {});
        else
            post(DlState::Failed, -1, why, {});
    };

    if (!QDir().mkpath(folder)) {
        fail(tr("Can't write to %1").arg(folder));
        return;
    }

    const QDateTime start = QDateTime::fromSecsSinceEpoch(item.start);
    const QDateTime end = QDateTime::fromSecsSinceEpoch(item.end);
    const api::BatchResult b = src.client->callSlow(
        Json::array({api::nvrDownloadBody(src.channel, start, end, QStringLiteral("main"))}),
        prepareTimeoutSecs(item.end - item.start), cancel);
    if (cancel->load()) {
        fail({});
        return;
    }
    if (!b.transportOk || b.results.isEmpty() || !b.results.first().ok) {
        fail(b.error.isEmpty() ? tr("The NVR could not prepare this clip") : b.error);
        return;
    }
    const QVector<api::DownloadFile> files = api::parseNvrDownload(b.results.first().value);
    if (files.isEmpty()) {
        fail(tr("No recording covers this range"));
        return;
    }

    qint64 total = 0;
    for (const api::DownloadFile &f : files)
        total += f.size;
    if (total > 0 && QStorageInfo(folder).bytesAvailable() < total + (256ll << 20)) {
        fail(tr("Not enough free space in %1").arg(folder));
        return;
    }

    qint64 before = 0;
    QElapsedTimer sinceEmit;
    sinceEmit.start();
    for (int part = 0; part < files.size(); ++part) {
        const api::DownloadFile &f = files[part];
        QString err;
        if (!src.client->ensureLogin(&err)) { // a long queue can outlive the session token
            fail(err);
            return;
        }
        const QString url = api::downloadUrl(src.host, src.port, src.https, f.fileName,
                                             src.client->token());
        const QString path = uniquePath(
            folder, downloadFileName(src.site, src.camera, start, part + 1, int(files.size())));
        const QString label = files.size() > 1
            ? tr("Downloading part %1 of %2").arg(part + 1).arg(files.size()) : tr("Downloading");
        post(DlState::Downloading, total > 0 ? int(before * 100 / total) : -1, label, {});
        made << path;
        const bool ok = src.client->downloadToFile(
            url, path, 0, &err,
            [&](qint64 done, qint64) {
                if (total <= 0 || sinceEmit.elapsed() < 250)
                    return;
                sinceEmit.restart();
                post(DlState::Downloading, int(std::min<qint64>(99, (before + done) * 100 / total)),
                     label, {});
            },
            kStallSec, cancel);
        if (!ok) {
            made.removeOne(path); // downloadToFile already removed the partial file
            fail(err);
            return;
        }
        before += f.size;
    }
    post(DlState::Done, 100, tr("Saved"), made);
}

} // namespace rl
