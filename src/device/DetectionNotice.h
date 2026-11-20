#pragma once

#include <QString>

namespace rl::device {

// The desktop notice for a detection: the camera's name over what it saw ("Person
// detected", ...). Unknown types read as motion; a camera without a name is "Camera".
// Translated in EventManager's context, where these strings were first marked.
struct DetectionNotice
{
    QString title;
    QString body;

    static DetectionNotice of(const QString &type, const QString &camera);
};

} // namespace rl::device
