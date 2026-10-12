#pragma once
#include <QVector>
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QSet>
#include <QSqlDatabase>
#include <QMutex>
#include <QFutureSynchronizer>
#include <atomic>
#include <optional>

namespace LapesEye {

class ThumbCache {
public:
    static constexpr int THUMB_SIZE = 256;

    explicit ThumbCache();
    ~ThumbCache();

    bool open();
    void close();

    // Zwraca QImage jeśli w cache i aktualna (data + rozmiar pliku zgodne)
    std::optional<QImage> get(const QString& path);
    void flush();  // zapisz zakolejkowane wpisy na dysk (wywoływać z wątku GUI)

    // Zapisuje QImage do cache (jako JPEG blob)
    void put(const QString& path, const QImage& thumb, int orig_w, int orig_h);

    void   remove(const QString& path);   // usuń wpis z cache (np. po obrocie)
    double db_size_mb() const;
    // Przenosi wpis w cache pod nową ścieżkę (po rename pliku)
    void   rename_entry(const QString& old_path, const QString& new_path);

    // Limit rozmiaru bazy w MB; 0 = bez limitu
    void   set_limit_mb(int mb) { m_limit_mb = mb; }

    // Usuwa wpisy plików, których już nie ma (pomija odłączone dyski)
    void   purge_missing();
    // Usuwa wpisy nieużywane od `days` dni
    void   purge_older_than(int days);
    // Usuwa WSZYSTKIE wpisy (w tle) i zmniejsza plik bazy
    void   clear_all_async();
    // Zadania w tle (czyszczenie przy starcie)
    void   maintenance_async(bool purge_missing_files, int older_than_days);
    // Po wczytaniu folderu: usuń z cache wpisy plików, które zniknęły z tego folderu
    void   purge_folder_async(const QString& dir);

    // Ujednolicenie ścieżki (separatory, litera dysku na Windows)
    static QString norm(const QString& path);

private:
    bool    create_schema();
    QString db_path() const;
    double  used_mb_locked();
    void    enforce_limit_locked();
    void    flush_batch_locked();
    void    purge_missing_impl();
    static QString volume_root(const QString& path);

    QSqlDatabase m_db;
    QString      m_conn_name;
    bool         m_open = false;
    mutable QMutex m_mutex;  // chroni QSqlDatabase przed wielowątkowym dostępem
    static constexpr int BATCH_SIZE = 32;
    struct BatchEntry { QString path; qint64 mtime, size, now; QByteArray blob; int orig_w, orig_h, thumb_w; };
    QVector<BatchEntry> m_batch;
    QSet<QString>       m_touched;          // wpisy odczytane — aktualizacja last_used
    std::atomic<int>    m_limit_mb{2048};
    double              m_reported_mb = -1; // ostatnio zapisany rozmiar do ustawień
    QFutureSynchronizer<void> m_bg;         // zadania w tle (czekamy przy close)
};

} // namespace LapesEye
