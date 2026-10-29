#pragma once
// Motyw kolorystyczny programu.
//
// Cały interfejs ma wpisane na sztywno szarości (#1e1e1e, #2a2a2a, #ccc …)
// oraz niebieski akcent (#2D7DD2). Zamiast zmieniać setki miejsc ręcznie,
// Theme przelicza te kolory "w locie":
//   • Theme::c(QColor)   – dla kolorów rysowanych przez QPainter / QPalette
//   • Theme::css(QString)– dla arkuszy stylów (zamienia wszystkie #rrggbb)
//   • ThemeFilter        – filtr zdarzeń aplikacji, który sam przepuszcza
//                          przez css() każdy setStyleSheet() w programie
//
// Kolory spoza rodziny "szarości" i "niebieski akcent" (czerwone/zielone/
// żółte etykiety, odznaki RAW itp.) NIE są zmieniane.
//
// Ustawienia (QSettings "Lape"/"LapesEye"):
// Zmiana działa po ponownym uruchomieniu programu.

#include <QColor>
#include <QString>
#include <QSettings>
#include <QRegularExpression>
#include <QObject>
#include <QEvent>
#include <QWidget>
#include <QApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <QPalette>
#include <QFont>
#include <QPointer>
#include <atomic>
#include <functional>
#include <vector>
#include <algorithm>
#include <cmath>

namespace LapesEye {

class Theme {
public:
    struct Cfg {
        // Tło: suwak (0 = obecne, 100 = białe) albo własny kolor
        int    lighten    = 0;                       // 0..100
        bool   bg_custom  = false;
        QColor bg_color   = QColor(0x1E, 0x1E, 0x1E);
        // Akcent (zaznaczenia, aktywne elementy)
        bool   accent_on    = false;
        QColor accent_color = QColor(0x2D, 0x7D, 0xD2);
        // Rozmiar tekstu interfejsu w % (100 = obecny)
        int    font_pct     = 100;

        bool operator==(const Cfg& o) const {
            return lighten == o.lighten && bg_custom == o.bg_custom &&
                   bg_color == o.bg_color && accent_on == o.accent_on &&
                   accent_color == o.accent_color && font_pct == o.font_pct;
        }
        bool operator!=(const Cfg& o) const { return !(*this == o); }
    };

    static Cfg load() {
        QSettings s("Lape", "LapesEye");
        Cfg c;
        c.lighten   = std::clamp(s.value("theme/lighten", 0).toInt(), 0, 100);
        c.bg_custom = s.value("theme/bg_custom", false).toBool();
        QColor bc(s.value("theme/bg_color", "#1e1e1e").toString());
        if (bc.isValid()) c.bg_color = bc;
        c.accent_on = s.value("theme/accent_on", false).toBool();
        QColor ac(s.value("theme/accent_color", "#2d7dd2").toString());
        if (ac.isValid()) c.accent_color = ac;
        c.font_pct = std::clamp(s.value("theme/font_pct", 100).toInt(), 70, 160);
        return c;
    }

    static void save(const Cfg& c) {
        QSettings s("Lape", "LapesEye");
        s.setValue("theme/lighten",      c.lighten);
        s.setValue("theme/bg_custom",    c.bg_custom);
        s.setValue("theme/bg_color",     c.bg_color.name());
        s.setValue("theme/accent_on",    c.accent_on);
        s.setValue("theme/accent_color", c.accent_color.name());
        s.setValue("theme/font_pct",     c.font_pct);
    }

    // Wołać raz, w głównym wątku, po setOrganizationName/ApplicationName.
    static void init() {
        base_font() = QApplication::font();      // czcionka systemowa — punkt odniesienia dla skali
        compute(load());
        apply_font();
        flip_cur().store(target_flip(), std::memory_order_relaxed);   // start bez animacji
    }

    static bool active() { return st().active; }

    // ── Zmiana motywu na żywo ───────────────────────────────────────────────
    // Przelicza kolory, ustawia paletę, odświeża arkusze stylów wszystkich
    // widgetów i powiadamia subskrybentów (panele, które trzymają kolory
    // w ikonach / elementach list).
    static void apply(const Cfg& cfg) {
        compute(cfg);
        animate_flip();
        reapply_all();
    }

    // Komponent rejestruje funkcję odświeżającą swoje kolory po zmianie motywu.
    // Wpis znika sam, gdy ctx zostanie zniszczony.
    static void subscribe(QObject* ctx, std::function<void()> fn) {
        subs().push_back({ QPointer<QObject>(ctx), std::move(fn) });
    }

    // Paleta aplikacji (widgety bez własnego arkusza stylów).
    static void apply_palette() {
        QPalette pal;
        pal.setColor(QPalette::Window,          c(QColor(0x1E, 0x1E, 0x1E)));
        pal.setColor(QPalette::WindowText,      c(QColor(0xCC, 0xCC, 0xCC)));
        pal.setColor(QPalette::Base,            c(QColor(0x28, 0x28, 0x28)));
        pal.setColor(QPalette::AlternateBase,   c(QColor(0x24, 0x24, 0x24)));
        pal.setColor(QPalette::Text,            c(QColor(0xCC, 0xCC, 0xCC)));
        pal.setColor(QPalette::Button,          c(QColor(0x38, 0x38, 0x38)));
        pal.setColor(QPalette::ButtonText,      c(QColor(0xCC, 0xCC, 0xCC)));
        pal.setColor(QPalette::Highlight,       c(QColor(0x2D, 0x7D, 0xD2)));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        qApp->setPalette(pal);
    }

    static void reapply_all() {
        // Jedno przerysowanie na koniec zamiast setek w trakcie
        std::vector<QPointer<QWidget>> tops;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            tops.emplace_back(w);
            w->setUpdatesEnabled(false);
        }
        apply_palette();
        apply_font();
        const auto widgets = QApplication::allWidgets();
        for (QWidget* w : widgets) {
            if (w->property("leye_no_theme").toBool()) continue;
            if (!w->property("_leye_css_src").isValid()) continue;
            if (w->styleSheet().isEmpty()) {            // arkusz usunięty w międzyczasie
                w->setProperty("_leye_css_src", QVariant());
                w->setProperty("_leye_css_out", QVariant());
                continue;
            }
            const QString out = css(w->property("_leye_css_src").toString());
            w->setProperty("_leye_css_out", out);
            if (w->styleSheet() != out) w->setStyleSheet(out);
        }
        // subskrybenci (kopia — callback może rejestrować kolejnych)
        auto& v = subs();
        v.erase(std::remove_if(v.begin(), v.end(),
                    [](const Sub& x) { return x.ctx.isNull(); }), v.end());
        const auto copy = v;
        for (const auto& x : copy)
            if (!x.ctx.isNull()) x.fn();
        for (auto& w : tops)
            if (w) w->setUpdatesEnabled(true);     // przerysuje całe okno raz
    }


    // ── Przeliczenie pojedynczego koloru ─────────────────────────────────────
    static QColor c(const QColor& in) {
        const S& t = st();
        if (!t.active || !in.isValid()) return in;

        const double a  = in.alphaF();
        const double h  = in.hslHueF();          // -1 dla szarości
        const double s  = in.hslSaturationF();
        const double l  = in.lightnessF();

        // ── rodzina akcentu (niebieski ~210°) ───────────────────────────────
        if (is_accent(h, s, l)) {
            if (!t.accent) return in;
            const double nl = std::clamp(l * t.acc_l / ACC_L0, 0.08, 0.92);
            QColor out = QColor::fromHslF(t.acc_h, t.acc_s, nl);
            out.setAlphaF(a);
            return out;
        }

        // ── rodzina tła / szarości ─────────────────────────────────────────
        if (is_neutral(h, s, l)) {
            if (!t.bg) return in;
            const double nl = map_lightness(l, t.lb);
            double nh = h < 0 ? 0.0 : h;
            double ns = s;
            if (t.tint) { nh = t.bg_h; ns = t.bg_s; }
            QColor out = QColor::fromHslF(nh, std::clamp(ns, 0.0, 1.0),
                                          std::clamp(nl, 0.0, 1.0));
            out.setAlphaF(a);
            return out;
        }
        return in;       // czerwone, zielone, żółte … bez zmian
    }

    static QColor c(int r, int g, int b, int a = 255) { return c(QColor(r, g, b, a)); }

    // Odwrócenie kolorów liter (ciemny ↔ jasny motyw) następuje przy jasności
    // tła 50% (~43% suwaka), ale jest animowane w czasie (0,5 s), a nie skokowe.
    // 0 = ciemny wygląd, 1 = jasny; wartość zmienia animate_flip().
    static double flip_t(double /*lb*/) { return flip_cur().load(std::memory_order_relaxed); }

    static QColor mixc(const QColor& a, const QColor& b, double t) {
        return QColor(int(a.red()   * (1 - t) + b.red()   * t + 0.5),
                      int(a.green() * (1 - t) + b.green() * t + 0.5),
                      int(a.blue()  * (1 - t) + b.blue()  * t + 0.5));
    }

    // Tło zaznaczonej etykiety koloru (miniatura / folder). W ciemnym motywie
    // jak dawniej: kolor etykiety przyciemniony (lc / div). W jasnym motywie
    // etykieta jest rozjaśniana — mieszana z tłem — żeby tekst był czytelny.
    static QColor label_bg(const QColor& lc, int div) {
        const S& t = st();
        const QColor dark(lc.red() / div, lc.green() / div, lc.blue() / div);
        if (!t.active || !t.bg) return dark;
        const QColor light = mixc(c(QColor(0x28, 0x28, 0x28)), lc, 0.35);
        return mixc(dark, light, flip_t(t.lb));
    }

    // Tło "bieżącego" elementu listy (np. otwarty folder w panelu bocznym).
    // Ciemny motyw: ciemnoniebieski jak dawniej. Jasny motyw: jasny odcień
    // akcentu, żeby ciemny tekst był czytelny.
    static QColor soft_selection_bg() {
        const S& t = st();
        const QColor dark = c(QColor(0x1a, 0x3a, 0x5c));
        if (!t.active || !t.bg) return dark;
        const QColor light = mixc(c(QColor(0x1e, 0x1e, 0x1e)),
                                  c(QColor(0x2D, 0x7D, 0xD2)), 0.40);
        return mixc(dark, light, flip_t(t.lb));
    }
    // Kolor tekstu na soft_selection_bg()
    static QColor soft_selection_fg() {
        const S& t = st();
        if (!t.active || !t.bg) return QColor(Qt::white);
        return mixc(QColor(Qt::white), c(QColor(0xCC, 0xCC, 0xCC)), flip_t(t.lb));
    }

    // ── Arkusze stylów: podmień każdy #rgb / #rrggbb ─────────────────────────
    // Rozmiary czcionek przeskalowane suwakiem "Rozmiar tekstu".
    static int px(int n) { return std::max(1, int(std::lround(n * fontpct().load() / 100.0))); }
    static int pt(int n) { return std::max(1, int(std::lround(n * fontpct().load() / 100.0))); }

    static QString css(const QString& in) {
        if (in.isEmpty()) return in;
        QString cur = in;
        if (st().active && cur.contains('#')) {
            static const QRegularExpression re("#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\\b");
            QString out;
            out.reserve(cur.size() + 16);
            int last = 0;
            auto it = re.globalMatch(cur);
            while (it.hasNext()) {
                const auto m = it.next();
                out += cur.mid(last, m.capturedStart() - last);
                out += c(QColor(m.captured(0))).name();
                last = m.capturedEnd();
            }
            out += cur.mid(last);
            cur = out;
        }
        const int pct = fontpct().load();
        if (pct != 100 && cur.contains("font-size")) {
            static const QRegularExpression fre("font-size:\\s*(\\d+)\\s*(px|pt)");
            QString out;
            int last = 0;
            auto it = fre.globalMatch(cur);
            while (it.hasNext()) {
                const auto m = it.next();
                out += cur.mid(last, m.capturedStart() - last);
                out += QString("font-size: %1%2")
                           .arg(std::max(1, int(std::lround(m.captured(1).toInt() * pct / 100.0))))
                           .arg(m.captured(2));
                last = m.capturedEnd();
            }
            out += cur.mid(last);
            cur = out;
        }
        return cur;
    }

private:
    static constexpr double BG_L0  = 30.0 / 255.0;                 // #1e1e1e
    static constexpr double ACC_L0 = (0x2D + 0xD2) / 2.0 / 255.0;  // #2D7DD2

    static std::atomic<double>& flip_cur() { static std::atomic<double> v{0.0}; return v; }
    static double target_flip() {
        const S& t = st();
        return (t.active && t.bg && t.lb > 0.5) ? 1.0 : 0.0;
    }
    // Animacja odwrócenia liter: 0,5 s, łagodny start i koniec.
    static void animate_flip() {
        static QTimer* tm = nullptr;
        static QElapsedTimer el;
        static double from = 0.0, to = -1.0;
        const double target = target_flip();
        if (!tm) {
            tm = new QTimer(qApp);
            tm->setInterval(33);
            QObject::connect(tm, &QTimer::timeout, tm, []() {
                const double x = std::min(el.elapsed() / 500.0, 1.0);
                const double e = x * x * (3.0 - 2.0 * x);
                flip_cur().store(from + (to - from) * e, std::memory_order_relaxed);
                reapply_all();
                if (x >= 1.0) tm->stop();
            });
        }
        if (tm->isActive() && target == to) return;        // już zmierza w tę stronę
        if (std::abs(flip_cur().load() - target) < 1e-6) {
            flip_cur().store(target);
            to = target;
            tm->stop();
            return;
        }
        from = flip_cur().load();
        to   = target;
        el.restart();
        tm->start();
    }

    static std::atomic<int>& fontpct() { static std::atomic<int> v{100}; return v; }
    static QFont& base_font() { static QFont f; return f; }
    static int& applied_pct() { static int v = 100; return v; }
    // Domyślna czcionka aplikacji (widgety bez własnego rozmiaru) według skali.
    static void apply_font() {
        const int pct = fontpct().load();
        if (pct == applied_pct()) return;
        applied_pct() = pct;
        QFont f = base_font();
        if (f.pointSizeF() > 0)      f.setPointSizeF(base_font().pointSizeF() * pct / 100.0);
        else if (f.pixelSize() > 0)  f.setPixelSize(std::max(1, int(std::lround(base_font().pixelSize() * pct / 100.0))));
        QApplication::setFont(f);
    }

    struct Sub { QPointer<QObject> ctx; std::function<void()> fn; };
    static std::vector<Sub>& subs() { static std::vector<Sub> v; return v; }

    struct S {
        bool   active = false;
        bool   bg     = false;
        bool   accent = false;
        bool   tint   = false;     // true = barwimy szarości odcieniem własnego koloru
        double lb     = BG_L0;     // docelowa jasność tła
        double bg_h = 0, bg_s = 0;
        double acc_h = 0.58, acc_s = 0.64, acc_l = ACC_L0;
    };
    // Stan motywu: wskaźnik podmieniany atomowo (render GL czyta go z innego wątku).
    // Stare wersje celowo nie są zwalniane (kilkadziesiąt bajtów na zmianę).
    static std::atomic<const S*>& sp() { static std::atomic<const S*> p{ new S() }; return p; }
    static const S& st() { return *sp().load(std::memory_order_acquire); }

    static void compute(const Cfg& cfg) {
        S* n = new S{};
        S& t = *n;
        fontpct().store(std::clamp(cfg.font_pct, 70, 160));
        if (cfg.bg_custom) {
            const QColor col = cfg.bg_color;
            t.bg   = true;
            t.tint = true;
            t.lb   = col.lightnessF();
            t.bg_h = col.hslHueF() < 0 ? 0.0 : col.hslHueF();
            t.bg_s = col.hslSaturationF();
        } else if (cfg.lighten > 0) {
            t.bg = true;
            t.lb = BG_L0 + (1.0 - BG_L0) * (cfg.lighten / 100.0);
        }
        if (cfg.accent_on) {
            const QColor col = cfg.accent_color;
            t.accent = true;
            t.acc_h  = col.hslHueF() < 0 ? 0.0 : col.hslHueF();
            t.acc_s  = col.hslSaturationF();
            t.acc_l  = std::clamp(double(col.lightnessF()), 0.25, 0.75);
        }
        t.active = t.bg || t.accent;
        sp().store(n, std::memory_order_release);
    }

    static bool is_accent(double h, double s, double l) {
        if (h < 0) return false;
        const double deg = h * 360.0;
        return deg >= 190 && deg <= 235 && s >= 0.45 && l >= 0.17 && l <= 0.72;
    }
    static bool is_neutral(double h, double s, double l) {
        (void)h;
        return s < 0.30 || l < 0.20;
    }

    // Skala jasności: 0x1e (obecne tło) → lb. Ciemne motywy zachowują kierunek
    // (tekst jaśniejszy od tła), jasne odwracają go (tekst ciemniejszy od tła).
    static double map_lightness(double g, double lb) {
        // Dwie skale: ciemna (tekst jaśniejszy od tła) i jasna (tekst ciemniejszy).
        // W paśmie przejściowym obie są mieszane, więc kolor liter zmienia się płynnie.
        const double dark = (g < BG_L0) ? lb * (g / BG_L0)
                                        : lb + (g - BG_L0) * (1.0 - lb) / (1.0 - BG_L0);
        const double light = (g < BG_L0) ? lb - (BG_L0 - g) * 0.5
                                         : lb - (g - BG_L0) * (lb - 0.05) / (1.0 - BG_L0);
        const double k = flip_t(lb);
        return dark * (1.0 - k) + light * k;
    }
};

// Filtr zdarzeń: każdy widget, któremu ustawiono styleSheet, dostaje jego wersję
// przepuszczoną przez Theme::css(). Działa też na widgety tworzone później
// (menu, okna dialogowe, przyciski ścieżki itd.).
class ThemeFilter : public QObject {
public:
    explicit ThemeFilter(QObject* parent = nullptr) : QObject(parent) {}
protected:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        const auto t = ev->type();
        if ((t == QEvent::Polish || t == QEvent::StyleChange) && obj->isWidgetType()) {
            auto* w = static_cast<QWidget*>(obj);
            if (w->property("leye_no_theme").toBool()) return false;  // np. próbka koloru
            const QString cur = w->styleSheet();
            if (cur.isEmpty()) {
                if (w->property("_leye_css_src").isValid()) {
                    w->setProperty("_leye_css_src", QVariant());
                    w->setProperty("_leye_css_out", QVariant());
                }
                return false;
            }
            if (cur != w->property("_leye_css_out").toString()) {
                // Nowy arkusz ustawiony przez program: zapamiętaj oryginał
                // (potrzebny do przeliczenia przy zmianie motywu na żywo).
                const QString out = Theme::css(cur);
                w->setProperty("_leye_css_src", cur);
                w->setProperty("_leye_css_out", out);
                if (out != cur) w->setStyleSheet(out);
            }
        }
        return false;
    }
};

} // namespace LapesEye
