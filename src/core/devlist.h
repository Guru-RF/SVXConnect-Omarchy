/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Audio device enumeration for the pickers.
 *
 * Thin, but not trivial, because of one thing worth surfacing:
 * svx_audio_resolve() returns 1 when it matched a specific device and 0 when
 * it fell through to the system default. That zero is the only way to learn
 * that a device the user pinned is no longer present — a USB headset that is
 * unplugged, a dock that is undocked — and the picker must SAY so rather than
 * quietly showing the default as though nothing happened. The macOS app
 * resets the stored id to "default" and logs; doing nothing would leave a
 * stale id in the shared config file that the terminal client then inherits.
 *
 * svx_audio_list() spins up a miniaudio context and takes tens to hundreds of
 * milliseconds, so it is called when a picker opens, never on a repaint tick.
 */
#ifndef SVXCONNECT_OMARCHY_DEVLIST_H
#define SVXCONNECT_OMARCHY_DEVLIST_H

#include <QString>
#include <QVector>

namespace DevList {

struct Device {
    QString id;        /* backend-stable; empty means the system default */
    QString name;      /* what to show a human */
    bool    isDefault = false;
};

/* Enumerate. `capture` selects inputs; otherwise outputs. Blocking. */
QVector<Device> list(bool capture);

struct Resolution {
    Device  device;
    bool    matched = false;   /* false => fell through to the system default */
};

/* What `want` actually resolves to right now. `matched == false` with a
 * non-empty `want` is the "your pinned device is gone" case. */
Resolution resolve(bool capture, const QString &want);

} // namespace DevList

#endif
