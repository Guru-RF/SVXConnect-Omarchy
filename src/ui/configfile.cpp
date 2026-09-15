/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/configfile.h"
#include "core/svxcore.h"

#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QDesktopServices>
#include <QProcess>
#include <QMessageBox>
#include <QCoreApplication>
#include <QSaveFile>
#include <QByteArray>
#include <QTemporaryFile>
#include <QStandardPaths>

#include <cstdio>

namespace ConfigFile {

bool exists(const QString &path)
{
    const QFileInfo fi(path);
    return fi.exists() && fi.isFile() && fi.isReadable();
}

bool createDefault(const QString &path, const svx_config *cfg, QString *errorOut)
{
    auto fail = [errorOut](const QString &why) {
        if (errorOut) *errorOut = why;
        return false;
    };

    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath()))
        return fail(QCoreApplication::translate("ConfigFile",
            "Could not create the directory %1.").arg(fi.absolutePath()));

    /* config_dump() writes to a FILE*, so render it into a temporary file
     * first and then place the result atomically. Building the text in memory
     * would mean reimplementing the key table, which is exactly the
     * duplication this approach avoids. */
    svx_config defaults;
    if (!cfg) {
        config_defaults(&defaults);
        cfg = &defaults;
    }

    QTemporaryFile tmp;
    if (!tmp.open())
        return fail(QCoreApplication::translate("ConfigFile",
            "Could not create a temporary file."));
    tmp.close();

    std::FILE *f = std::fopen(qPrintable(tmp.fileName()), "we");
    if (!f)
        return fail(QCoreApplication::translate("ConfigFile",
            "Could not write the temporary file."));
    config_dump(cfg, f);
    std::fclose(f);

    if (!tmp.open())
        return fail(QCoreApplication::translate("ConfigFile",
            "Could not read back the generated configuration."));
    const QByteArray body = tmp.readAll();
    tmp.close();

    /* A header, because a bare dump of forty keys tells a first-time user
     * nothing about which two of them they actually have to fill in. */
    const QByteArray header =
        "# SVXConnect configuration\n"
        "#\n"
        "# Shared verbatim with the svxconnect command-line client: one file,\n"
        "# one identity, one enrolment. Either program may be the one running,\n"
        "# but only one at a time may hold the reflector connection.\n"
        "#\n"
        "# Generated from this build's own defaults, so every key it understands\n"
        "# is listed below with its current value.\n"
        "#\n"
        "# To get on the air you must set at least:\n"
        "#     callsign   your callsign, e.g. ON6URE\n"
        "#     email      where the reflector sysop can reach you\n"
        "#     reflector  the reflector host, e.g. be.svx.link\n"
        "# then enrol once:   svxconnect --enroll\n"
        "#\n"
        "# Talkgroup priority is written as trailing '+' characters:\n"
        "#     8 = normal,  8+ = higher,  8++ = highest\n"
        "#\n"
        "# '#' or ';' starts a comment. Restart SVXConnect after editing.\n"
        "\n";

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(QCoreApplication::translate("ConfigFile",
            "Could not open %1 for writing.").arg(path));
    out.write(header);
    out.write(body);
    if (!out.commit())
        return fail(QCoreApplication::translate("ConfigFile",
            "Could not save %1.").arg(path));

    /* The file carries no secret — the private key lives in pki_dir — but it
     * does carry an email address, so keep it to the owner. */
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    log_info("wrote a starter configuration to %s", qPrintable(path));
    return true;
}

namespace {

bool handOff(const QString &target, QWidget *parent, const QString &whatFailed)
{
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(target)))
        return true;

    /* QDesktopServices can fail when no portal is reachable and the .desktop
     * database is not where Qt expects it. xdg-open uses a different lookup
     * and often succeeds where it did not. */
    if (QProcess::startDetached(QStringLiteral("xdg-open"), {target}))
        return true;

    log_warn("could not open %s with any desktop handler", qPrintable(target));

    if (parent) {
        QMessageBox box(parent);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QCoreApplication::translate("ConfigFile",
            "Could not open the editor"));
        box.setText(whatFailed);
        /* Give them the path in a selectable form rather than a dead end. */
        box.setInformativeText(QCoreApplication::translate("ConfigFile",
            "Open this path manually:\n\n%1").arg(target));
        box.setTextInteractionFlags(Qt::TextSelectableByMouse);
        box.exec();
    }
    return false;
}

} // namespace

bool openInEditor(const QString &path, QWidget *parent)
{
    /* On Omarchy the editor is the user's choice, recorded by the Omarchy
     * defaults and launched by omarchy-launch-editor — in a terminal when it is
     * a TUI such as Neovim. That is what Omarchy's own "edit config" menu
     * entries use, so a config file opens where every other one does. */
    if (!QStandardPaths::findExecutable(QStringLiteral("omarchy-launch-editor")).isEmpty()
        && QProcess::startDetached(QStringLiteral("omarchy-launch-editor"), {path}))
        return true;

    return handOff(path, parent, QCoreApplication::translate("ConfigFile",
        "No application is registered to open plain text files."));
}

bool openContainingFolder(const QString &path, QWidget *parent)
{
    /* There is no portable reveal-and-select on Linux, so open the directory.
     * Doing this properly would mean a org.freedesktop.FileManager1.ShowItems
     * D-Bus call, which not every file manager implements. */
    return handOff(QFileInfo(path).absolutePath(), parent,
        QCoreApplication::translate("ConfigFile",
            "No application is registered to open folders."));
}

} // namespace ConfigFile
