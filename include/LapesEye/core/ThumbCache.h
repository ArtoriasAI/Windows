#pragma once
#include <QVector>
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QSqlDatabase>
#include <QMutex>
#include <optional>

namespace LapesEye {

class ThumbCache {
public:
    static constexpr int THUMB_SIZE = 256;

    explicit ThumbCache();
    ~ThumbCache();

    bool open();
    void close();

    // Zwraca QImage jeśli w cache i aktualna
    std::optional<QImage> get(const QString& path);
    void flush();  // zapisz zakolejkowane wpisy na dysk

    // Zapisuje QImage do cache (jako PNG blob)
    void put(const QString& path, const QImage& thumb, int orig_w, int orig_h);

    void   remove(const QString& path);   // usuń wpis z cache (np. po obrocie)
    void   purge_missing();
    double db_size_mb() const;
    // Przenosi wpis w cache pod nową ścieżkę (po rename pliku)
    void   rename_entry(const QString& old_path, const QString& new_path);

private:
    bool    create_schema();
    QString db_path() const;

    QSqlDatabase m_db;
    QString      m_conn_name;
    bool         m_open = false;
    mutable QMutex m_mutex;  // chroni QSqlDatabase przed wielowątkowym dostępem
    static constexpr int BATCH_SIZE = 32;
    struct BatchEntry { QString path; qint64 mtime, size, now; QByteArray blob; int orig_w, orig_h, thumb_w; };
    QVector<BatchEntry> m_batch;
    void flush_batch_locked();
};

} // namespace LapesEye
