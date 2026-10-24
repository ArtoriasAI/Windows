#include "LapesEye/ui/SettingsDialog.h"
#include "LapesEye/core/ThumbCache.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSettings>
#include <QPixmapCache>
#include <QLabel>
#include <QSlider>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QColorDialog>

namespace LapesEye {

namespace {
// Photoshop pyta o zgodę przy każdym skrypcie uruchamianym z pliku (-r).
// Parametr "WarnRunningScripts 0" w PSUserConfig.txt wyłącza to pytanie.
// Zwraca: 1 = dodano, 0 = już było, -1 = nie znaleziono folderu ustawień
int leye_disable_ps_script_warning(const QString& exe_path) {
#ifdef Q_OS_WIN
    const QString appdata = qEnvironmentVariable("APPDATA");   // ...\\AppData\\Roaming
    if (appdata.isEmpty()) return -1;
    QDir roaming(appdata);
    QDir adobe(roaming.absoluteFilePath("Adobe"));
    if (!adobe.exists()) return -1;

    // Folder wersji z ścieżki exe: ...\Adobe Photoshop 2025\Photoshop.exe
    QStringList versions;
    const QString ver = QFileInfo(exe_path).dir().dirName();
    if (ver.startsWith("Adobe Photoshop")) versions << ver;
    if (versions.isEmpty())
        versions = adobe.entryList({"Adobe Photoshop*"}, QDir::Dirs | QDir::NoDotAndDotDot);

    int result = -1;
    for (const QString& v : versions) {
        QDir settings(adobe.absoluteFilePath(v + "/" + v + " Settings"));
        if (!settings.exists()) continue;
        const QString cfg = settings.absoluteFilePath("PSUserConfig.txt");
        QFile f(cfg);
        QString text;
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            text = QString::fromUtf8(f.readAll());
            f.close();
        }
        bool has = false, zero = false;
        for (const QString& line : text.split('\n')) {
            const QStringList w = line.simplified().split(' ');
            if (w.size() >= 2 && w[0].compare("WarnRunningScripts", Qt::CaseInsensitive) == 0) {
                has = true;
                if (w[1] == "0") zero = true;
            }
        }
        if (has) { if (zero && result < 1) result = 0; continue; }  // nie nadpisujemy cudzej wartości
        if (!f.open(QIODevice::Append | QIODevice::Text)) continue;
        QTextStream out(&f);
        if (!text.isEmpty() && !text.endsWith('\n')) out << "\n";
        out << "WarnRunningScripts 0\n";
        f.close();
        result = 1;
    }
    return result;
#else
    Q_UNUSED(exe_path);
    return -1;
#endif
}

void leye_maybe_fix_ps_warning(QWidget* parent, const QString& exe_path) {
#ifdef Q_OS_WIN
    if (!QFileInfo(exe_path).fileName().contains("photoshop", Qt::CaseInsensitive)) return;
    auto btn = QMessageBox::question(parent, "Photoshop",
        "Photoshop pyta o zgodę przy każdym skrypcie, którego Lape's Eye używa do "
        "otwierania zdjęć (klawisz E).\n\n"
        "Czy dopisać \"WarnRunningScripts 0\" do pliku PSUserConfig.txt Photoshopa, "
        "żeby to pytanie się nie pojawiało?\n"
        "(Plik zostanie utworzony, jeśli go nie ma. Wymaga ponownego uruchomienia Photoshopa.)",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (btn != QMessageBox::Yes) return;
    const int r = leye_disable_ps_script_warning(exe_path);
    if (r == 1)
        QMessageBox::information(parent, "Photoshop",
            "Gotowe. Uruchom Photoshopa ponownie, żeby zmiana zadziałała.");
    else if (r == 0)
        QMessageBox::information(parent, "Photoshop", "Ostrzeżenie jest już wyłączone.");
    else
        QMessageBox::warning(parent, "Photoshop",
            "Nie znaleziono folderu ustawień Photoshopa (uruchom go choć raz) "
            "albo plik ma już własną wartość WarnRunningScripts.");
#else
    Q_UNUSED(parent); Q_UNUSED(exe_path);
#endif
}
} // namespace


// ─── Statyczne odczyty ustawień ───────────────────────────────────────────────

int SettingsDialog::ram_cache_mb() {
    QSettings s("Lape", "LapesEye");
    return s.value("cache/ram_mb", 4096).toInt();
}
int SettingsDialog::disk_cache_mb() {
    QSettings s("Lape", "LapesEye");
    return s.value("cache/disk_mb", 2048).toInt();
}
int SettingsDialog::cache_clear_days() {
    QSettings s("Lape", "LapesEye");
    return s.value("cache/clear_days", 30).toInt();
}
int SettingsDialog::thumb_size_default() {
    QSettings s("Lape", "LapesEye");
    return s.value("thumb_size", 220).toInt();
}
bool SettingsDialog::preload_full_quality() {
    QSettings s("Lape", "LapesEye");
    return s.value("cache/preload_full", true).toBool();
}
bool SettingsDialog::histogram_visible() {
    QSettings s("Lape", "LapesEye");
    return s.value("ui/histogram_visible", true).toBool();
}
QString SettingsDialog::external_editor_path() {
    QSettings s("Lape", "LapesEye");
    return s.value("editor/path", "").toString();
}
QString SettingsDialog::external_editor_args() {
    QSettings s("Lape", "LapesEye");
    return s.value("editor/args", "").toString();
}
bool SettingsDialog::external_editor_as_layer() {
    QSettings s("Lape", "LapesEye");
    return s.value("editor/as_layer", false).toBool();
}

SettingsDialog::ColorMode SettingsDialog::color_mode() {
    QSettings s("Lape", "LapesEye");
    int v = s.value("color/mode", 1).toInt();  // domyślnie sRGB
    return static_cast<ColorMode>(v);
}

// ─── Konstruktor ──────────────────────────────────────────────────────────────

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Ustawienia Lape's Eye");
    setMinimumWidth(480);
    setWindowModality(Qt::WindowModal);  // blokuje tylko okno główne, bez zaciemniania
    setup_ui();
    load_values();
}

void SettingsDialog::setup_ui() {
    auto* layout = new QVBoxLayout(this);

    // ── Bufor RAM ──────────────────────────────────────────────────────────────
    auto* ram_box = new QGroupBox("Bufor RAM (QPixmapCache)", this);
    auto* ram_form = new QFormLayout(ram_box);
    ram_form->setRowWrapPolicy(QFormLayout::WrapLongRows);

    m_ram_cache_mb = new QSpinBox(this);
    m_ram_cache_mb->setRange(256, 32768);
    m_ram_cache_mb->setSingleStep(256);
    m_ram_cache_mb->setSuffix(" MB");
    m_ram_cache_mb->setToolTip(
        "Miniatury przechowywane w RAM — dostęp natychmiastowy.\n"
        "Przy 64GB RAM można dać nawet 8192–16384 MB.");

    m_ram_cache_label = new QLabel(this);
    m_ram_cache_label->setStyleSheet("color: #888; font-size: 10px;");

    ram_form->addRow("Rozmiar:", m_ram_cache_mb);
    ram_form->addRow("", m_ram_cache_label);
    layout->addWidget(ram_box);

    QObject::connect(m_ram_cache_mb, qOverload<int>(&QSpinBox::valueChanged),
                     this, &SettingsDialog::update_size_labels);

    // ── Bufor dysku ────────────────────────────────────────────────────────────
    auto* disk_box = new QGroupBox("Bufor dysku (SQLite)", this);
    auto* disk_form = new QFormLayout(disk_box);

    m_disk_cache_mb = new QSpinBox(this);
    m_disk_cache_mb->setRange(0, 10000000);      // dowolny limit; 0 = bez limitu
    m_disk_cache_mb->setSingleStep(256);
    m_disk_cache_mb->setSuffix(" MB");
    m_disk_cache_mb->setSpecialValueText("Bez limitu");
    m_disk_cache_mb->setKeyboardTracking(false);
    m_disk_cache_mb->setToolTip(
        "Miniatury zapisane na dysku — trwałe między sesjami.\n"
        "Większy = więcej zdjęć bez ponownego generowania miniatur.\n"
        "Wpisz dowolną wartość w MB (1 GB = 1024 MB). 0 = bez limitu.");

    m_disk_cache_label = new QLabel(this);
    m_disk_cache_label->setStyleSheet("color: #888; font-size: 10px;");

    m_disk_cache_info = new QLabel(this);
    m_disk_cache_info->setStyleSheet("color: #aaa; font-size: 10px;");

    disk_form->addRow("Rozmiar:", m_disk_cache_mb);
    disk_form->addRow("", m_disk_cache_label);
    disk_form->addRow("Bieżący rozmiar:", m_disk_cache_info);
    layout->addWidget(disk_box);

    QObject::connect(m_disk_cache_mb, qOverload<int>(&QSpinBox::valueChanged),
                     this, &SettingsDialog::update_size_labels);

    // ── Czyszczenie ────────────────────────────────────────────────────────────
    auto* clear_box = new QGroupBox("Automatyczne czyszczenie", this);
    auto* clear_form = new QFormLayout(clear_box);

    m_clear_interval = new QComboBox(this);
    m_clear_interval->addItem("Nigdy",        0);
    m_clear_interval->addItem("Co tydzień",   7);
    m_clear_interval->addItem("Co 2 tygodnie", 14);
    m_clear_interval->addItem("Co miesiąc",   30);
    m_clear_interval->addItem("Co 3 miesiące", 90);
    m_clear_interval->setToolTip("Usuwa miniatury plików które nie były używane przez podany czas.");

    m_clear_on_start = new QCheckBox("Wyczyść brakujące pliki przy starcie", this);
    m_clear_on_start->setToolTip("Usuwa miniatury dla plików których już nie ma na dysku.");

    m_preload_full = new QCheckBox("Wczytuj pełną jakość w tle", this);
    m_preload_full->setToolTip(
        "Po załadowaniu szybkiej miniatury automatycznie\n"
        "ładuje wersję wysokiej jakości w tle.");

    clear_form->addRow("Czyść cache starszy niż:", m_clear_interval);
    clear_form->addRow("", m_clear_on_start);
    clear_form->addRow("", m_preload_full);

    // Przycisk ręcznego czyszczenia
    auto* clear_now_btn = new QPushButton("🗑  Wyczyść cache teraz", this);
    clear_now_btn->setStyleSheet("padding: 4px 12px;");
    clear_form->addRow("", clear_now_btn);
    QObject::connect(clear_now_btn, &QPushButton::clicked,
                     this, &SettingsDialog::clear_disk_cache_now);

    layout->addWidget(clear_box);

    // ── Interfejs ─────────────────────────────────────────────────────────────
    auto* ui_box  = new QGroupBox("Interfejs", this);
    auto* ui_form = new QFormLayout(ui_box);
    m_histogram_visible = new QCheckBox("Pokazuj histogram w panelu podglądu", this);
    m_histogram_visible->setToolTip(
        "Histogram RGB pod miniaturą podglądu.\n"
        "Wyłącz jeśli wolisz mieć więcej miejsca na podgląd.");
    ui_form->addRow("", m_histogram_visible);
    layout->addWidget(ui_box);

    // ── Motyw kolorystyczny ───────────────────────────────────────────────────
    auto* th_box  = new QGroupBox("Motyw kolorystyczny", this);
    auto* th_lay  = new QVBoxLayout(th_box);

    // Tło
    m_theme_slider_lbl = new QLabel(this);
    m_theme_slider = new QSlider(Qt::Horizontal, this);
    m_theme_slider->setRange(0, 100);
    m_theme_slider->setToolTip("Od obecnego ciemnego tła (lewo) do białego (prawo).\n"
                               "Kolor tekstu dopasowuje się automatycznie.");
    auto* sl_row = new QHBoxLayout();
    sl_row->addWidget(new QLabel("Ciemne", this));
    sl_row->addWidget(m_theme_slider, 1);
    sl_row->addWidget(new QLabel("Białe", this));
    th_lay->addWidget(m_theme_slider_lbl);
    th_lay->addLayout(sl_row);

    m_theme_bg_custom = new QCheckBox("Własny kolor tła (zamiast suwaka)", this);
    m_theme_bg_btn = new QPushButton("Wybierz kolor…", this);
    m_theme_bg_btn->setFixedWidth(150);
    m_theme_bg_btn->setProperty("leye_no_theme", true);   // próbka musi pokazać prawdziwy kolor
    m_theme_bg_btn->setToolTip("Koło wyboru koloru oraz suwak jasności/ciemności.");
    auto* bg_row = new QHBoxLayout();
    bg_row->addWidget(m_theme_bg_custom);
    bg_row->addStretch();
    bg_row->addWidget(m_theme_bg_btn);
    th_lay->addLayout(bg_row);

    // Akcent
    m_theme_accent = new QCheckBox("Własny kolor akcentu (zaznaczenia, aktywne elementy)", this);
    m_theme_acc_btn = new QPushButton("Wybierz kolor…", this);
    m_theme_acc_btn->setFixedWidth(150);
    m_theme_acc_btn->setProperty("leye_no_theme", true);
    auto* ac_row = new QHBoxLayout();
    ac_row->addWidget(m_theme_accent);
    ac_row->addStretch();
    ac_row->addWidget(m_theme_acc_btn);
    th_lay->addLayout(ac_row);

    auto* note = new QLabel("<small>Zmiana motywu wchodzi w życie po ponownym uruchomieniu programu.</small>", this);
    note->setStyleSheet("color: #888;");
    th_lay->addWidget(note);
    auto* reset_btn = new QPushButton("Przywróć domyślny", this);
    th_lay->addWidget(reset_btn, 0, Qt::AlignLeft);
    layout->addWidget(th_box);

    QObject::connect(m_theme_slider,     &QSlider::valueChanged, this, [this](int)  { update_theme_ui(); });
    QObject::connect(m_theme_bg_custom,  &QCheckBox::toggled,    this, [this](bool) { update_theme_ui(); });
    QObject::connect(m_theme_accent,     &QCheckBox::toggled,    this, [this](bool) { update_theme_ui(); });
    QObject::connect(m_theme_bg_btn, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_theme_bg_color, this, "Kolor tła");
        if (c.isValid()) { m_theme_bg_color = c; update_theme_ui(); }
    });
    QObject::connect(m_theme_acc_btn, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_theme_acc_color, this, "Kolor akcentu");
        if (c.isValid()) { m_theme_acc_color = c; update_theme_ui(); }
    });
    QObject::connect(reset_btn, &QPushButton::clicked, this, [this]() {
        m_theme_slider->setValue(0);
        m_theme_bg_custom->setChecked(false);
        m_theme_accent->setChecked(false);
        m_theme_bg_color  = QColor(0x1E, 0x1E, 0x1E);
        m_theme_acc_color = QColor(0x2D, 0x7D, 0xD2);
        update_theme_ui();
    });

    // ── Zarządzanie kolorem ───────────────────────────────────────────────────
    auto* col_box  = new QGroupBox("Zarządzanie kolorem", this);
    auto* col_form = new QFormLayout(col_box);

    m_color_mode = new QComboBox(this);
    m_color_mode->addItem("Brak — surowe RGB (najszybszy)",         0);
    m_color_mode->addItem("Konwertuj do sRGB (zalecane)",           1);
    m_color_mode->addItem("Konwertuj do profilu monitora",          2);
    m_color_mode->setToolTip(
        "Brak: wyświetla surowe wartości RGB bez korekcji profilu ICC.\n"
        "  Szybko, ale zdjęcia Adobe RGB mogą wyglądać wyblakle.\n\n"
        "sRGB: konwertuje profil ICC zdjęcia (Adobe RGB, ProPhoto itd.)\n"
        "  na sRGB przed wyświetleniem. Zgodne z Bridge i przeglądarkami.\n\n"
        "Profil monitora: konwertuje do profilu ICC monitora (jeśli dostępny).\n"
        "  Najdokładniejsze — wymaga skalibrowanego monitora z profilem.");
    col_form->addRow("Tryb:", m_color_mode);

    auto* col_info = new QLabel(
        "<small style='color:#888'>"
        "Ustawienie wpływa na wyświetlanie miniatur, podglądu i pełnoekranowego.<br>"
        "Zmiana wymaga odświeżenia miniatur (Wyczyść cache → restart)."
        "</small>", this);
    col_info->setTextFormat(Qt::RichText);
    col_form->addRow("", col_info);
    layout->addWidget(col_box);

    // ── Zewnętrzny edytor ─────────────────────────────────────────────────────
    auto* ed_box  = new QGroupBox("Zewnętrzny edytor (E)", this);
    auto* ed_form = new QFormLayout(ed_box);
    ed_form->setRowWrapPolicy(QFormLayout::WrapLongRows);

    // Wiersz: pole ścieżki + przycisk Przeglądaj
    m_editor_path = new QLineEdit(this);
    m_editor_path->setPlaceholderText("/usr/bin/photoshop  lub  flatpak run com.adobe.Photoshop");
    m_editor_path->setToolTip("Ścieżka do pliku wykonywalnego edytora");

    m_editor_browse = new QPushButton("…", this);
    m_editor_browse->setFixedWidth(32);
    QObject::connect(m_editor_browse, &QPushButton::clicked,
                     this, &SettingsDialog::browse_editor);

    auto* path_row = new QHBoxLayout();
    path_row->addWidget(m_editor_path);
    path_row->addWidget(m_editor_browse);
    ed_form->addRow("Program:", path_row);

    // Dodatkowe argumenty
    m_editor_args = new QLineEdit(this);
    m_editor_args->setPlaceholderText("np. --new-window  (pozostaw puste jeśli nie potrzeba)");
    m_editor_args->setToolTip(
        "Argumenty przekazywane przed ścieżką pliku.\n"
        "Plik jest zawsze dodawany na końcu: program [args] plik.jpg");
    ed_form->addRow("Argumenty:", m_editor_args);

    // Opcja Photoshop — otwórz jako warstwę
    m_editor_as_layer = new QCheckBox("Otwórz jako warstwę w aktywnym dokumencie Photoshopa", this);
    m_editor_as_layer->setToolTip(
        "Gdy zaznaczone: zamiast uruchamiać program, wysyła plik do otwartego\n"
        "Photoshopa jako nową warstwę (wymaga Wine + Photoshop lub natywnego PS).\n"
        "Używa: File → Place Embedded / PS Actions / IPC przez Lape plugin.");
    ed_form->addRow("", m_editor_as_layer);

    // Info jak działa
    auto* ed_info = new QLabel(
        "<small style='color:#888'>Skrót <b>E</b> — otwórz zaznaczone zdjęcia w edytorze.<br>"
        "Skrót <b>Shift+E</b> — otwórz jako warstwę w Photoshopie (jeśli opcja zaznaczona).</small>",
        this);
    ed_info->setTextFormat(Qt::RichText);
    ed_form->addRow("", ed_info);

    layout->addWidget(ed_box);

    // ── Przyciski ──────────────────────────────────────────────────────────────
    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
        QDialogButtonBox::Apply, this);
    btns->button(QDialogButtonBox::Ok)->setText("OK");
    btns->button(QDialogButtonBox::Cancel)->setText("Anuluj");
    btns->button(QDialogButtonBox::Apply)->setText("Zastosuj");

    QObject::connect(btns, &QDialogButtonBox::accepted, this, [this]() {
        apply(); accept();
    });
    QObject::connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(btns->button(QDialogButtonBox::Apply),
                     &QPushButton::clicked, this, &SettingsDialog::apply);

    layout->addWidget(btns);
}

void SettingsDialog::load_values() {
    m_ram_cache_mb->setValue(ram_cache_mb());
    m_disk_cache_mb->setValue(disk_cache_mb());
    m_preload_full->setChecked(preload_full_quality());

    QSettings s("Lape", "LapesEye");
    m_clear_on_start->setChecked(s.value("cache/clear_on_start", true).toBool());

    int days = cache_clear_days();
    for (int i = 0; i < m_clear_interval->count(); ++i) {
        if (m_clear_interval->itemData(i).toInt() == days) {
            m_clear_interval->setCurrentIndex(i);
            break;
        }
    }

    m_histogram_visible->setChecked(histogram_visible());

    // Motyw kolorystyczny
    {
        const Theme::Cfg tc = Theme::load();
        m_theme_slider->setValue(tc.lighten);
        m_theme_bg_custom->setChecked(tc.bg_custom);
        m_theme_bg_color = tc.bg_color;
        m_theme_accent->setChecked(tc.accent_on);
        m_theme_acc_color = tc.accent_color;
        update_theme_ui();
    }

    // Zarządzanie kolorem
    int cm = static_cast<int>(color_mode());
    for (int i = 0; i < m_color_mode->count(); ++i) {
        if (m_color_mode->itemData(i).toInt() == cm) {
            m_color_mode->setCurrentIndex(i);
            break;
        }
    }

    // Zewnętrzny edytor
    m_editor_path->setText(external_editor_path());
    m_editor_args->setText(external_editor_args());
    m_editor_as_layer->setChecked(external_editor_as_layer());

    update_size_labels();
}

static QString leye_swatch_css(const QColor& c) {
    const QColor fg = c.lightnessF() > 0.55 ? QColor(Qt::black) : QColor(Qt::white);
    return QString("QPushButton { background: %1; color: %2; padding: 4px 8px; }")
        .arg(c.name(), fg.name());
}

void SettingsDialog::update_theme_ui() {
    const bool bgc = m_theme_bg_custom->isChecked();
    m_theme_slider->setEnabled(!bgc);
    m_theme_bg_btn->setEnabled(bgc);
    m_theme_acc_btn->setEnabled(m_theme_accent->isChecked());
    const int v = m_theme_slider->value();
    m_theme_slider_lbl->setText(
        bgc ? QString("Tło: własny kolor (suwak wyłączony)")
            : (v == 0 ? QString("Jasność tła: obecna")
                      : QString("Jasność tła: %1%").arg(v)));
    m_theme_bg_btn->setStyleSheet(leye_swatch_css(m_theme_bg_color));
    m_theme_acc_btn->setStyleSheet(leye_swatch_css(m_theme_acc_color));
}

void SettingsDialog::update_size_labels() {
    int ram = m_ram_cache_mb->value();
    m_ram_cache_label->setText(
        QString("≈ %1 miniatur w pamięci (przy śr. 150 KB/szt.)")
            .arg(ram * 1024 / 150));

    int disk = m_disk_cache_mb->value();
    if (disk <= 0)
        m_disk_cache_label->setText("Bez limitu — bufor rośnie dopóki go nie wyczyścisz");
    else
        m_disk_cache_label->setText(
            QString("≈ %1 miniatur na dysku (przy śr. 80 KB/szt.)")
                .arg(qint64(disk) * 1024 / 80));

    // Pokaż aktualny rozmiar bazy (jeśli dostępny przez settings)
    QSettings s("Lape", "LapesEye");
    double current_mb = s.value("cache/current_size_mb", 0.0).toDouble();
    if (current_mb > 0)
        m_disk_cache_info->setText(
            disk > 0
              ? QString("%1 MB (%2% limitu)")
                    .arg(current_mb, 0, 'f', 1)
                    .arg(current_mb / disk * 100, 0, 'f', 0)
              : QString("%1 MB").arg(current_mb, 0, 'f', 1));
    else
        m_disk_cache_info->setText("(nieznany — uruchom program żeby sprawdzić)");
}

void SettingsDialog::apply() {
    save_values();

    // Zastosuj RAM cache natychmiast
    int ram_kb = m_ram_cache_mb->value() * 1024;
    QPixmapCache::setCacheLimit(ram_kb);
}

void SettingsDialog::save_values() {
    QSettings s("Lape", "LapesEye");
    {   // Ręcznie wpisana ścieżka Photoshopa — zaproponuj wyłączenie ostrzeżenia
        const QString new_path = m_editor_path->text().trimmed();
        if (new_path != s.value("editor/path").toString() && QFileInfo::exists(new_path))
            leye_maybe_fix_ps_warning(this, new_path);
    }
    s.setValue("cache/ram_mb",       m_ram_cache_mb->value());
    s.setValue("cache/disk_mb",      m_disk_cache_mb->value());
    s.setValue("cache/clear_days",   m_clear_interval->currentData().toInt());
    s.setValue("cache/clear_on_start", m_clear_on_start->isChecked());
    s.setValue("cache/preload_full", m_preload_full->isChecked());
    s.setValue("ui/histogram_visible", m_histogram_visible->isChecked());
    s.setValue("color/mode", m_color_mode->currentData().toInt());

    // Motyw kolorystyczny
    {
        Theme::Cfg tc;
        tc.lighten      = m_theme_slider->value();
        tc.bg_custom    = m_theme_bg_custom->isChecked();
        tc.bg_color     = m_theme_bg_color;
        tc.accent_on    = m_theme_accent->isChecked();
        tc.accent_color = m_theme_acc_color;
        const Theme::Cfg old_tc = Theme::load();
        if (tc != old_tc) {
            Theme::save(tc);
            QMessageBox::information(this, "Motyw kolorystyczny",
                "Nowy motyw zostanie zastosowany po ponownym uruchomieniu programu.");
        }
    }

    // Zewnętrzny edytor
    s.setValue("editor/path",     m_editor_path->text().trimmed());
    s.setValue("editor/args",     m_editor_args->text().trimmed());
    s.setValue("editor/as_layer", m_editor_as_layer->isChecked());
}

void SettingsDialog::browse_editor() {
    QString path = QFileDialog::getOpenFileName(
        this, "Wybierz edytor", "/usr/bin",
        "Programy (*);; Wszystkie pliki (*)");
    if (!path.isEmpty()) {
        m_editor_path->setText(path);   // pytanie o Photoshopa pojawi się przy zapisie
    }
}

void SettingsDialog::clear_disk_cache_now() {
    auto btn = QMessageBox::question(this, "Wyczyść cache",
        "Usunąć wszystkie miniatury z cache dysku?\n"
        "Zostaną wygenerowane ponownie przy następnym przeglądaniu.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (btn != QMessageBox::Yes) return;

    QPixmapCache::clear();

    // Sygnał do MainWindow żeby wyczyścił ThumbCache
    emit cache_clear_requested();
}

} // namespace LapesEye
