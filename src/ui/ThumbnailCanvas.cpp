#include "LapesEye/ui/ThumbnailCanvas.h"
#include "LapesEye/core/PerfTimer.h"
#include "LapesEye/ui/ColorLabelEditor.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QFontMetrics>
#include <QApplication>
#include <QDragEnterEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QGuiApplication>

namespace LapesEye {

static QColor tc_label_color(ColorLabel c) {
    if (c == ColorLabel::None) return Qt::transparent;
    int idx = static_cast<int>(c) - 1;
    auto labels = LabelConfig::load();
    if (idx >= 0 && idx < labels.size()) return labels[idx].color;
    return Qt::transparent;
}

// ─── Stałe layoutu ────────────────────────────────────────────────────────────
// Odstęp między kafelkami
static constexpr int CELL_GAP  = 4;
// Padding poziomy (boki)
static constexpr int CELL_PAD  = 4;
// Padding pionowy (góra/dół) — mniejszy żeby zdjęcia poziome nie traciły miejsca
static constexpr int CELL_VPAD = 2;
// Wysokość paska z nazwą pliku
static constexpr int NAME_H    = 15;
// Wysokość paska z gwiazdkami
static constexpr int STARS_H   = 12;

ThumbnailCanvas::ThumbnailCanvas(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    // Timer: po 150ms bez scrollu przełącz na SmoothTransformation i odrysuj
    m_scroll_end_timer = new QTimer(this);
    m_scroll_end_timer->setSingleShot(true);
    m_scroll_end_timer->setInterval(150);
    connect(m_scroll_end_timer, &QTimer::timeout, this, [this]() {
        m_is_scrolling = false;
        update();  // przerysuj z Smooth po zatrzymaniu scrollu
    });
}

void ThumbnailCanvas::set_items(QVector<ThumbnailCanvasItem> items) {
    PERF_SCOPE("set_items");
    m_items = std::move(items);
    // Uzupełnij pixmapy z trwałego magazynu
    for (auto& ci : m_items) {
        if (ci.thumb.isNull()) {
            auto it = m_pixmap_store.find(ci.file.path);
            if (it != m_pixmap_store.end())
                ci.thumb = it.value();
        }
    }
    m_hovered_idx = -1;
    int h = total_height();
    setFixedHeight(qMax(1, h));
    update();
}

void ThumbnailCanvas::set_thumb_size(int size) {
    if (m_thumb_size == size) return;
    m_thumb_size = size;
    setFixedHeight(qMax(1, total_height()));
    update();
}

void ThumbnailCanvas::set_selected(const QSet<QString>& selected) {
    m_selected = selected;
    update();
}

void ThumbnailCanvas::set_cut_paths(const QSet<QString>& cut) {
    m_cut = cut;
    update();
}

void ThumbnailCanvas::set_drag_active(bool active) {
    m_drag_active = active;
    update();
}

void ThumbnailCanvas::set_pixmap(const QString& path, const QPixmap& pix) {
    // Zawsze zapisuj do trwałego magazynu — przeżyje set_items()
    if (!pix.isNull())
        m_pixmap_store[path] = pix;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].file.path == path) {
            m_items[i].thumb = pix;
            update();
            return;
        }
    }
}

void ThumbnailCanvas::set_metadata(const QString& path, const FileMetadata& meta) {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].file.path == path) {
            m_items[i].meta = meta;
            m_items[i].meta_loaded = true;
            update(item_rect(i));
            return;
        }
    }
}

void ThumbnailCanvas::rename_pixmap_in_store(const QString& old_path, const QString& new_path) {
    auto it = m_pixmap_store.find(old_path);
    if (it != m_pixmap_store.end()) {
        QPixmap pix = it.value();
        m_pixmap_store.erase(it);
        m_pixmap_store[new_path] = pix;
    }
}

void ThumbnailCanvas::rename_item(const QString& old_path, const QString& new_path,
                                   const QString& new_name) {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].file.path == old_path) {
            m_items[i].file.path = new_path;
            m_items[i].file.name = new_name;
            update(item_rect(i));
            return;
        }
    }
}

// ─── Geometria ────────────────────────────────────────────────────────────────

int ThumbnailCanvas::cols() const {
    int w = width();
    if (w < 10) return 1;
    // Rozmiar kafelka = thumb + padding po obu stronach + gap
    int tile = m_thumb_size + 2*CELL_PAD;
    int c = qMax(1, w / (tile + CELL_GAP));
    return c;
}

int ThumbnailCanvas::cell_size() const {
    return m_thumb_size + 2*CELL_VPAD + NAME_H + STARS_H;
}

int ThumbnailCanvas::total_height() const {
    if (m_items.isEmpty()) return 0;
    int c = cols();
    int n_rows = (m_items.size() + c - 1) / c;
    // Wiersze + odstępy między wierszami
    return n_rows * cell_size() + (n_rows + 1) * CELL_GAP;
}

// Szerokość kafelka — równo rozłożone z odstępami
int ThumbnailCanvas::cell_width() const {
    int c = cols();
    if (c <= 0) return m_thumb_size + 2*CELL_PAD;
    // Dostępna szerokość minus odstępy po obu stronach i między kafelkami
    int total_gap = (c + 1) * CELL_GAP;
    return (width() - total_gap) / c;
}

QRect ThumbnailCanvas::item_rect(int idx) const {
    if (idx < 0 || idx >= m_items.size()) return {};
    int c    = cols();
    int col  = idx % c;
    int row  = idx / c;
    int cw   = cell_width();
    int ch   = cell_size();
    int x    = CELL_GAP + col * (cw + CELL_GAP);
    int y    = CELL_GAP + row * (ch + CELL_GAP);
    return QRect(x, y, cw, ch);
}

int ThumbnailCanvas::index_at(const QPoint& pos) const {
    int c  = cols();
    int cw = cell_width();
    int ch = cell_size();
    if (c <= 0 || cw <= 0 || ch <= 0) return -1;
    // Uwzględnij CELL_GAP — kliknięcie w gap nie trafi w żaden item
    int col = (pos.x() - CELL_GAP) / (cw + CELL_GAP);
    int row = (pos.y() - CELL_GAP) / (ch + CELL_GAP);
    if (col < 0 || col >= c) return -1;
    if (row < 0) return -1;
    // Sprawdź czy klik jest wewnątrz kafelka (nie w gap)
    int tile_x = CELL_GAP + col * (cw + CELL_GAP);
    int tile_y = CELL_GAP + row * (ch + CELL_GAP);
    if (pos.x() < tile_x || pos.x() >= tile_x + cw) return -1;
    if (pos.y() < tile_y || pos.y() >= tile_y + ch) return -1;
    int idx = row * c + col;
    if (idx < 0 || idx >= m_items.size()) return -1;
    return idx;
}

// ─── Rysowanie ────────────────────────────────────────────────────────────────

void ThumbnailCanvas::paintEvent(QPaintEvent* e) {
    PERF_SCOPE("paintEvent_canvas");
    QPainter p(this);
    QRect clip = e->rect();
    p.fillRect(clip, QColor(0x1e, 0x1e, 0x1e));
    p.setRenderHint(QPainter::Antialiasing);
    // SmoothPixmapTransform ustawiane per-kafelek (off podczas scrollu)

    int ch = cell_size();
    int c  = cols();
    if (c == 0 || ch == 0) return;

    int first_row = qMax(0, (clip.top() - CELL_GAP) / (ch + CELL_GAP));
    int last_row  = (clip.bottom() - CELL_GAP) / (ch + CELL_GAP);
    int n_rows = (m_items.size() + c - 1) / c;
    last_row = qMin(last_row, n_rows - 1);

    for (int row = first_row; row <= last_row; ++row) {
        for (int col = 0; col < c; ++col) {
            int idx = row * c + col;
            if (idx >= m_items.size()) break;
            draw_item(p, idx, item_rect(idx));
        }
    }
}

void ThumbnailCanvas::draw_item(QPainter& p, int idx, const QRect& r) {
    const auto& item = m_items[idx];
    bool selected = m_selected.contains(item.file.path) && !m_drag_active;
    bool hovered  = (idx == m_hovered_idx);
    bool cut      = m_cut.contains(item.file.path);

    // ── Tło kafelka ───────────────────────────────────────────────────────────
    QColor bg;
    if (selected) {
        bg = QColor(0x2D, 0x7D, 0xD2);
    } else if (item.meta_loaded && item.meta.color_label != ColorLabel::None) {
        QColor lc = tc_label_color(item.meta.color_label);
        bg = QColor(lc.red()/4, lc.green()/4, lc.blue()/4);
        if (hovered) bg = bg.lighter(140);
    } else {
        bg = hovered ? QColor(0x3A, 0x3A, 0x3A) : QColor(0x28, 0x28, 0x28);
    }
    QPainterPath bgPath;
    bgPath.addRoundedRect(r, 6, 6);
    p.fillPath(bgPath, bg);

    // ── Miniatura — wypełnia cały kafelek zachowując proporcje (crop) ─────────
    int avail_w = r.width()  - 2*CELL_PAD;
    int avail_h = r.height() - 2*CELL_VPAD - NAME_H - STARS_H - 3;
    int img_x   = r.x() + CELL_PAD;
    int img_y   = r.y() + CELL_VPAD;
    QRect img_rect(img_x, img_y, avail_w, avail_h);

    if (item.file.is_dir) {
        p.fillRect(img_rect, QColor(0x22, 0x22, 0x22));
        QFont f = p.font();
        f.setPixelSize(qMin(qMin(avail_w, avail_h) * 2/3, 72));
        p.setFont(f);
        p.setPen(QColor(0xF0, 0xC0, 0x20));
        p.drawText(img_rect, Qt::AlignCenter, "📁");
    } else if (item.thumb.isNull()) {
        p.fillRect(img_rect, QColor(0x22, 0x22, 0x22));
        p.setPen(QColor(0x44, 0x44, 0x44));
        QFont f = p.font(); f.setPixelSize(11); p.setFont(f);
        p.drawText(img_rect, Qt::AlignCenter, "⏳");
    } else {
        QSize scaled = item.thumb.size().scaled(img_rect.size(), Qt::KeepAspectRatio);
        int ox = img_rect.x() + (img_rect.width()  - scaled.width())  / 2;
        int oy = img_rect.y() + (img_rect.height() - scaled.height()) / 2;
        p.setRenderHint(QPainter::SmoothPixmapTransform, !m_is_scrolling);
        p.drawPixmap(QRect(ox, oy, scaled.width(), scaled.height()), item.thumb);
    }

    // ── Cut overlay ───────────────────────────────────────────────────────────
    if (cut) p.fillPath(bgPath, QColor(0, 0, 0, 100));

    // ── Color label ramka ─────────────────────────────────────────────────────
    if (item.meta_loaded && item.meta.color_label != ColorLabel::None) {
        QColor lc = tc_label_color(item.meta.color_label);
        p.setPen(QPen(lc, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(img_rect).adjusted(1,1,-1,-1), 4, 4);
    }

    // ── Badges RAW/PSD ────────────────────────────────────────────────────────
    if (item.file.is_raw)      draw_badge(p, img_rect, "RAW", QColor(0xE5, 0x89, 0x20));
    else if (item.file.is_psd) draw_badge(p, img_rect, "PSD", QColor(0x20, 0x6E, 0xE5));

    // ── Nazwa pliku ───────────────────────────────────────────────────────────
    if (m_rename_idx != idx) {
        int name_y = img_rect.bottom() + 3;
        QRect name_rect(r.x() + CELL_PAD, name_y, r.width() - 2*CELL_PAD, NAME_H);
        p.setPen(selected ? Qt::white : QColor(0xCC, 0xCC, 0xCC));
        QFont fn = p.font(); fn.setPixelSize(11); p.setFont(fn);
        QFontMetrics fm(fn);
        p.drawText(name_rect, Qt::AlignHCenter | Qt::AlignVCenter,
                   fm.elidedText(item.file.name, Qt::ElideMiddle, name_rect.width()));

        // ── Gwiazdki ──────────────────────────────────────────────────────────
        if (item.meta_loaded && item.meta.rating > 0 && !item.file.is_dir) {
            QRect star_rect(r.x() + CELL_PAD, name_rect.bottom() + 1,
                            r.width() - 2*CELL_PAD, STARS_H);
            draw_stars(p, star_rect, item.meta.rating);
        }
    }
}

void ThumbnailCanvas::draw_stars(QPainter& p, const QRect& r, int rating) {
    p.setPen(QColor(0xFF, 0xD7, 0x00));
    QFont f = p.font(); f.setPixelSize(9); p.setFont(f);
    QString stars;
    for (int i = 0; i < 5; ++i) stars += (i < rating) ? "★" : "☆";
    p.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, stars);
}

void ThumbnailCanvas::draw_badge(QPainter& p, const QRect& img_rect,
                                  const QString& text, const QColor& color) {
    QFont f = p.font(); f.setPixelSize(9); f.setBold(true); p.setFont(f);
    QFontMetrics fm(f);
    int bw = fm.horizontalAdvance(text) + 4, bh = 13;
    QRect br(img_rect.right() - bw - 2, img_rect.top() + 2, bw, bh);
    p.fillRect(br, color);
    p.setPen(Qt::white);
    p.drawText(br, Qt::AlignCenter, text);
}

// ─── Mouse events ─────────────────────────────────────────────────────────────

void ThumbnailCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_press_global = e->globalPosition().toPoint();
        m_press_idx    = index_at(e->pos());
        if (m_press_idx >= 0) {
            emit clicked(m_items[m_press_idx].file.path, e->modifiers());
        } else {
            emit clicked("", e->modifiers());
            // Pozwól na rubber band — emituj pozycję żeby ThumbnailGrid mógł ustawić origin
            emit press_in_empty(e->globalPosition().toPoint());
        }
    }
}

void ThumbnailCanvas::mouseMoveEvent(QMouseEvent* e) {
    int new_hovered = index_at(e->pos());
    if (new_hovered != m_hovered_idx) {
        int old = m_hovered_idx;
        m_hovered_idx = new_hovered;
        if (old >= 0) update(item_rect(old));
        if (new_hovered >= 0) {
            update(item_rect(new_hovered));
            // Prefetch — emituj ścieżkę pod kursorem
            emit hovered(m_items[new_hovered].file.path);
        }
    }
    if (e->buttons() & Qt::LeftButton) {
        if (m_press_idx >= 0)
            emit drag_move(m_press_global, e->globalPosition().toPoint());
        else
            emit rubber_move(e->globalPosition().toPoint());
    }
}

void ThumbnailCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        emit drag_released(e->globalPosition().toPoint());
        m_press_idx = -1;
    }
}

void ThumbnailCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    int idx = index_at(e->pos());
    if (idx < 0) return;
    // Klik w obszar nazwy = rename, klik w miniaturę = open
    QRect r  = item_rect(idx);
    int avail = r.width() - 2*CELL_PAD;
    int ts    = qMin(m_thumb_size, avail);
    int img_y = r.y() + CELL_PAD;
    QRect img_rect(r.x() + (r.width() - ts)/2, img_y, ts, ts);
    int name_y = img_rect.bottom() + 3;
    QRect name_rect(r.x() + CELL_PAD, name_y, r.width() - 2*CELL_PAD, NAME_H);

    if (name_rect.contains(e->pos()))
        start_rename(idx);
    else
        emit double_clicked(m_items[idx].file.path);
}

void ThumbnailCanvas::contextMenuEvent(QContextMenuEvent* e) {
    int idx = index_at(e->pos());
    if (idx >= 0)
        emit context_menu_requested(m_items[idx].file.path, e->globalPos());
}

void ThumbnailCanvas::leaveEvent(QEvent*) {
    if (m_hovered_idx >= 0) {
        update(item_rect(m_hovered_idx));
        m_hovered_idx = -1;
    }
}

// Drag eventy — canvas je ignoruje, QScrollArea propaguje do ThumbnailGrid
void ThumbnailCanvas::dragEnterEvent(QDragEnterEvent* e) {
    e->ignore();
}

void ThumbnailCanvas::dragMoveEvent(QDragMoveEvent* e) {
    e->ignore();
}

void ThumbnailCanvas::dropEvent(QDropEvent* e) {
    e->ignore();
}

// ─── Rename ───────────────────────────────────────────────────────────────────

void ThumbnailCanvas::start_rename(int idx) {
    if (idx < 0 || idx >= m_items.size()) return;
    cancel_rename();
    m_rename_idx = idx;
    QRect r     = item_rect(idx);
    int avail_h = r.height() - 2*CELL_VPAD - NAME_H - STARS_H - 3;
    int name_y  = r.y() + CELL_VPAD + avail_h + 3;
    // Koordynaty w canvas → przelicz do viewport (rodzic canvas)
    QWidget* vp = parentWidget();  // viewport
    QRect name_rect_canvas(r.x() + CELL_PAD, name_y, r.width() - 2*CELL_PAD, NAME_H);
    QRect name_rect_vp = name_rect_canvas;
    if (vp) {
        QPoint offset = mapTo(vp, QPoint(0, 0));
        name_rect_vp = name_rect_canvas.translated(offset);
    }
    // QLineEdit jako dziecko viewport — scrolluje się razem z canvas
    m_rename_edit = new QLineEdit(vp ? vp : this);
    m_rename_edit->setGeometry(name_rect_vp);
    m_rename_edit->setAlignment(Qt::AlignHCenter);
    QString full_name = m_items[idx].file.name;
    QString display   = m_items[idx].file.is_dir
                        ? full_name
                        : QFileInfo(full_name).completeBaseName();  // bez rozszerzenia
    m_rename_edit->setText(full_name);  // pełna nazwa w polu
    // Zaznacz tylko część bez rozszerzenia (wygodniejsze dla usera)
    if (!m_items[idx].file.is_dir) {
        int sel_len = display.length();
        m_rename_edit->setSelection(0, sel_len);
    } else {
        m_rename_edit->selectAll();
    }
    m_rename_edit->show();
    m_rename_edit->raise();
    m_rename_edit->setFocus();
    m_rename_edit->installEventFilter(this);
    // Nie podłączamy editingFinished — canvas eventFilter obsługuje Enter i focus-loss
    // przez focusOutEvent na QLineEdit
    update(r);
}

bool ThumbnailCanvas::eventFilter(QObject* obj, QEvent* event) {
    if (obj != m_rename_edit) return QWidget::eventFilter(obj, event);

    // FocusOut — zatwierdź rename TYLKO gdy Enter nie obsłużył już (m_rename_committed=false)
    if (event->type() == QEvent::FocusOut) {
        if (!m_rename_committed && m_rename_idx >= 0 && m_rename_idx < m_items.size() && m_rename_edit) {
            QString new_name = m_rename_edit->text().trimmed();
            QString old_path = m_items[m_rename_idx].file.path;
            // Dodaj rozszerzenie jeśli brak
            if (!m_items[m_rename_idx].file.is_dir && !new_name.isEmpty()) {
                QString orig_ext = QFileInfo(old_path).suffix();
                QString new_ext  = QFileInfo(new_name).suffix();
                if (!orig_ext.isEmpty() &&
                    new_ext.compare(orig_ext, Qt::CaseInsensitive) != 0)
                    new_name += "." + orig_ext;
            }
            cancel_rename();
            if (!new_name.isEmpty()) {
                QString orig = QFileInfo(old_path).fileName();
                if (new_name != orig)
                    emit rename_requested(old_path, new_name);
            }
        }
        m_rename_committed = false;
        return false;
    }

    if (event->type() != QEvent::KeyPress) return QWidget::eventFilter(obj, event);

    auto* ke = static_cast<QKeyEvent*>(event);

    if (ke->key() == Qt::Key_Escape) {
        m_rename_committed = true;  // blokuj FocusOut który nastąpi po cancel
        cancel_rename();
        return true;
    }

    if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
        if (m_rename_idx >= 0 && m_rename_idx < m_items.size() && m_rename_edit) {
            QString new_name = m_rename_edit->text().trimmed();
            QString old_path = m_items[m_rename_idx].file.path;

            // Automatycznie dodaj oryginalne rozszerzenie jeśli user go nie wpisał
            if (!m_items[m_rename_idx].file.is_dir && !new_name.isEmpty()) {
                QString orig_ext = QFileInfo(old_path).suffix();
                QString new_ext  = QFileInfo(new_name).suffix();
                if (!orig_ext.isEmpty() &&
                    new_ext.compare(orig_ext, Qt::CaseInsensitive) != 0)
                    new_name += "." + orig_ext;
            }

            m_rename_committed = true;
            cancel_rename();
            if (!new_name.isEmpty()) {
                QString orig = QFileInfo(old_path).fileName();
                if (new_name != orig)
                    emit rename_requested(old_path, new_name);
            }
        }
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

void ThumbnailCanvas::cancel_rename() {
    if (m_rename_edit) {
        m_rename_edit->deleteLater();
        m_rename_edit = nullptr;
    }
    int old = m_rename_idx;
    m_rename_idx = -1;
    if (old >= 0) update(item_rect(old));
}

} // namespace LapesEye
