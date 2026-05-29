# Port Windows 11 — Lape's Eye

## Co już działa bez zmian

CMakeLists.txt ma pełną obsługę Windows (`if(WIN32)` bloki, vcpkg, RC file).
Cały kod używa Qt API, więc jest przenośny. Jeden wyjątek — patrz niżej.

---

## Jedyna zmiana kodu: LapeIPC (Unix socket → named pipe)

`QLocalSocket` działa na Windows, ale ścieżka socketa musi być w formacie
Windows named pipe. Na Linuksie używasz:

```
~/.config/lape/bridge/lape.sock
```

Na Windows `QLocalSocket` automatycznie tworzy named pipe z nazwy,
ale **nie może zawierać ukośników** — tylko samą nazwę.

**Zmień `LapeIPC::socket_path()` w `src/core/LapeIPC.cpp`:**

```cpp
QString LapeIPC::socket_path() {
#ifdef Q_OS_WIN
    // Windows: QLocalSocket używa named pipes — tylko nazwa (bez ścieżki)
    return QStringLiteral("lape-bridge");
#else
    QString cfg = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    QString dir = cfg + "/lape/bridge";
    QDir().mkpath(dir);
    return dir + "/lape.sock";
#endif
}
```

Tyle — żadnych innych zmian w kodzie.

---

## Struktura plików do dodania do repo

```
lapes-eye/
├── .github/
│   └── workflows/
│       └── windows-build.yml      ← CI/CD (GitHub Actions)
├── installer/
│   └── lapes-eye-installer.nsi    ← skrypt NSIS
├── vcpkg.json                     ← manifest zależności
├── build-windows.ps1              ← lokalny build script
└── LICENSE.txt                    ← potrzebny przez NSIS (utwórz jeśli nie ma)
```

---

## Budowanie lokalnie na Windows 11

### Wymagania (instaluj raz)

| Narzędzie | Skąd |
|-----------|------|
| Visual Studio 2022 Build Tools | https://visualstudio.microsoft.com/downloads/ → "Build Tools" → workload "Desktop development with C++" |
| Qt 6.7 MSVC 2019 x64 | https://www.qt.io/download-open-source → Qt Online Installer |
| CMake ≥ 3.20 | https://cmake.org/download/ |
| Git | https://git-scm.com/ |
| NSIS 3.x | https://nsis.sourceforge.io/ (tylko do instalatora) |

### Komendy

```powershell
# Zwykły build (deploy folder gotowy do uruchomienia)
.\build-windows.ps1 -QtDir "C:\Qt\6.7.3\msvc2019_64"

# Build + instalator .exe
.\build-windows.ps1 -QtDir "C:\Qt\6.7.3\msvc2019_64" -Installer

# Czysty build od zera
.\build-windows.ps1 -QtDir "C:\Qt\6.7.3\msvc2019_64" -Clean -Installer
```

---

## Budowanie przez GitHub Actions (CI/CD)

1. Wrzuć pliki z tego folderu do repo
2. Push do `main` lub utwórz tag `v0.5.0`
3. Actions automatycznie:
   - Instaluje Qt6 + vcpkg + MSVC
   - Buduje `lapes-eye.exe`
   - Uruchamia `windeployqt` (kopiuje Qt DLL-e)
   - Buduje `lapes-eye-setup-v0.5.0.exe` przez NSIS
   - Upload jako artifact (do pobrania z zakładki Actions)
   - Przy tagu `v*` — tworzy GitHub Release z instalatorem

---

## Co robi instalator NSIS

- Instaluje do `C:\Program Files\LapesEye\`
- Tworzy skrót na pulpicie i w Menu Start
- Rejestruje rozszerzenie `.leye` (kolekcje)
- Pojawia się w "Dodaj/Usuń programy" z możliwością odinstalowania
- Uninstaller usuwa pliki aplikacji (cache użytkownika w `%APPDATA%` zostaje)

---

## Dane użytkownika na Windows

Twój kod używa `QStandardPaths` — automatycznie mapuje na właściwe ścieżki:

| Linux | Windows |
|-------|---------|
| `~/.cache/lapes-eye/` | `%LOCALAPPDATA%\lapes-eye\cache\` |
| `~/.config/lape/` | `%APPDATA%\lape\` |

Żadnych zmian w kodzie nie potrzeba.

---

## Potencjalne problemy przy kompilacji

### `rdynamic` — już obsłużone
CMakeLists.txt ma `if(NOT WIN32)` wokół `-rdynamic`. ✓

### `backtrace` w crash handlerze
Jeśli masz `#include <execinfo.h>` gdziekolwiek — owiń w `#ifndef Q_OS_WIN`.
Szukaj: `grep -r "execinfo" src/`

### `SIGBUS` — nie istnieje na Windows  
Jeśli używasz w crash handlerze — dodaj `#ifndef Q_OS_WIN` wokół.

---

## Rozmiar instalatora (szacunkowy)

| Składnik | Rozmiar |
|----------|---------|
| lapes-eye.exe | ~3–5 MB |
| Qt6 DLL-e | ~25–35 MB |
| libraw.dll + exiv2.dll | ~8–12 MB |
| Pozostałe DLL-e | ~5 MB |
| **Instalator (LZMA)** | **~25–35 MB** |
