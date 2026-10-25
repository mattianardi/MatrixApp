# Stato dei lavori e punti da riprendere (pausa concordata, ripresa mercoledì)

## Funziona ed è verificato (QEMU)
- HIL Studio 0.33.3 gira dentro NEOS (server FastAPI) e la sua pagina si carica in un browser vero tramite porta inoltrata:
  `tools/run-hilstudio.py` (solo provato su Linux), `tools/check-hilstudio.py`.
- Correzioni di NEOS emerse da quel lavoro: TCP asincrono (finestra, ritrasmissione, close non bloccante), futex con identità esatta
  (le collisioni perdevano i risvegli), heap che si allarga, nomi di file fino a 255 caratteri, 256 slot di thread, 1536 mappe per processo,
  memoria condivisa/AF_UNIX/real-time/seriale/fb0/evdev (vedi docs/COMPAT.md).
- Suite `make test`: ultima esecuzione completa verde prima delle correzioni TCP/futex/memoria; dopo sono state rieseguite solo
  nettest, netappstest, tcpload (`tools/check-tcpload.py`) e le prove mirate. **Da rilanciare per intero.**

## In corso (non finito): M15 browser interno = Chromium vero
Strategia: il motore headless di Chromium (`headless_shell.real`, 306 MB) gira sul livello Linux di NEOS; un client Python lo pilota via CDP
(screencast -> finestra NEOS, input -> CDP). Sul computer di prova funziona con `--single-process` e usa solo 88 syscall, 11 delle quali mancavano:
aggiunte memfd_create, fallocate, getresuid/gid, setpriority, inotify (finto), timer_* e landlock/pkey (ENOSYS).
- `tools/mkchrome.py` -> `build/chrome.npk` (338 MB, `extract=root` = scompattato all'avvio come l'initrd). `tools/check-chrome.py` lo prova.
- Boot a 3 GiB: `kernel/boot.asm` mappa in cache fino a 3 GiB, `pmm.c` MAX_PAGES = 3 GiB (con -m 2048 ora si vedono 2 GiB).
- **Bug trovato, da correggere per primo**: `ls /opt/chrome` mostra `headless_shell.real` di **0 byte**. Il file supera i 256 MiB e `inode_write`
  in `kernel/ramfs.c` rifiuta file oltre 256 MiB (`off + n > 256<<20`): alzare il limite (e `inode_truncate`), poi rifare `check-chrome`.
- Poi: Chromium con `--remote-debugging-port` + client `neosbrowse.py` (Pillow per decodificare JPEG) che scrive nella finestra di `/dev/neosgfx`.

## Progettato, non ancora scritto: finestre di NEOS (serve a M15 e a M16)
`kernel/neosgfx.h` (protocollo: ogni open di /dev/neosgfx = una finestra; mmap dei pixel, ioctl NG_SETUP/NG_PRESENT, read di eventi) e
`chrops.per_open` (handle per file aperto, già nel codice di chr/fdt/linux). Manca: gestione finestre client in `kernel/gui.c`
(zorder dinamico, chrome, focus, instradamento tastiera e mouse, eventi), `kernel/neosgfx.c` (il dispositivo) e i client.
La modifica a fdt/linux/chr per `per_open` compila ma **non è stata provata**.

## Dopo: M16 finestre Win32 (user32/gdi32 in winrt sopra /dev/neosgfx), poi M14 multiprocessore
- M16: classi di finestra, CreateWindowEx, coda messaggi, DefWindowProc, BeginPaint/GDI base, controlli STATIC/BUTTON/EDIT, MessageBox.
- M14 (SMP): APIC, avvio degli AP, scheduler per CPU, TLB shootdown; piano a lock globale del kernel con codice utente parallelo.
- M13 (mouse USB UHCI/EHCI/OHCI) e M12 (misura prestazioni GUI) restano aperti.

## Strumenti utili
`tools/neos-vm.sh <MiB> <porta> <moduli>` avvia NEOS headless con la console su TCP; `tools/ser.py 'cmd' '@30:cmd lento'` ci scrive.
`NEOS_LIVE=/percorso.png` fa salvare lo schermo durante i test. Attenzione: `pgrep -f`/`pkill -f` con parole presenti nel comando uccidono la propria shell.
