#pragma once

#include "playback/TimelinePlayer.h"

#include <QtQml/qqmlregistration.h>

// Expone movix::playback::TimelinePlayer a QML (como tipo no instanciable: lo crea el
// ProjectController) sin que la biblioteca de reproducción dependa del módulo QML.
struct TimelinePlayerForeign {
    Q_GADGET
    QML_FOREIGN(movix::playback::TimelinePlayer)
    QML_NAMED_ELEMENT(TimelinePlayer)
    QML_UNCREATABLE("Lo crea ProjectController")
};
