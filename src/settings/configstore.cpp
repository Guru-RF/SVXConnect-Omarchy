/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "settings/configstore.h"

#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

namespace {

/* Where an unescaped comment starts, or -1.
 *
 * Mirrors strip_comment() (config.c:376-384) exactly. Note the escape is
 * recognised ONLY before '#' or ';' — a backslash anywhere else is an ordinary
 * character, so a path like C:\tools must not be treated as an escape. */
int commentStart(const QString &line)
{
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('\\') && i + 1 < line.size()) {
            const QChar n = line.at(i + 1);
            if (n == QLatin1Char('#') || n == QLatin1Char(';')) {
                ++i;                    /* \# and \; are literals */
                continue;
            }
        }
        if (c == QLatin1Char('#') || c == QLatin1Char(';'))
            return i;
    }
    return -1;
}

/* Encode a value so config_load() reads back exactly what was set.
 *
 * QUOTING DOES NOT PROTECT A '#'. This is the trap, and the core says so in as
 * many words at config.c:374 — "Quoting does not protect a '#' — keeping one
 * rule beats a clever one nobody remembers." The reason is ordering:
 * config_load() calls strip_comment() on the raw line FIRST and strip_quotes()
 * on the value afterwards, so writing
 *
 *     location = "Gent #2"
 *
 * loses everything from the '#' onward before the quotes are ever considered,
 * and the value comes back as a dangling `"Gent`. The backslash escape is the
 * only thing that works, and it covers ';' as well.
 *
 * Quotes are still needed for one thing: leading or trailing whitespace, which
 * str_trim() would otherwise eat. */
QString encodeValue(const QString &v)
{
    QString out = v;
    out.replace(QLatin1Char('#'), QLatin1String("\\#"));
    out.replace(QLatin1Char(';'), QLatin1String("\\;"));

    if (out != out.trimmed())
        out = QLatin1Char('"') + out + QLatin1Char('"');

    return out;
}

/* The key on a line, lower-cased, or an empty string if the line does not
 * assign one. Applies the parser's rules to a copy; the original is untouched. */
QString keyOnLine(const QString &line)
{
    const int c = commentStart(line);
    const QString body = (c >= 0 ? line.left(c) : line).trimmed();
    if (body.isEmpty())
        return QString();

    const int eq = body.indexOf(QLatin1Char('='));
    if (eq < 0)
        return QString();

    return body.left(eq).trimmed().toLower();
}

} // namespace

ConfigStore::ConfigStore(const svx_config *cfg, QString path)
    : m_cfg(cfg), m_path(std::move(path))
{
}

QString ConfigStore::value(const QString &key) const
{
    const QString k = key.toLower();
    if (m_changes.contains(k))
        return m_changes.value(k);

    if (!m_cfg)
        return QString();

    /* Read the current value out of the live struct the same way the core
     * would print it, so that "is this a change?" comparisons are exact.
     * config_dump() formats the whole struct; doing it per key here keeps the
     * comparison honest without a second format table. */
    if (k == QLatin1String("callsign"))      return QString::fromUtf8(m_cfg->callsign);
    if (k == QLatin1String("email"))         return QString::fromUtf8(m_cfg->email);
    if (k == QLatin1String("reflector"))     return QString::fromUtf8(m_cfg->reflector);
    if (k == QLatin1String("port"))          return QString::number(m_cfg->port);
    if (k == QLatin1String("pki_dir"))       return QString::fromUtf8(m_cfg->pki_dir);
    if (k == QLatin1String("location"))      return QString::fromUtf8(m_cfg->location);
    if (k == QLatin1String("latitude"))      return QString::number(m_cfg->latitude, 'f', 7);
    if (k == QLatin1String("longitude"))     return QString::number(m_cfg->longitude, 'f', 7);

    if (k == QLatin1String("default_tg"))    return QString::number(m_cfg->default_tg);
    if (k == QLatin1String("lock_on_start")) return m_cfg->lock_on_start ? QStringLiteral("on")
                                                                        : QStringLiteral("off");
    if (k == QLatin1String("linger_seconds"))return QString::number(m_cfg->linger_seconds);
    if (k == QLatin1String("idle_seconds"))  return QString::number(m_cfg->idle_seconds);
    if (k == QLatin1String("tg_order"))      return QString::fromUtf8(m_cfg->tg_order);

    if (k == QLatin1String("input_device"))  return QString::fromUtf8(m_cfg->input_device);
    if (k == QLatin1String("output_device")) return QString::fromUtf8(m_cfg->output_device);
    if (k == QLatin1String("output_volume_pct")) return QString::number(m_cfg->output_volume_pct);
    if (k == QLatin1String("mic_agc"))       return m_cfg->mic_agc ? QStringLiteral("on")
                                                                   : QStringLiteral("off");
    if (k == QLatin1String("mic_agc_target_pct")) return QString::number(m_cfg->mic_agc_target_pct);
    if (k == QLatin1String("mic_gain"))      return QString::number(m_cfg->mic_gain);
    if (k == QLatin1String("jitter_ms"))     return QString::number(m_cfg->jitter_ms);
    if (k == QLatin1String("tail_trim_ms"))  return QString::number(m_cfg->tail_trim_ms);
    if (k == QLatin1String("roger_beep"))    return m_cfg->roger_beep ? QStringLiteral("on")
                                                                      : QStringLiteral("off");
    if (k == QLatin1String("roger_beep_min_sec")) return QString::number(m_cfg->roger_beep_min_sec);
    if (k == QLatin1String("auto_duck"))     return m_cfg->auto_duck ? QStringLiteral("on")
                                                                     : QStringLiteral("off");
    if (k == QLatin1String("auto_duck_quiet_pct")) return QString::number(m_cfg->auto_duck_quiet_pct);

    if (k == QLatin1String("tx_timeout_sec"))return QString::number(m_cfg->tx_timeout_sec);
    if (k == QLatin1String("ctl_fifo"))      return QString::fromUtf8(m_cfg->ctl_fifo);
    if (k == QLatin1String("log_file"))      return QString::fromUtf8(m_cfg->log_file);
    if (k == QLatin1String("log_level"))     return QString::fromUtf8(m_cfg->log_level);
    if (k == QLatin1String("lock_file"))     return QString::fromUtf8(m_cfg->lock_file);
    if (k == QLatin1String("status_file"))   return QString::fromUtf8(m_cfg->status_file);

    if (k == QLatin1String("switchable") || k == QLatin1String("monitored")) {
        const svx_tg_entry *v = (k == QLatin1String("switchable"))
                                    ? m_cfg->switchable : m_cfg->monitored;
        const int n = (k == QLatin1String("switchable"))
                          ? m_cfg->n_switchable : m_cfg->n_monitored;
        char buf[1024];
        tglist_format(buf, sizeof buf, v, n);
        return QString::fromUtf8(buf);
    }

    return QString();
}

int  ConfigStore::valueInt(const QString &key) const  { return value(key).toInt(); }

bool ConfigStore::valueBool(const QString &key) const
{
    const QString v = value(key).trimmed().toLower();
    return v == QLatin1String("on") || v == QLatin1String("yes")
        || v == QLatin1String("true") || v == QLatin1String("1");
}

void ConfigStore::set(const QString &key, const QString &newValue)
{
    const QString k = key.toLower();

    /* Compare against the LIVE value, not the staged one, so that changing a
     * control and changing it back leaves the file untouched. Without this,
     * opening Preferences and closing it would rewrite the file. */
    m_changes.remove(k);
    if (value(k) == newValue)
        return;

    m_changes.insert(k, newValue);
}

void ConfigStore::setInt(const QString &key, int v)
{
    set(key, QString::number(v));
}

void ConfigStore::setBool(const QString &key, bool v)
{
    set(key, v ? QStringLiteral("on") : QStringLiteral("off"));
}

QStringList ConfigStore::changedKeys() const
{
    QStringList keys = m_changes.keys();
    keys.sort();
    return keys;
}

bool ConfigStore::save(QString *errorOut)
{
    auto fail = [errorOut](const QString &why) {
        if (errorOut) *errorOut = why;
        return false;
    };

    if (m_changes.isEmpty())
        return true;

    if (m_path.isEmpty())
        return fail(QCoreApplication::translate("ConfigStore",
            "No configuration file path is set."));

    QFile in(m_path);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
        return fail(QCoreApplication::translate("ConfigStore",
            "Could not read %1.").arg(m_path));
    const QByteArray original = in.readAll();
    in.close();

    /* Split keeping the line endings, so a file that uses CRLF, or one with no
     * trailing newline, comes back out the way it went in. */
    const QString text = QString::fromUtf8(original);
    QStringList lines = text.split(QLatin1Char('\n'));

    QSet<QString> written;

    for (QString &line : lines) {
        const QString k = keyOnLine(line);
        if (k.isEmpty() || !m_changes.contains(k))
            continue;

        /* Rewrite EVERY occurrence, not just the first: config_load() calls
         * config_set() for each, so the last one wins. Leaving an earlier
         * duplicate at the old value would be harmless; leaving a LATER one
         * would silently discard the change. */
        const int cStart = commentStart(line);
        const QString comment = (cStart >= 0) ? line.mid(cStart) : QString();
        const QString body    = (cStart >= 0) ? line.left(cStart) : line;

        const int eq = body.indexOf(QLatin1Char('='));

        /* Everything up to and including '=' is preserved verbatim: the
         * leading indent, the key as the user capitalised it, and whatever
         * padding they used to line the '=' up with its neighbours. */
        const QString lhs = body.left(eq + 1);

        /* Reproduce the run of spaces that followed '=', so a column-aligned
         * file stays aligned. `example.conf` pads to a fixed column, and a
         * save that collapsed that to one space would show up as a diff on
         * every line it touched. */
        const QString afterEq = body.mid(eq + 1);
        int pad = 0;
        while (pad < afterEq.size() && afterEq.at(pad) == QLatin1Char(' '))
            ++pad;
        const QString spacing = (pad > 0) ? QString(pad, QLatin1Char(' '))
                                          : QStringLiteral(" ");

        QString rebuilt = lhs + spacing + encodeValue(m_changes.value(k));

        /* Put the trailing comment back where it was if it still fits the
         * original column; otherwise just append it with a single space. */
        if (!comment.isEmpty()) {
            if (rebuilt.size() < cStart)
                rebuilt += QString(cStart - rebuilt.size(), QLatin1Char(' '));
            else
                rebuilt += QLatin1Char(' ');
            rebuilt += comment;
        }

        line = rebuilt;
        written.insert(k);
    }

    /* Keys the file never mentioned. Appended in one clearly-marked block so
     * the user can see what the dialog added rather than finding new keys
     * scattered through their file. */
    QStringList missing;
    for (auto it = m_changes.constBegin(); it != m_changes.constEnd(); ++it)
        if (!written.contains(it.key()))
            missing.append(it.key());
    missing.sort();

    if (!missing.isEmpty()) {
        if (!lines.isEmpty() && !lines.last().trimmed().isEmpty())
            lines.append(QString());
        lines.append(QStringLiteral("# --- added by SVXConnect ---"));
        for (const QString &k : std::as_const(missing))
            lines.append(QStringLiteral("%1 = %2")
                             .arg(k, encodeValue(m_changes.value(k))));
        lines.append(QString());
    }

    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(QCoreApplication::translate("ConfigStore",
            "Could not open %1 for writing.").arg(m_path));
    out.write(lines.join(QLatin1Char('\n')).toUtf8());
    if (!out.commit())
        return fail(QCoreApplication::translate("ConfigStore",
            "Could not save %1.").arg(m_path));

    log_info("configuration saved: %s", qPrintable(changedKeys().join(QLatin1String(", "))));
    m_changes.clear();
    return true;
}
