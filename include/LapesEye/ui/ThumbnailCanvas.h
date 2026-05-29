#pragma once
#include <QWidget>
#include <QPixmap>
#include <QSet>
#include <QHash>
#include <QMap>
#include <QLineEdit>
#include <QTimer>
#include <QFileInfo>
#include "LapesEye/core/FileScanner.h"
#include "LapesEye/core/MetaStore.h"
#include "LapesEye/ui/ColorLabelEditor.h"

namespace LapesEye {

// Stałe rysowania — muszą pasować do ThumbnailItem
static constexpr int TC_PAD    = 6;
static constexpr int TC_NAME_H = 16;
static constexpr int TC_STARS_H= 12;

struct ThumbnailCanvasItem {
    ScannedFile file;
    QPixmap     thumb;
    FileMetadata meta;
    bool        meta_loaded = false;
};

class ThumbnailCanvas : public QWidget {
    Q_OBJECT
public:
    explicit ThumbnailCanvas(QWidget* parent = nullptr);

    void set_items(QVector<ThumbnailCanvasItem> items);  // przyjmuje przez wartość (move-friendly)
    void set_thumb_size(int size);
    void set_selected(const QSet<QString>& selected);
    void set_cut_paths(const QSet<QString>& cut);
    void set_drag_active(bool active);
    void set_pixmap(const QString& path, const QPixmap& pix);
    void set_metadata(const QString& path, const FileMetadata& meta);
    void rename_item(const QString& old_path, const QString& new_path, const QString& new_name);
    void rename_pixmap_in_store(const QString& old_path, const QString& new_path);
    void clear_pixmap_store() { m_pixmap_store.clear(); }

    // Dostęp do danych (do zachowania pixmap przy sync)
    const QVector<ThumbnailCanvasItem>& items() const { return m_items; }

    // Geometria
    int  cols() const;
    int  cell_size() const;
    int  cell_width() const;
    int  total_height() const;
    int  gap() const { return 4; }
    void notify_scroll_start() {  // wywołaj przy scrollu — Fast transformation
        m_is_scrolling = true;
        if (m_scroll_end_timer) m_scroll_end_timer->start();
    }
    QRect item_rect(int idx) const;
    int   index_at(const QPoint& pos) const;  // -1 jeśli poza

    // Rename overlay
    void start_rename(int idx);
    void cancel_rename();
    bool is_renaming() const { return m_rename_idx >= 0; }
    QLineEdit* rename_edit() const { return m_rename_edit; }

signals:
    void clicked(const QString& path, Qt::KeyboardModifiers mods);
    void press_in_empty(const QPoint& global_pos);
    void rubber_move(const QPoint& global_pos);
    void double_clicked(const QString& path);
    void context_menu_requested(const QString& path, const QPoint& global_pos);
    void drag_move(const QPoint& press_global, const QPoint& cur_global);
    void drag_released(const QPoint& global_pos);
    void rename_requested(const QString& old_path, const QString& new_name);
    void hovered(const QString& path);  // emitowany gdy kursor wchodzi na kafelek

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void draw_item(QPainter& p, int idx, const QRect& r);
    void draw_stars(QPainter& p, const QRect& r, int rating);
    void draw_badge(QPainter& p, const QRect& img_rect, const QString& text, const QColor& color);

    QVector<ThumbnailCanvasItem> m_items;
    QHash<QString, QPixmap> m_pixmap_store;  // trwały magazyn — nie czyszczony przy set_items
    QSet<QString>  m_selected;
    QSet<QString>  m_cut;
    int            m_thumb_size = 160;
    bool           m_drag_active = false;
    int            m_hovered_idx = -1;
    QPoint         m_press_global;
    int            m_press_idx = -1;

    // Rename
    QLineEdit* m_rename_edit      = nullptr;
    int        m_rename_idx       = -1;
    bool       m_rename_committed = false;
    bool       m_is_scrolling     = false;  // Fast vs Smooth transformation
    QTimer*    m_scroll_end_timer = nullptr;
};

} // namespace LapesEye
