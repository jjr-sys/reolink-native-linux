#include "device/EventSnapshot.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>

#include <algorithm>

namespace rl::device {

namespace {
constexpr int kMaxWidth = 2560;

bool writeRaw(const QByteArray &bytes, const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
} // namespace

int storedSnapshotWidth(int width)
{
    return std::clamp(width, 0, kMaxWidth);
}

bool storeEventSnapshot(const QByteArray &jpeg, const QString &path)
{
    // The header says the size without decoding the picture.
    QBuffer header;
    header.setData(jpeg);
    header.open(QIODevice::ReadOnly);
    const QSize size = QImageReader(&header).size();
    if (!size.isValid() || size.width() <= kMaxWidth)
        return writeRaw(jpeg, path);   // as the camera sent it (or not a picture we can read)
    const QImage img = QImage::fromData(jpeg);
    if (img.isNull())
        return writeRaw(jpeg, path);
    return img.scaledToWidth(storedSnapshotWidth(img.width()), Qt::SmoothTransformation)
               .save(path, "JPG", 85)
           || writeRaw(jpeg, path);
}

} // namespace rl::device
