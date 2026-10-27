#pragma once
#include "LapesEye/core/MetaStore.h"
#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QScrollArea>
#include <QEvent>
#include <QStringList>
#include <functional>

namespace LapesEye {

class MetaPanel : public QWidget {
    Q_OBJECT
public:
    explicit MetaPanel(QWidget* parent = nullptr);
    void load(const FileMetadata& meta);
    void set_flag(PickFlag flag);  // programowe ustawienie flagi (skróty Z/X)
    void set_flag_for_path(const QString& path, PickFlag flag);  // Z/X z fullscreen
    // Flaga dla wielu zaznaczonych zdjęć naraz (toggle: jeśli wszystkie już ją mają → zdejmij)
    void set_flag_for_paths(const QStringList& paths, PickFlag flag);
    // Źródło zaznaczenia (MainWindow): przyciski Wybrane/Odrzucone działają na całe zaznaczenie
    void set_selection_provider(std::function<QStringList()> fn) { m_selection_provider = std::move(fn); }
    void set_file_count(int loaded, int total);
    bool is_editing_filename() const;
    QString current_path() const { return m_current_path; }

signals:
    void metadata_saved(const QString& path);
    void rename_requested(const QString& old_path, const QString& new_name);
    void flag_changed(const QString& path);
    void flags_changed(const QStringList& paths);   // zmiana flagi wielu zdjęć naraz
    void return_focus();  // żądanie zwrócenia focusu do siatki

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;  // problem 4

private slots:
    void save();
    void on_rename_committed();

private:
    void setup_ui();
    void apply_flag_to_paths(const QStringList& paths, PickFlag flag);   // bez togglowania
    std::function<QStringList()> m_selection_provider;
    void mark_dirty() { m_save_btn->setEnabled(true); }

    // ── EXIF (read-only) ──────────────────────────────────────────────────────
    QLabel* m_lbl_camera;
    QLabel* m_lbl_lens;
    QLabel* m_lbl_date;
    QLabel* m_lbl_exposure;
    QLabel* m_lbl_iso;
    QLabel* m_lbl_focal;
    QLabel* m_lbl_dims;
    QLabel* m_lbl_colorspace;

    // ── Edytowalne ────────────────────────────────────────────────────────────
    QLineEdit*   m_filename;
    QPushButton* m_btn_pick;
    QPushButton* m_btn_reject;

    QPushButton* m_save_btn;
    QLabel*      m_file_counter = nullptr;
    QString      m_current_path;
    PickFlag     m_current_flag = PickFlag::None;
};

} // namespace LapesEye
