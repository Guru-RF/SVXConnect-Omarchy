/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ptt/hyprlandbinding.h"
#include "core/svxcore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

namespace HyprlandBinding {

namespace {

const QLatin1String kBegin("-- >>> SVXConnect push-to-talk");
const QLatin1String kEnd("-- <<< SVXConnect push-to-talk");

QString tr(const char *text)
{
    return QCoreApplication::translate("HyprlandBinding", text);
}

struct Modifier { const char *name; int mask; };

/* Hyprland's modmask bits, in the order Omarchy writes chords. */
constexpr Modifier kModifiers[] = {
    {"SUPER", 64}, {"CTRL", 4}, {"ALT", 8}, {"SHIFT", 1},
    {"CAPS", 2},   {"MOD2", 16}, {"MOD3", 32}, {"MOD5", 128},
};

int modifierMask(const QString &token)
{
    const QString t = token.toUpper();
    if (t == QLatin1String("WIN") || t == QLatin1String("LOGO") || t == QLatin1String("MOD4"))
        return 64;
    if (t == QLatin1String("CONTROL"))
        return 4;
    if (t == QLatin1String("MOD1"))
        return 8;
    for (const Modifier &m : kModifiers)
        if (t == QLatin1String(m.name))
            return m.mask;
    return 0;
}

QStringList tokens(const QString &keys)
{
    static const QRegularExpression sep(QStringLiteral(R"([\s+]+)"));
    return keys.split(sep, Qt::SkipEmptyParts);
}

QString join(int mods, const QString &key)
{
    const QString m = modsToString(mods);
    return m.isEmpty() ? key : m + QStringLiteral(" + ") + key;
}

QByteArray hyprctl(const QStringList &args, bool *ok = nullptr)
{
    QProcess p;
    p.start(QStringLiteral("hyprctl"), args);
    const bool done = p.waitForFinished(3000)
                   && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (ok) *ok = done;
    return done ? p.readAllStandardOutput() : QByteArray();
}

/* `hyprctl configerrors` prints a blank line when the config is clean. */
QString configErrors()
{
    return QString::fromUtf8(hyprctl({QStringLiteral("configerrors")})).trimmed();
}

QString readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll());
}

bool writeFile(const QString &path, const QString &text, QString *error)
{
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = tr("Could not open %1 for writing.").arg(path);
        return false;
    }
    out.write(text.toUtf8());
    if (!out.commit()) {
        if (error) *error = tr("Could not save %1.").arg(path);
        return false;
    }
    return true;
}

QString luaString(QString s)
{
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

} // namespace

bool isHyprland()
{
    return !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE");
}

QString bindingsFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
         + QStringLiteral("/hypr/bindings.lua");
}

/* ---- pure ------------------------------------------------------------- */

QString modsToString(int modmask)
{
    QStringList out;
    for (const Modifier &m : kModifiers)
        if (modmask & m.mask)
            out << QLatin1String(m.name);
    return out.join(QStringLiteral(" + "));
}

Keys parseKeys(const QString &keys)
{
    Keys k;
    const QStringList t = tokens(keys);
    if (t.isEmpty())
        return k;

    for (int i = 0; i < t.size() - 1; ++i) {
        const int m = modifierMask(t.at(i));
        if (!m)
            return k;   /* a non-modifier before the key: not a chord */
        k.mods |= m;
    }

    k.key = t.last().toUpper();
    if (modifierMask(k.key))
        return k;       /* modifiers only, no key */

    k.ok = true;
    return k;
}

QString normalizeKeys(const QString &keys)
{
    const Keys k = parseKeys(keys);
    return k.ok ? join(k.mods, k.key) : keys.trimmed();
}

bool isSafeKeys(const QString &keys, QString *why)
{
    const Keys k = parseKeys(keys);
    if (!k.ok) {
        if (why)
            *why = tr("\"%1\" is not a key combination. Write it the way Hyprland does, "
                      "for example SUPER + GRAVE.").arg(keys.trimmed());
        return false;
    }

    /* Shift and Caps do not make a key safe: Shift+Return is still typed. */
    const bool realModifier = k.mods & ~(1 | 2);
    if (realModifier)
        return true;

    static const QRegularExpression untyped(
        QStringLiteral(R"(^(F([1-9]|1[0-9]|2[0-4])|PAUSE|SCROLL_LOCK|INSERT|MENU)$)"));
    if (untyped.match(k.key).hasMatch())
        return true;

    if (why)
        *why = tr("%1 on its own is a key you type. As a global push-to-talk key it would "
                  "transmit every time you pressed it in any application. Add SUPER, CTRL "
                  "or ALT, or use a key nobody types, such as F12 or PAUSE.")
                   .arg(join(k.mods, k.key));
    return false;
}

QString keysFromBindsJson(const QByteArray &json)
{
    const QJsonArray binds = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &v : binds) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("description")).toString() != QLatin1String(kDescription))
            continue;
        const QString key = o.value(QStringLiteral("key")).toString().toUpper();
        if (key.isEmpty())
            continue;
        return join(o.value(QStringLiteral("modmask")).toInt(), key);
    }
    return QString();
}

QString conflictFromBindsJson(const QByteArray &json, const QString &keys)
{
    const Keys want = parseKeys(keys);
    if (!want.ok)
        return QString();

    const QJsonArray binds = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &v : binds) {
        const QJsonObject o = v.toObject();
        const QString desc = o.value(QStringLiteral("description")).toString();
        if (desc == QLatin1String(kDescription))
            continue;
        if (o.value(QStringLiteral("modmask")).toInt() != want.mods)
            continue;
        if (o.value(QStringLiteral("key")).toString().compare(want.key, Qt::CaseInsensitive) != 0)
            continue;
        if (o.value(QStringLiteral("submap")).toString().size())
            continue;   /* only the global submap competes */
        return desc.isEmpty() ? tr("an existing binding") : desc;
    }
    return QString();
}

QString keysFromLua(const QString &luaText)
{
    static const QRegularExpression bind(
        QStringLiteral(R"re(^\s*(?:hl|o)\.bind\(\s*"([^"]+)".*SVXConnect:ptt)re"));
    const QStringList lines = luaText.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const auto m = bind.match(line);
        if (m.hasMatch())
            return normalizeKeys(m.captured(1));
    }
    return QString();
}

QString managedBlock(const QString &keys, const QString &replaces)
{
    const QString k = normalizeKeys(keys);
    QString b;
    b += kBegin + QLatin1Char('\n');
    b += QStringLiteral("-- Written by SVXConnect (Preferences > Push-to-talk). Hyprland passes both\n"
                        "-- the press and the release to SVXConnect, so holding the key transmits.\n");
    if (!replaces.isEmpty()) {
        QString what = replaces;
        what.replace(QLatin1Char('\n'), QLatin1Char(' '));
        b += QStringLiteral("-- %1 was: %2\n").arg(k, what);
        b += QStringLiteral("hl.unbind(%1)\n").arg(luaString(k));
    }
    b += QStringLiteral("hl.bind(%1, hl.dsp.global(%2),\n        { description = %3 })\n")
             .arg(luaString(k), luaString(QLatin1String(kShortcutId)),
                  luaString(QLatin1String(kDescription)));
    b += kEnd + QLatin1Char('\n');
    return b;
}

QString withManagedBlock(const QString &fileText, const QString &keys, const QString &replaces)
{
    const QString block = managedBlock(keys, replaces);

    const int begin = fileText.indexOf(kBegin);
    const int end   = begin < 0 ? -1 : fileText.indexOf(kEnd, begin);
    if (begin >= 0 && end >= 0) {
        int stop = end + kEnd.size();
        if (stop < fileText.size() && fileText.at(stop) == QLatin1Char('\n'))
            ++stop;
        return fileText.left(begin) + block + fileText.mid(stop);
    }

    QString out = fileText;
    if (!out.isEmpty() && !out.endsWith(QLatin1Char('\n')))
        out += QLatin1Char('\n');
    if (!out.isEmpty())
        out += QLatin1Char('\n');
    return out + block;
}

QString withoutManagedBlock(const QString &fileText)
{
    const int begin = fileText.indexOf(kBegin);
    const int end   = begin < 0 ? -1 : fileText.indexOf(kEnd, begin);
    if (begin < 0 || end < 0)
        return fileText;

    int start = begin;
    int stop  = end + kEnd.size();
    if (stop < fileText.size() && fileText.at(stop) == QLatin1Char('\n'))
        ++stop;
    /* Take back the blank line withManagedBlock() put in front. */
    if (start >= 2 && fileText.at(start - 1) == QLatin1Char('\n')
                   && fileText.at(start - 2) == QLatin1Char('\n'))
        --start;
    return fileText.left(start) + fileText.mid(stop);
}

QString fifoExample(const QString &fifoPath, const QString &keys)
{
    const QString k = normalizeKeys(keys);
    const QString on  = QStringLiteral("echo 'ptt on' > %1").arg(fifoPath);
    const QString off = QStringLiteral("echo 'ptt off' > %1").arg(fifoPath);
    return QStringLiteral("hl.bind(%1, hl.dsp.exec_cmd(%2))\n"
                          "hl.bind(%1, hl.dsp.exec_cmd(%3), { release = true })")
               .arg(luaString(k), luaString(on), luaString(off));
}

/* ---- live -------------------------------------------------------------- */

QString boundKeys()
{
    if (isHyprland()) {
        bool ok = false;
        const QByteArray json = hyprctl({QStringLiteral("binds"), QStringLiteral("-j")}, &ok);
        if (ok) {
            const QString keys = keysFromBindsJson(json);
            if (!keys.isEmpty())
                return keys;
        }
    }
    return keysFromLua(readFile(bindingsFile()));
}

QString conflictFor(const QString &keys)
{
    if (!isHyprland())
        return QString();
    return conflictFromBindsJson(hyprctl({QStringLiteral("binds"), QStringLiteral("-j")}), keys);
}

bool install(const QString &keys, const QString &replaces, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };

    if (!isHyprland())
        return fail(tr("Hyprland is not running in this session."));

    QString why;
    if (!isSafeKeys(keys, &why))
        return fail(why);

    const QString path = bindingsFile();
    QDir().mkpath(QFileInfo(path).absolutePath());

    const QString original = readFile(path);
    const QString before   = configErrors();

    /* Omarchy's own habit: a timestamped copy next to the file. */
    if (QFileInfo::exists(path)) {
        const QString backup = path + QStringLiteral(".bak.")
                             + QString::number(QDateTime::currentSecsSinceEpoch());
        if (!QFile::copy(path, backup))
            return fail(tr("Could not back up %1 before changing it.").arg(path));
    }

    if (!writeFile(path, withManagedBlock(original, keys, replaces), error))
        return false;

    bool reloaded = false;
    hyprctl({QStringLiteral("reload")}, &reloaded);
    const QString after = configErrors();

    if (!after.isEmpty() && after != before) {
        writeFile(path, original, nullptr);
        hyprctl({QStringLiteral("reload")});
        log_err("ptt: Hyprland rejected the binding, rolled back: %s", qPrintable(after));
        return fail(tr("Hyprland rejected the binding, so bindings.lua was put back as it "
                       "was.\n\n%1").arg(after));
    }

    log_info("ptt: bound %s to %s in %s", qPrintable(normalizeKeys(keys)),
             kShortcutId, qPrintable(path));
    if (!reloaded)
        log_warn("ptt: hyprctl reload did not answer; Hyprland reloads on save anyway");
    return true;
}

bool remove(QString *error)
{
    const QString path = bindingsFile();
    const QString original = readFile(path);
    const QString updated = withoutManagedBlock(original);
    if (updated == original)
        return true;

    if (!writeFile(path, updated, error))
        return false;
    if (isHyprland())
        hyprctl({QStringLiteral("reload")});
    log_info("ptt: removed the push-to-talk binding from %s", qPrintable(path));
    return true;
}

bool openBindingsFile()
{
    const QString path = bindingsFile();
    if (!QStandardPaths::findExecutable(QStringLiteral("omarchy-launch-editor")).isEmpty()
        && QProcess::startDetached(QStringLiteral("omarchy-launch-editor"), {path}))
        return true;
    return QProcess::startDetached(QStringLiteral("xdg-open"), {path});
}

} // namespace HyprlandBinding
