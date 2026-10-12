#include "LapesEye/core/ThumbCache.h"
#include <QPixmapCache>
#include <QSettings>
#include <QMutexLocker>
#include <QHash>
#include <QtConcurrent/QtConcurrent>

#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QBuffer>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QVariant>

namespace LapesEye {

ThumbCache::ThumbCache() {}
ThumbCache::~ThumbCache() { close(); }

QString ThumbCache::norm(const QString& path) {
    QString p = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    if (p.size() >= 2 && p[1] == ':') p[0] = p[0].toUpper();
#endif
    return p;
}

QString ThumbCache::volume_root(const QString& path) {
#ifdef Q_OS_WIN
    return path.left(3);   // "D:/"
#else
    static const char* prefixes[] = { "/run/media/", "/media/", "/mnt/" };
    for (const char* pf : prefixes) {
        const QString pre = QString::fromLatin1(pf);
        if (path.startsWith(pre)) {
            const QStringList parts = path.mid(pre.size()).split('/', Qt::SkipEmptyParts);
            const int take = (pre == "/mnt/") ? 1 : 2;   // user/etykieta albo nazwa
            if (parts.size() >= take)
                return pre + parts.mid(0, take).join('/');
        }
    }
    return "/";
#endif
}

QString ThumbCache::db_path() const {
    QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cache);
    return cache + "/thumbs.db";
}

bool ThumbCache::open() {
    // Unikalne połączenie per instancja — używamy wskaźnika this jako ID
    m_conn_name = QString("leye_thumbcache_%1")
                  .arg(reinterpret_cast<quintptr>(this));
    if (QSqlDatabase::contains(m_conn_name))
        QSqlDatabase::removeDatabase(m_conn_name);
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_conn_name);
    m_db.setDatabaseName(db_path());
    if (!m_db.open()) return false;

    // Optymalizacje SQLite dla szybkiego odczytu miniatur
    QSqlQuery pragma(m_db);
    pragma.exec("PRAGMA journal_mode = WAL");       // Write-Ahead Log — szybszy zapis
    pragma.exec("PRAGMA synchronous = NORMAL");     // mniej fsync
    pragma.exec("PRAGMA cache_size = -65536");      // 64MB cache strony SQLite
    pragma.exec("PRAGMA mmap_size = 268435456");    // 256MB memory-mapped I/O
    pragma.exec("PRAGMA temp_store = MEMORY");      // tabele tymczasowe w RAM

    // RAM cache QPixmap — 512MB (QPixmapCache jest globalny dla całej apki)
    QPixmapCache::setCacheLimit(512 * 1024);  // w KB

    m_open = create_schema();
    return m_open;
}

void ThumbCache::close() {
    m_bg.waitForFinished();   // dokończ zadania w tle zanim zamkniemy bazę
    if (m_open) {
        {
            QMutexLocker lock(&m_mutex);
            flush_batch_locked();
        }
        m_db.close();
        m_db = QSqlDatabase();  // release referencji przed removeDatabase
        m_open = false;
        if (!m_conn_name.isEmpty()) {
            QSqlDatabase::removeDatabase(m_conn_name);
            m_conn_name.clear();
        }
    }
}

bool ThumbCache::create_schema() {
    QSqlQuery q(m_db);
    bool ok = q.exec(R"(
        CREATE TABLE IF NOT EXISTS thumbnails (
            path       TEXT    PRIMARY KEY,
            mtime      INTEGER NOT NULL,
            size       INTEGER NOT NULL,
            thumb_png  BLOB    NOT NULL,
            width      INTEGER NOT NULL DEFAULT 0,
            height     INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL
        )
    )");
    // Migracje — dodaj kolumny jeśli nie istnieją (błąd "duplicate column" ignorowany)
    q.exec("ALTER TABLE thumbnails ADD COLUMN thumb_jpeg BLOB");
    q.exec("ALTER TABLE thumbnails ADD COLUMN thumb_size INTEGER NOT NULL DEFAULT 0");
    q.exec("ALTER TABLE thumbnails ADD COLUMN last_used INTEGER NOT NULL DEFAULT 0");
    return ok;
}

std::optional<QImage> ThumbCache::get(const QString& path_in) {
    if (!m_open) return std::nullopt;
    const QString path = norm(path_in);

    // Dane pliku z dysku PRZED blokadą — stat nie blokuje innych wątków
    QFileInfo fi(path_in);
    const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
    const qint64 size  = fi.size();

    QByteArray blob;
    const char* fmt = "JPEG";
    {
        QMutexLocker lock(&m_mutex);
        QSqlQuery q(m_db);
        q.prepare("SELECT thumb_jpeg, thumb_png, mtime, size FROM thumbnails WHERE path = ?");
        q.addBindValue(path);
        if (!q.exec() || !q.next()) return std::nullopt;
        if (q.value(2).toLongLong() != mtime || q.value(3).toLongLong() != size)
            return std::nullopt;
        blob = q.value(0).toByteArray();
        if (blob.isEmpty()) {
            blob = q.value(1).toByteArray();
            fmt = "PNG";
        }
        m_touched.insert(path);   // last_used zapisze się przy najbliższym flush
    }

    // Dekodowanie JPEG poza blokadą — wątki nie czekają na siebie
    QImage img;
    if (!img.loadFromData(blob, fmt) || img.isNull()) return std::nullopt;
    return img;
}

void ThumbCache::put(const QString& path_in, const QImage& thumb,
                     int orig_w, int orig_h)
{
    if (!m_open || thumb.isNull()) return;

    QFileInfo fi(path_in);
    qint64 mtime = fi.lastModified().toSecsSinceEpoch();
    qint64 size  = fi.size();
    qint64 now   = QDateTime::currentSecsSinceEpoch();

    // JPEG quality 85 — 4× mniejszy od PNG, 10× szybszy odczyt
    QByteArray blob;
    QBuffer buf(&blob);
    buf.open(QIODevice::WriteOnly);
    thumb.save(&buf, "JPEG", 85);

    QMutexLocker lock(&m_mutex);

    // Dodaj do kolejki batch — nie zapisuj od razu żeby uniknąć setek
    // osobnych transakcji przy ładowaniu dużego folderu
    BatchEntry entry;
    entry.path      = norm(path_in);
    entry.mtime     = mtime;
    entry.size      = size;
    entry.blob      = blob;
    entry.orig_w    = orig_w;
    entry.orig_h    = orig_h;
    entry.thumb_w   = thumb.width();
    entry.now       = now;
    m_batch.append(std::move(entry));

    if (m_batch.size() >= BATCH_SIZE)
        flush_batch_locked();
}

double ThumbCache::used_mb_locked() {
    // Rzeczywiście zajęte strony (plik bazy nie kurczy się po DELETE)
    QSqlQuery q(m_db);
    qint64 pages = 0, free_pages = 0, page_size = 4096;
    if (q.exec("PRAGMA page_count") && q.next()) pages = q.value(0).toLongLong();
    if (q.exec("PRAGMA freelist_count") && q.next()) free_pages = q.value(0).toLongLong();
    if (q.exec("PRAGMA page_size") && q.next()) page_size = q.value(0).toLongLong();
    return double(pages - free_pages) * double(page_size) / (1024.0 * 1024.0);
}

void ThumbCache::enforce_limit_locked() {
    const int lim = m_limit_mb.load();
    if (lim <= 0) return;                       // 0 = bez limitu
    if (used_mb_locked() <= lim) return;
    // Usuń najdawniej używane wpisy (20% bazy)
    QSqlQuery del(m_db);
    del.exec(R"(DELETE FROM thumbnails WHERE path IN (
        SELECT path FROM thumbnails
        ORDER BY COALESCE(NULLIF(last_used,0), created_at) ASC
        LIMIT (SELECT COUNT(*)/5 FROM thumbnails)))");
}

void ThumbCache::flush_batch_locked() {
    if (m_batch.isEmpty() && m_touched.isEmpty()) return;

    m_db.transaction();
    if (!m_batch.isEmpty()) {
        QSqlQuery q(m_db);
        q.prepare(R"(
            INSERT OR REPLACE INTO thumbnails
                (path, mtime, size, thumb_png, thumb_jpeg, width, height, thumb_size, created_at, last_used)
            VALUES (?, ?, ?, X'', ?, ?, ?, ?, ?, ?)
        )");
        for (const auto& e : m_batch) {
            q.addBindValue(e.path);
            q.addBindValue(e.mtime);
            q.addBindValue(e.size);
            q.addBindValue(e.blob);
            q.addBindValue(e.orig_w);
            q.addBindValue(e.orig_h);
            q.addBindValue(e.thumb_w);
            q.addBindValue(e.now);
            q.addBindValue(e.now);
            q.exec();
        }
        m_batch.clear();
    }
    if (!m_touched.isEmpty()) {
        QSqlQuery u(m_db);
        u.prepare("UPDATE thumbnails SET last_used = ? WHERE path = ?");
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (const QString& p : std::as_const(m_touched)) {
            u.addBindValue(now);
            u.addBindValue(p);
            u.exec();
        }
        m_touched.clear();
    }
    m_db.commit();
    enforce_limit_locked();
}

void ThumbCache::flush() {
    double mb = 0;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_open) return;
        flush_batch_locked();
        mb = used_mb_locked();
    }
    // Rozmiar dla okna Ustawienia — tylko z wątku GUI (QSettings), tylko gdy się zmienił
    if (m_reported_mb < 0 || qAbs(mb - m_reported_mb) > 1.0) {
        m_reported_mb = mb;
        QSettings cfg("Lape", "LapesEye");
        cfg.setValue("cache/current_size_mb", mb);
    }
}

void ThumbCache::remove(const QString& path) {
    QMutexLocker lk(&m_mutex);
    if (!m_open) return;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM thumbnails WHERE path = ?");
    q.addBindValue(norm(path));
    q.exec();
}

void ThumbCache::purge_missing() {
    if (!m_open) return;
    purge_missing_impl();
}

void ThumbCache::purge_missing_impl() {
    // 1. Pobierz listę ścieżek (krótka blokada)
    QStringList all;
    {
        QMutexLocker lock(&m_mutex);
        flush_batch_locked();
        QSqlQuery q(m_db);
        q.exec("SELECT path FROM thumbnails");
        while (q.next()) all << q.value(0).toString();
    }
    // 2. Sprawdź istnienie poza blokadą; pomiń wpisy z odłączonych dysków
    QHash<QString, bool> root_ok;
    QStringList to_delete;
    for (const QString& p : all) {
        const QString root = volume_root(p);
        auto it = root_ok.find(root);
        if (it == root_ok.end())
            it = root_ok.insert(root, QFileInfo::exists(root));
        if (!it.value()) continue;              // dysk niedostępny — zostaw wpisy
        if (!QFileInfo::exists(p)) to_delete << p;
    }
    if (to_delete.isEmpty()) return;
    // 3. Usuń w jednej transakcji
    QMutexLocker lock(&m_mutex);
    m_db.transaction();
    QSqlQuery del(m_db);
    del.prepare("DELETE FROM thumbnails WHERE path = ?");
    for (const QString& p : to_delete) { del.addBindValue(p); del.exec(); }
    m_db.commit();
}

void ThumbCache::purge_older_than(int days) {
    if (!m_open || days <= 0) return;
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - qint64(days) * 86400;
    QMutexLocker lock(&m_mutex);
    flush_batch_locked();
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM thumbnails WHERE COALESCE(NULLIF(last_used,0), created_at) < ?");
    q.addBindValue(cutoff);
    q.exec();
}

void ThumbCache::clear_all_async() {
    if (!m_open) return;
    m_bg.addFuture(QtConcurrent::run([this]() {
        QMutexLocker lock(&m_mutex);
        if (!m_open) return;
        m_batch.clear();
        m_touched.clear();
        QSqlQuery q(m_db);
        q.exec("DELETE FROM thumbnails");
        q.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        q.exec("VACUUM");                       // zwróć miejsce na dysku
    }));
}

void ThumbCache::maintenance_async(bool purge_missing_files, int older_than_days) {
    if (!m_open) return;
    m_bg.addFuture(QtConcurrent::run([this, purge_missing_files, older_than_days]() {
        if (older_than_days > 0) purge_older_than(older_than_days);
        if (purge_missing_files) purge_missing_impl();
    }));
}

void ThumbCache::purge_folder_async(const QString& dir) {
    if (!m_open) return;
    if (m_bg.futures().size() > 64) { m_bg.waitForFinished(); m_bg.clearFutures(); }
    const QString prefix = norm(dir) + "/";
    m_bg.addFuture(QtConcurrent::run([this, prefix]() {
        QStringList candidates;
        {
            QMutexLocker lock(&m_mutex);
            if (!m_open) return;
            QSqlQuery q(m_db);
            q.prepare("SELECT path FROM thumbnails WHERE substr(path, 1, ?) = ?");
            q.addBindValue(int(prefix.size()));
            q.addBindValue(prefix);
            if (q.exec())
                while (q.next()) {
                    const QString p = q.value(0).toString();
                    if (!p.mid(prefix.size()).contains('/'))   // tylko pliki bezpośrednio w folderze
                        candidates << p;
                }
        }
        QStringList gone;
        for (const QString& p : candidates)
            if (!QFileInfo::exists(p)) gone << p;
        if (gone.isEmpty()) return;
        QMutexLocker lock(&m_mutex);
        if (!m_open) return;
        m_db.transaction();
        QSqlQuery del(m_db);
        del.prepare("DELETE FROM thumbnails WHERE path = ?");
        for (const QString& p : gone) { del.addBindValue(p); del.exec(); }
        m_db.commit();
    }));
}

double ThumbCache::db_size_mb() const {
    return QFileInfo(db_path()).size() / (1024.0 * 1024.0);
}

void ThumbCache::rename_entry(const QString& old_path, const QString& new_path) {
    if (!m_open || old_path == new_path) return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("UPDATE thumbnails SET path = ? WHERE path = ?");
    q.addBindValue(norm(new_path));
    q.addBindValue(norm(old_path));
    q.exec();
}

} // namespace LapesEye
