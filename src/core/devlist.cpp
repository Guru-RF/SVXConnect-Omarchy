/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "core/devlist.h"
#include "core/svxcore.h"

#include <vector>

namespace DevList {

QVector<Device> list(bool capture)
{
    /* The core takes a caller-supplied array. 64 is well past any realistic
     * machine; a truncated list would silently hide a device. */
    std::vector<svx_devinfo> raw(64);
    const int n = svx_audio_list(capture ? 1 : 0, raw.data(), static_cast<int>(raw.size()));

    QVector<Device> out;
    if (n <= 0)
        return out;

    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        Device d;
        d.id        = QString::fromUtf8(raw[i].id);
        d.name      = QString::fromUtf8(raw[i].name);
        d.isDefault = raw[i].is_default != 0;
        out.append(d);
    }
    return out;
}

Resolution resolve(bool capture, const QString &want)
{
    svx_devinfo info{};
    const int matched = svx_audio_resolve(capture ? 1 : 0, qPrintable(want), &info);

    Resolution r;
    r.matched          = (matched == 1);
    r.device.id        = QString::fromUtf8(info.id);
    r.device.name      = QString::fromUtf8(info.name);
    r.device.isDefault = info.is_default != 0;
    return r;
}

} // namespace DevList
