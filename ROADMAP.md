# Lape's Eye — Roadmap

## ✅ v0.1 — MVP
## ✅ v0.2 — RAW + Cache
## ✅ v0.3 — Wyszukiwanie + Eksport + Drukowanie
## ✅ v0.4 — Wydajność i duże foldery
## ✅ v0.5 — Funkcje użytkowe (historia, zewnętrzny edytor, ikona)

## 📋 v0.6 — Integracja z Lape
- [ ] Batch wysyłanie do Lape (kolejka z paskiem postępu)
- [ ] Watcher systemu plików (inotify — auto-odświeżanie)
- [ ] Podgląd edycji .leye w miniaturze (ramka koloru, gwiazdki na miniaturze)
- [ ] Pełna synchronizacja IPC (status połączenia, kolejka)
- [ ] Obsługa SMB/NFS (montowanie dysków sieciowych)
- [ ] Port na Windows 11

## 📋 v0.7 — Lapes RAW Editor (osobny program, C++/Qt6)
- [ ] Podstawowy pipeline: ekspozycja, balans bieli, krzywe
- [ ] Demozaik przez libraw
- [ ] HSL, redukcja szumów, sharpen
- [ ] Zapis jako .lraw sidecar (niedestrukcyjny)
- [ ] IPC z Lapes Eye

## 📋 v0.8 — Scalenie Lapes RAW z Lapes Eye
- [ ] Embedded panel w prawym docku (jak Camera Raw w Bridge)
- [ ] Podgląd live w PreviewPanel podczas edycji RAW
- [ ] Kolejka batch RAW → eksport JPEG/TIFF

## 📋 v0.9 — GPU (Vulkan/QRhi)
- [ ] Vulkan rendering dla PreviewPanel i FullscreenViewer
- [ ] Płynny zoom Lanczos na GPU
- [ ] Lepsza jakość miniatur przez GPU downsampling
