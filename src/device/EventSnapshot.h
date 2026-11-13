#pragma once

#include <QByteArray>
#include <QString>

namespace rl::device {

// An event's snapshot is kept in HD (wayfinder/fisheye-and-framing ticket 11): the width it is
// stored at for a picture `width` px wide (at most 2560).
int storedSnapshotWidth(int width);

// Store an event snapshot at `path`: the camera's own JPEG untouched up to 2560 px wide (it is
// sharper and smaller than re-encoding it), a wider picture shrunk to 2560 px; anything that
// isn't a readable picture is written as it came. False if nothing could be written.
bool storeEventSnapshot(const QByteArray &jpeg, const QString &path);

} // namespace rl::device
