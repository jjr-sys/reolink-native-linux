#include "device/DetectionNotice.h"

#include <QCoreApplication>

namespace rl::device {

// The context EventManager's tr() used, so existing translations still match.
DetectionNotice DetectionNotice::of(const QString &type, const QString &camera)
{
    QString what;
    if (type == QLatin1String("person"))
        what = QCoreApplication::translate("rl::EventManager", "Person detected");
    else if (type == QLatin1String("vehicle"))
        what = QCoreApplication::translate("rl::EventManager", "Vehicle detected");
    else if (type == QLatin1String("pet") || type == QLatin1String("dog_cat"))
        what = QCoreApplication::translate("rl::EventManager", "Pet detected");
    else if (type == QLatin1String("visitor"))
        what = QCoreApplication::translate("rl::EventManager", "Visitor at the door");
    else
        what = QCoreApplication::translate("rl::EventManager", "Motion detected");
    return {camera.isEmpty() ? QCoreApplication::translate("rl::EventManager", "Camera") : camera,
            what};
}

} // namespace rl::device
