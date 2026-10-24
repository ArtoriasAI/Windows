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
//   theme/custom   bool    false = suwak jasności, true = własny kolor
//   theme/lighten  int     0..100  (0 = obecny wygląd, 100 = biały)
//   theme/color    string  "#rrggbb" – własny kolor
//   theme/bg       bool    własny kolor zmienia tło
//   theme/accent   bool    własny kolor zmienia akcent
// Zmiana działa po ponownym uruchomieniu programu.

#include <QColor>
#include <QString>
#include <QSettings>
#include <QRegularExpression>
#include <QObject>
#include <QEvent>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace LapesEye {

class Theme {
public:
    struct Cfg {
        bool   custom  = false;
        int    lighten = 0;                       // 0..100
        QColor color   = QColor(0x2D, 0x7D, 0xD2);
        bool   bg      = true;
        bool   accent  = true;

        bool operator==(const Cfg& o) const {
            return custom == o.custom && lighten == o.lighten &&
                   color == o.color && bg == o.bg && accent == o.accent;
        }
        bool operator!=(const Cfg& o) const { return !(*this == o); }
    };

    static Cfg load() {
        QSettings s("Lape", "LapesEye");
        Cfg c;
        c.custom  = s.value("theme/custom",  false).toBool();
        c.lighten = std::clamp(s.value("theme/lighten", 0).toInt(), 0, 100);
        QColor col(s.value("theme/color", "#2d7dd2").toString());
        if (col.isValid()) c.color = col;
        c.bg      = s.value("theme/bg",     true).toBool();
        c.accent  = s.value("theme/accent", true).toBool();
        return c;
    }

    static void save(const Cfg& c) {
        QSettings s("Lape", "LapesEye");
        s.setValue("theme/custom",  c.custom);
        s.setValue("theme/lighten", c.lighten);
        s.setValue("theme/color",   c.color.name());
        s.setValue("theme/bg",      c.bg);
        s.setValue("theme/accent",  c.accent);
    }

    // Wołać raz, w głównym wątku, po setOrganizationName/ApplicationName.
    static void init() { compute(load()); }

    static bool active() { return st().active; }

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

    struct S {
        bool   active = false;
        bool   bg     = false;
        bool   accent = false;
        bool   tint   = false;     // true = barwimy szarości odcieniem własnego koloru
        double lb     = BG_L0;     // docelowa jasność tła
        double bg_h = 0, bg_s = 0;
        double acc_h = 0.58, acc_s = 0.64, acc_l = ACC_L0;
    };
    static S& st() { static S s; return s; }

    static void compute(const Cfg& cfg) {
        S& t = st();
        t = S{};
        if (!cfg.custom) {
            if (cfg.lighten <= 0) return;                  // wygląd domyślny
            const double k = cfg.lighten / 100.0;
            t.active = true;
            t.bg     = true;
            t.lb     = BG_L0 + (1.0 - BG_L0) * k;
            return;
        }
        // własny kolor
        const QColor col = cfg.color;
        if (cfg.bg) {
            t.bg   = true;
            t.tint = true;
            t.lb   = col.lightnessF();
            t.bg_h = col.hslHueF() < 0 ? 0.0 : col.hslHueF();
            t.bg_s = col.hslSaturationF();
        }
        if (cfg.accent) {
            t.accent = true;
            t.acc_h  = col.hslHueF() < 0 ? 0.0 : col.hslHueF();
            t.acc_s  = col.hslSaturationF();
            t.acc_l  = std::clamp(col.lightnessF(), 0.25, 0.75);
        }
        t.active = t.bg || t.accent;
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
            if (!cur.isEmpty() && cur != w->property("_leye_css_out").toString()) {
                const QString out = Theme::css(cur);
                w->setProperty("_leye_css_out", out);
                if (out != cur) w->setStyleSheet(out);
            }
        }
        return false;
    }
};

} // namespace LapesEye
