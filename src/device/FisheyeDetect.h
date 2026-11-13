#pragma once

#include <QSize>

namespace rl::device {

// Whether a camera is a fisheye, on every Site (wayfinder/fisheye-and-framing ticket 12).
// A fisheye records a full circle in a square frame, so a square main stream is one. The size
// comes from GetEnc each time the recorder is read; when that read fails (the RLN8 answers
// some proxied camera commands with HTTP 502) the lens seen on an earlier read stands, so the
// views don't vanish until the next good read.

// What a read teaches about the lens: 1 fisheye (square), 0 not, -1 nothing (size not read).
inline int lensSeen(QSize mainSize)
{
    if (mainSize.width() <= 0 || mainSize.height() <= 0)
        return -1;
    return mainSize.width() == mainSize.height() ? 1 : 0;
}

// userChoice: the person's "Fisheye lens" menu tick (-1 not set, 0 no, 1 yes), which wins.
// remembered: lensSeen() of an earlier read (-1 none).
inline bool isFisheye(int userChoice, QSize mainSize, int remembered)
{
    if (userChoice >= 0)
        return userChoice == 1;
    const int seen = lensSeen(mainSize);
    return (seen >= 0 ? seen : remembered) == 1;
}

} // namespace rl::device
