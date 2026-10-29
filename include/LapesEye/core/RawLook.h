#pragma once
// "Wygląd RAW" — jedna wspólna korekta kontrastu i nasycenia dla plików RAW (ARW…)
// stosowana w miniaturach, podglądzie w prawym panelu i w pełnym ekranie, żeby
// wszystkie widoki wyglądały tak samo (i zbliżone do Adobe Bridge).
//
// Ustawienia (QSettings "Lape"/"LapesEye"), zmieniane w Ustawieniach → Kolor i edytor:
//   raw_look/vibrance    0..100    domyślnie 12   (selektywne nasycenie; mocniej na mało nasyconych)
//   raw_look/saturation  -100..100 domyślnie 0    (zwykłe nasycenie wszystkich kolorów)
//   raw_look/contrast    -100..100 domyślnie 0    (krzywa S wokół środka; 0 = bez zmian)
//
// Wartości są trzymane w atomach (czytane z wątków roboczych); reload() wołać
// z głównego wątku po starcie i po zmianie ustawień.

#include <QImage>
#include <QSettings>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace LapesEye {

class RawLook {
public:
    static constexpr int DEF_VIBRANCE   = 12;
    static constexpr int DEF_SATURATION = 0;
    static constexpr int DEF_CONTRAST   = 0;

    static void reload() {
        // Suwaki w Ustawieniach zostały na razie usunięte — stosujemy wartości domyślne.
        // (Żeby je przywrócić, wystarczy znów czytać raw_look/* z QSettings.)
        vib().store(DEF_VIBRANCE);
        sat().store(DEF_SATURATION);
        con().store(DEF_CONTRAST);
    }

    static QImage apply(const QImage& src) {
        const float vibrance   = vib().load() / 100.f;
        const float saturation = sat().load() / 100.f;
        const float contrast   = con().load() / 100.f;
        if (src.isNull()) return src;
        if (vibrance == 0.f && saturation == 0.f && contrast == 0.f) return src;

        // Krzywa kontrastu jako tablica 256 wartości: y = x + c·(smoothstep(x) − x)
        // (punkty 0, 0.5 i 1 pozostają bez zmian)
        unsigned char lut[256];
        for (int i = 0; i < 256; ++i) {
            const float x = i / 255.f;
            const float sm = x * x * (3.f - 2.f * x);
            const float y = std::clamp(x + contrast * (sm - x), 0.f, 1.f);
            lut[i] = (unsigned char)(y * 255.f + 0.5f);
        }

        QImage out = src.convertToFormat(QImage::Format_RGB32);
        for (int yy = 0; yy < out.height(); ++yy) {
            QRgb* line = reinterpret_cast<QRgb*>(out.scanLine(yy));
            for (int xx = 0; xx < out.width(); ++xx) {
                const QRgb px = line[xx];
                float r = lut[qRed(px)]   / 255.f;
                float g = lut[qGreen(px)] / 255.f;
                float b = lut[qBlue(px)]  / 255.f;

                if (saturation != 0.f) {
                    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    const float k = 1.f + saturation;
                    r = std::clamp(luma + (r - luma) * k, 0.f, 1.f);
                    g = std::clamp(luma + (g - luma) * k, 0.f, 1.f);
                    b = std::clamp(luma + (b - luma) * k, 0.f, 1.f);
                }
                if (vibrance != 0.f) {          // identyczny wzór jak dotychczasowy Vibrance +12
                    const float mx = std::max({r, g, b});
                    const float mn = std::min({r, g, b});
                    const float s  = (mx > 0.f) ? (mx - mn) / mx : 0.f;
                    const float boost = vibrance * (1.f - s);
                    const float avg = (r + g + b) / 3.f;
                    r = std::clamp(avg + (r - avg) * (1.f + boost), 0.f, 1.f);
                    g = std::clamp(avg + (g - avg) * (1.f + boost), 0.f, 1.f);
                    b = std::clamp(avg + (b - avg) * (1.f + boost), 0.f, 1.f);
                }
                line[xx] = qRgb((int)(r * 255.f + .5f), (int)(g * 255.f + .5f), (int)(b * 255.f + .5f));
            }
        }
        return out;
    }

private:
    static std::atomic<int>& vib() { static std::atomic<int> v{DEF_VIBRANCE};   return v; }
    static std::atomic<int>& sat() { static std::atomic<int> v{DEF_SATURATION}; return v; }
    static std::atomic<int>& con() { static std::atomic<int> v{DEF_CONTRAST};   return v; }
};

} // namespace LapesEye
