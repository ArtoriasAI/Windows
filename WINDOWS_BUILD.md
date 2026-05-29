# Lape's Eye — Budowanie na Windows 11

## Wymagania

- Windows 11 64-bit
- Visual Studio 2022 (Community — darmowe) z komponentem **Desktop development with C++**
- Git for Windows
- CMake 3.25+ (instalator ze strony cmake.org)

---

## 1. Zainstaluj vcpkg (menedżer bibliotek)

```powershell
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
```

Dodaj do zmiennej środowiskowej `PATH`: `C:\vcpkg`

---

## 2. Zainstaluj zależności przez vcpkg

```powershell
vcpkg install qt6[core,gui,widgets,concurrent,network,sql,printsupport] --triplet x64-windows
vcpkg install exiv2 --triplet x64-windows
vcpkg install libraw --triplet x64-windows
```

> Uwaga: Qt6 przez vcpkg zajmuje ~3-5GB i kompiluje się ~30-60 minut.
> Alternatywa: zainstaluj Qt6 z oficjalnego instalatora qt.io (szybciej).

### Alternatywa — Qt6 z oficjalnego instalatora

1. Pobierz Qt Online Installer z https://www.qt.io/download-qt-installer
2. Zainstaluj: Qt 6.7 → MSVC 2022 64-bit
3. Dodaj do PATH: `C:\Qt\6.7.0\msvc2022_64\bin`

Wtedy pomiń `qt6` w vcpkg, zainstaluj tylko:
```powershell
vcpkg install exiv2 --triplet x64-windows
vcpkg install libraw --triplet x64-windows
```

---

## 3. Sklonuj i zbuduj

```powershell
git clone <repo-url> "Lapes Eye"
cd "Lapes Eye"

cmake -B build -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows ^
    -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release --parallel
```

Plik wykonywalny: `build\Release\lapes-eye.exe`

---

## 4. Zdeployuj DLL-ki (windeployqt)

Po zbudowaniu EXE potrzebujesz skopiować DLL Qt i inne obok pliku exe:

```powershell
cd build\Release
windeployqt6 --release lapes-eye.exe
```

To skopiuje automatycznie wszystkie potrzebne DLL-ki Qt.

Dodatkowo skopiuj ręcznie DLL z vcpkg:
```powershell
copy C:\vcpkg\installed\x64-windows\bin\exiv2.dll .
copy C:\vcpkg\installed\x64-windows\bin\libraw.dll .
# Sprawdź też: libexpat.dll, zlib1.dll, libiconv.dll
```

---

## 5. Skrót na pulpicie

Kliknij prawym na `lapes-eye.exe` → Wyślij do → Pulpit (utwórz skrót).
Ikona jest wbudowana w EXE przez plik RC.

---

## 6. Instalator (opcjonalnie — przyszłość)

Do stworzenia instalatora `.msi` lub `.exe` użyj:
- **NSIS** (darmowy): https://nsis.sourceforge.io
- **Inno Setup** (darmowy): https://jrsoftware.org/isinfo.php
- **CPack** (wbudowany w CMake):
  ```powershell
  cpack -G NSIS --config build\CPackConfig.cmake
  ```

---

## Rozwiązywanie problemów

### "Qt6 not found"
Ustaw zmienną: `set Qt6_DIR=C:\Qt\6.7.0\msvc2022_64\lib\cmake\Qt6`
lub dodaj do cmake: `-DQt6_DIR=C:\Qt\6.7.0\msvc2022_64\lib\cmake\Qt6`

### "exiv2 not found"
Upewnij się że vcpkg jest zintegrowane: `vcpkg integrate install`
i że toolchain jest przekazany do cmake.

### Czarny ekran konsoli przy uruchomieniu
Normalny przy Debug build. Release build używa `WIN32` subsystem — brak konsoli.

### Ikona nie pojawia się w pasku zadań
Uruchom `windeployqt6` — bez niego Windows może nie załadować ikony z QRC.
