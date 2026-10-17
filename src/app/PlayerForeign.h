#pragma once

#include "playback/Player.h"

#include <QtQml/qqmlregistration.h>

// Expone movix::playback::Player a QML como tipo "Player" sin que la biblioteca de
// reproducción dependa del módulo QML.
struct PlayerForeign {
    Q_GADGET
    QML_FOREIGN(movix::playback::Player)
    QML_NAMED_ELEMENT(Player)
};
