#pragma once
// Własne ikony folderów ustawione w systemie:
//  - Windows: desktop.ini w folderze ([.ShellClassInfo] IconResource= / IconFile=)
//  - Linux (KDE): plik .directory z kluczem Icon=
// Folder bez własnej ikony → null (program rysuje swoją domyślną).
#include <QString>
#include <QPixmap>
#include <QIcon>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>
#include <QPixmapCache>
#include <QFileIconProvider>
#include <QStringList>

namespace LapesEye {

namespace folder_icon_detail {

inline QString read_text_file(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.read(64 * 1024);
    if (raw.size() >= 2 && (uchar)raw[0] == 0xFF && (uchar)raw[1] == 0xFE)       // UTF-16 LE
        return QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData() + 2),
                                  (raw.size() - 2) / 2);
    if (raw.size() >= 2 && (uchar)raw[0] == 0xFE && (uchar)raw[1] == 0xFF) {     // UTF-16 BE
        QString s;
        for (int i = 2; i + 1 < raw.size(); i += 2)
            s += QChar((uchar)raw[i] << 8 | (uchar)raw[i + 1]);
        return s;
    }
    if (raw.startsWith("\xEF\xBB\xBF")) return QString::fromUtf8(raw.mid(3));
    return QString::fromLocal8Bit(raw);
}

// Zwraca wartość klucza z danej sekcji pliku INI (bez rozróżniania wielkości liter)
inline QString ini_value(const QString& text, const QString& section, const QString& key) {
    QString cur;
    const QStringList lines = text.split('\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith('[') && line.endsWith(']')) { cur = line.mid(1, line.size() - 2); continue; }
        if (cur.compare(section, Qt::CaseInsensitive) != 0) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        if (line.left(eq).trimmed().compare(key, Qt::CaseInsensitive) == 0)
            return line.mid(eq + 1).trimmed();
    }
    return {};
}

struct Entry { bool has = false; QString icon_spec; qint64 checked_ms = 0; };

inline QHash<QString, Entry>& cache() { static QHash<QString, Entry> c; return c; }
inline QMutex& cache_mutex() { static QMutex m; return m; }

inline Entry probe(const QString& dir) {
    Entry e;
#ifdef Q_OS_WIN
    const QString ini = dir + "/desktop.ini";
    if (QFileInfo::exists(ini)) {
        const QString t = read_text_file(ini);
        const QString res  = ini_value(t, ".ShellClassInfo", "IconResource");
        const QString file = ini_value(t, ".ShellClassInfo", "IconFile");
        if (!res.isEmpty() || !file.isEmpty()) { e.has = true; e.icon_spec = res.isEmpty() ? file : res; }
    }
#else
    const QString dd = dir + "/.directory";
    if (QFileInfo::exists(dd)) {
        const QString t = read_text_file(dd);
        const QString icon = ini_value(t, "Desktop Entry", "Icon");
        if (!icon.isEmpty()) { e.has = true; e.icon_spec = icon; }
    }
#endif
    return e;
}

} // namespace folder_icon_detail

// Czy folder ma własną ikonę ustawioną w systemie (wynik w cache na 30 s)
inline bool has_custom_folder_icon(const QString& dir) {
    using namespace folder_icon_detail;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker lk(&cache_mutex());
        auto it = cache().constFind(dir);
        if (it != cache().constEnd() && now - it->checked_ms < 30000) return it->has;
    }
    Entry e = probe(dir);
    e.checked_ms = now;
    QMutexLocker lk(&cache_mutex());
    cache().insert(dir, e);
    return e.has;
}

// Własna ikona folderu jako pixmapa (tylko wątek GUI). Null = brak własnej ikony.
inline QPixmap custom_folder_icon(const QString& dir, int px) {
    using namespace folder_icon_detail;
    if (px <= 0 || !has_custom_folder_icon(dir)) return {};
    const QString key = "foldericon:" + dir + "@" + QString::number(px);
    QPixmap pm;
    if (QPixmapCache::find(key, &pm)) return pm;

    QString spec;
    { QMutexLocker lk(&cache_mutex()); spec = cache().value(dir).icon_spec; }

#ifdef Q_OS_WIN
    // System sam wie, jak wyciągnąć ikonę (plik .ico, exe/dll z indeksem itd.)
    QIcon ic = QFileIconProvider().icon(QFileInfo(dir));
    if (!ic.isNull()) pm = ic.pixmap(px, px);
    Q_UNUSED(spec);
#else
    QString name = spec;
    if (name.startsWith("~/")) name = QDir::homePath() + name.mid(1);
    if (QFileInfo(name).isAbsolute() && QFileInfo::exists(name))
        pm = QPixmap(name);
    else {
        QIcon ic = QIcon::fromTheme(name);
        if (!ic.isNull()) pm = ic.pixmap(px, px);
    }
#endif
    if (pm.isNull()) return {};
    QPixmapCache::insert(key, pm);
    return pm;
}

} // namespace LapesEye
