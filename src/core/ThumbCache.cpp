#include "LapesEye/core/ThumbCache.h"
#include <QPixmapCache>
#include <QSettings>
#include <QMutexLocker>

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
    if (m_open) {
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
    // Nowa kolumna: thumb_data (JPEG) — stara thumb_png dla kompatybilności
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
    // Migracje — dodaj kolumny jeśli nie istnieją
    q.exec("ALTER TABLE thumbnails ADD COLUMN thumb_jpeg BLOB");
    q.exec("ALTER TABLE thumbnails ADD COLUMN thumb_size INTEGER NOT NULL DEFAULT 0");
    return ok;
}

std::optional<QImage> ThumbCache::get(const QString& path) {
    if (!m_open) return std::nullopt;
    QMutexLocker lock(&m_mutex);

    // Sprawdź QFileInfo bez exists() — unikamy I/O dla nieistniejących plików
    // przez sprawdzenie mtime w SQLite najpierw
    QSqlQuery q(m_db);
    q.prepare("SELECT thumb_jpeg, thumb_png, mtime, size FROM thumbnails WHERE path = ?");
    q.addBindValue(path);
    if (!q.exec() || !q.next()) return std::nullopt;

    // Porównaj mtime/size z dyskiem dopiero gdy wpis istnieje
    QFileInfo fi(path);
    qint64 mtime = fi.lastModified().toSecsSinceEpoch();
    qint64 size  = fi.size();
    if (q.value(2).toLongLong() != mtime || q.value(3).toLongLong() != size)
        return std::nullopt;

    // Preferuj JPEG (szybszy odczyt), fallback do PNG (stare wpisy)
    QByteArray blob = q.value(0).toByteArray();
    const char* fmt = "JPEG";
    if (blob.isEmpty()) {
        blob = q.value(1).toByteArray();
        fmt = "PNG";
    }
    QImage img;
    if (!img.loadFromData(blob, fmt) || img.isNull()) return std::nullopt;
    return img;
}

void ThumbCache::put(const QString& path, const QImage& thumb,
                     int orig_w, int orig_h)
{
    if (!m_open || thumb.isNull()) return;
    QMutexLocker lock(&m_mutex);

    // Sprawdź limit rozmiaru bazy (z QSettings)
    QSettings cfg("Lape", "LapesEye");
    int limit_mb = cfg.value("cache/disk_mb", 2048).toInt();
    double current_mb = db_size_mb();
    if (current_mb > limit_mb) {
        // Usuń najstarsze wpisy (20% bazy) żeby zrobić miejsce
        QSqlQuery del(m_db);
        del.exec(R"(DELETE FROM thumbnails WHERE path IN (
            SELECT path FROM thumbnails ORDER BY created_at ASC LIMIT
            (SELECT COUNT(*)/5 FROM thumbnails)))");
    }
    // Zapisz bieżący rozmiar do settings (dla SettingsDialog)
    cfg.setValue("cache/current_size_mb", db_size_mb());

    QFileInfo fi(path);
    qint64 mtime = fi.lastModified().toSecsSinceEpoch();
    qint64 size  = fi.size();
    qint64 now   = QDateTime::currentSecsSinceEpoch();

    // JPEG quality 85 — 4× mniejszy od PNG, 10× szybszy odczyt
    QByteArray blob;
    QBuffer buf(&blob);
    buf.open(QIODevice::WriteOnly);
    thumb.save(&buf, "JPEG", 85);

    // Dodaj do kolejki batch — nie zapisuj od razu żeby uniknąć setek
    // osobnych transakcji przy ładowaniu dużego folderu
    BatchEntry entry;
    entry.path      = path;
    entry.mtime     = mtime;
    entry.size      = size;
    entry.blob      = blob;
    entry.orig_w    = orig_w;
    entry.orig_h    = orig_h;
    entry.thumb_w   = thumb.width();
    entry.now       = now;
    m_batch.append(std::move(entry));

    // Flush co BATCH_SIZE wpisów lub od razu jeśli batch jest duży
    if (m_batch.size() >= BATCH_SIZE)
        flush_batch_locked();
}

void ThumbCache::flush_batch_locked() {
    if (m_batch.isEmpty()) return;

    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(R"(
        INSERT OR REPLACE INTO thumbnails
            (path, mtime, size, thumb_png, thumb_jpeg, width, height, thumb_size, created_at)
        VALUES (?, ?, ?, X'', ?, ?, ?, ?, ?)
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
        q.exec();
    }
    m_db.commit();
    m_batch.clear();
}

void ThumbCache::flush() {
    QMutexLocker lock(&m_mutex);
    flush_batch_locked();
}

void ThumbCache::remove(const QString& path) {
    QMutexLocker lk(&m_mutex);
    if (!m_open) return;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM thumbnails WHERE path = ?");
    q.addBindValue(path);
    q.exec();
}

void ThumbCache::purge_missing() {
    if (!m_open) return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery q(m_db);
    q.exec("SELECT path FROM thumbnails");
    QStringList to_delete;
    while (q.next()) {
        if (!QFileInfo::exists(q.value(0).toString()))
            to_delete << q.value(0).toString();
    }
    for (const auto& p : to_delete) {
        QSqlQuery del(m_db);
        del.prepare("DELETE FROM thumbnails WHERE path = ?");
        del.addBindValue(p);
        del.exec();
    }
}

double ThumbCache::db_size_mb() const {
    return QFileInfo(db_path()).size() / (1024.0 * 1024.0);
}

void ThumbCache::rename_entry(const QString& old_path, const QString& new_path) {
    if (!m_open || old_path == new_path) return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("UPDATE thumbnails SET path = ? WHERE path = ?");
    q.addBindValue(new_path);
    q.addBindValue(old_path);
    q.exec();
}

} // namespace LapesEye
