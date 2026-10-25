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
#include <QPalette>
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

        bool operator==(const Cfg& o) const {
            return lighten == o.lighten && bg_custom == o.bg_custom &&
                   bg_color == o.bg_color && accent_on == o.accent_on &&
                   accent_color == o.accent_color;
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
        return c;
    }

    static void save(const Cfg& c) {
        QSettings s("Lape", "LapesEye");
        s.setValue("theme/lighten",      c.lighten);
        s.setValue("theme/bg_custom",    c.bg_custom);
        s.setValue("theme/bg_color",     c.bg_color.name());
        s.setValue("theme/accent_on",    c.accent_on);
        s.setValue("theme/accent_color", c.accent_color.name());
    }

    // Wołać raz, w głównym wątku, po setOrganizationName/ApplicationName.
    static void init() { compute(load()); }

    static bool active() { return st().active; }

    // ── Zmiana motywu na żywo ───────────────────────────────────────────────
    // Przelicza kolory, ustawia paletę, odświeża arkusze stylów wszystkich
    // widgetów i powiadamia subskrybentów (panele, które trzymają kolory
    // w ikonach / elementach list).
    static void apply(const Cfg& cfg) {
        compute(cfg);
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
        apply_palette();
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
        for (QWidget* w : QApplication::allWidgets()) w->update();
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

    // Tło zaznaczonej etykiety koloru (miniatura / folder). W ciemnym motywie
    // jak dawniej: kolor etykiety przyciemniony (lc / div). W jasnym motywie
    // etykieta jest rozjaśniana — mieszana z tłem — żeby tekst był czytelny.
    static QColor label_bg(const QColor& lc, int div) {
        const S& t = st();
        if (!t.active || !t.bg || t.lb <= 0.5)
            return QColor(lc.red() / div, lc.green() / div, lc.blue() / div);
        const QColor base = c(QColor(0x28, 0x28, 0x28));
        const double k = 0.35;
        return QColor(int(base.red()   * (1 - k) + lc.red()   * k),
                      int(base.green() * (1 - k) + lc.green() * k),
                      int(base.blue()  * (1 - k) + lc.blue()  * k));
    }

    // Tło "bieżącego" elementu listy (np. otwarty folder w panelu bocznym).
    // Ciemny motyw: ciemnoniebieski jak dawniej. Jasny motyw: jasny odcień
    // akcentu, żeby ciemny tekst był czytelny.
    static QColor soft_selection_bg() {
        const S& t = st();
        const QColor navy(0x1a, 0x3a, 0x5c);
        if (!t.active || !t.bg || t.lb <= 0.5) return c(navy);
        const QColor base = c(QColor(0x1e, 0x1e, 0x1e));
        const QColor acc  = c(QColor(0x2D, 0x7D, 0xD2));
        const double k = 0.40;
        return QColor(int(base.red()   * (1 - k) + acc.red()   * k),
                      int(base.green() * (1 - k) + acc.green() * k),
                      int(base.blue()  * (1 - k) + acc.blue()  * k));
    }
    // Kolor tekstu na soft_selection_bg()
    static QColor soft_selection_fg() {
        const S& t = st();
        if (!t.active || !t.bg || t.lb <= 0.5) return QColor(Qt::white);
        return c(QColor(0xCC, 0xCC, 0xCC));
    }

    // ── Arkusze stylów: podmień każdy #rgb / #rrggbb ─────────────────────────
    static QString css(const QString& in) {
        if (!st().active || in.isEmpty() || !in.contains('#')) return in;
        static const QRegularExpression re("#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\\b");
        QString out;
        out.reserve(in.size() + 16);
        int last = 0;
        auto it = re.globalMatch(in);
        while (it.hasNext()) {
            const auto m = it.next();
            out += in.mid(last, m.capturedStart() - last);
            out += c(QColor(m.captured(0))).name();
            last = m.capturedEnd();
        }
        out += in.mid(last);
        return out;
    }

private:
    static constexpr double BG_L0  = 30.0 / 255.0;                 // #1e1e1e
    static constexpr double ACC_L0 = (0x2D + 0xD2) / 2.0 / 255.0;  // #2D7DD2

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
        if (lb <= 0.5) {
            if (g < BG_L0) return lb * (g / BG_L0);
            return lb + (g - BG_L0) * (1.0 - lb) / (1.0 - BG_L0);
        }
        if (g < BG_L0) return lb - (BG_L0 - g) * 0.5;
        return lb - (g - BG_L0) * (lb - 0.05) / (1.0 - BG_L0);
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
