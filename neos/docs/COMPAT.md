# Compatibilità applicativa: Linux, Windows, Python, LabVIEW

Requisito di progetto: su NEOS si installano ed eseguono **applicazioni, driver e librerie** pensate per Linux *e* per Windows,
in particolare i **runtime LabVIEW** (desktop = Windows, RT = Linux x86-64 / NI Linux RT) e le **applicazioni Python**
(esempio di riferimento: HiL Studio).

Questo documento dice, senza abbellire, **cosa funziona oggi, come lo si verifica e cosa manca**. Tutto è provato solo su QEMU.

## Come si misura (prima di installare qualsiasi cosa)

```
tools/compat-scan.py pe  <exe|dll|cartella>   # import Win32 usati e quanti ne fornisce winrt; GUI, 32 bit, .NET
tools/compat-scan.py py  <progetto Python>    # moduli, requirements, toolkit grafici, driver NI, USB/CAN, Windows-only
tools/compat-scan.py elf <binario|cartella>   # librerie richieste, accessi a /dev/bus/usb, X11, driver NI...
make hiltest                                  # sonde di un'applicazione Python da banco prova, eseguite dentro NEOS
```

Il verdetto `BLOCKED` indica un requisito che NEOS non soddisfa; l'elenco mostra quale.

## Stato per livello

| Livello | Stato | Note |
|---|---|---|
| Python 3.13 + pip | **funziona** | thread, socket, sqlite3, asyncio, ctypes, subprocess, `multiprocessing` (fork, `Lock`, `Value`, `shared_memory`), SCHED_FIFO, `mlockall`; wheel pure-Python e manylinux x86-64 con `pip install` |
| Memoria condivisa tra processi | **funziona** | `MAP_SHARED` (file e anonima, ereditata da `fork`), `/dev/shm`, semafori POSIX, futex condivisi, `msync` |
| Socket | **funziona** | IPv4 TCP/UDP, `AF_UNIX` (emulati su loopback, con file-segnaposto), `socketpair`, epoll |
| Tempo reale per i processi Linux | **parziale** | `sched_setscheduler(SCHED_FIFO/RR)` mappa le priorità 1..99 sulle priorità RT del kernel (sopra i processi ordinari); `PTHREAD_PRIO_INHERIT` accettato (futex PI) ma **senza** inversione di priorità risolta; un solo core |
| Porte seriali | **funziona** (QEMU, verificato con host reale su socket) | `/dev/ttyS2`, `/dev/ttyS3` (UART 16550 COM3/COM4; COM1 è la console e COM2 il canale degli agenti), `/dev/ttyUSB0` (FTDI: provato su QEMU; CP210x: scritto, **non provato**), `/dev/ttyACM0` (CDC-ACM: scritto, **non provato**, QEMU non lo emula); `termios`/`termios2`, `select`/`poll`, linee DTR/RTS. CH340 non supportato; nessun hub |
| Windows: Win32 non grafico | **parziale** | file, thread, sincronizzazione (anche SRW/condvar/InitOnce), registro (`Reg*`, persistente in `C:\ProgramData\neos-registry`), Winsock, tempo, COM solo avvio (`CoCreateInstance` fallisce come classe non registrata) |
| Windows: DLL reali | **funziona** | ricerca in cartella del programma, directory corrente, `C:\Windows\System32`, `C:\Windows`, `PATH`; nomi case-insensitive |
| Windows: GUI (user32/gdi32), DirectX, servizi, WMI, .NET, PE a 32 bit | **non c'è** | blocca LabVIEW Run-Time Engine **desktop** con pannelli frontali; vedi sotto |
| Driver | **non c'è** | nessun modello di driver di terze parti (né `.ko` né `.sys`): i driver sono codice del kernel NEOS |

## LabVIEW

### Run-Time Engine desktop (Windows, PE a 64 bit)
Un eseguibile LabVIEW compilato con interfaccia utente usa user32/gdi32/comctl32/ole32/oleaut32/shell32, il registro, COM,
`nisyscfg`/`NI-VISA`/`NI-DAQmx` e centinaia di API Win32 che NEOS non ha. **Oggi non parte.** Cosa serve, in ordine:

1. un sottosistema di finestre per programmi Win32 (user32 + gdi32 sul compositore di NEOS, superfici in memoria condivisa);
2. COM/OLE (classi registrate, `IUnknown`, marshalling in-process) e più registro;
3. i pacchetti NI stessi (il RTE e i driver NI sono software proprietario con licenza: vanno forniti dall'utente);
4. i driver hardware NI: NI-DAQmx/NI-VISA parlano con **driver di kernel** NI (`nipalk`, `NiViPal`, `nidaqmx`) che non si possono
   eseguire su un altro kernel. Serve un backend nuovo (USB-TMC/VISA su TCP/seriale, DAQ via protocollo proprio) o un porting da parte di NI.

I RTE senza interfaccia (DLL chiamate da altri programmi, servizi web LabVIEW) hanno bisogno solo di (2)–(4) più il caricamento
di DLL, già presente. Per sapere cosa manca *esattamente* ai file che si possiedono:
`tools/compat-scan.py pe "C:\Program Files\National Instruments\Shared\LabVIEW Run-Time"` (copiati sul disco C: di NEOS).

### LabVIEW RT (Linux x86-64 / NI Linux RT)
Gli eseguibili `lvrt` e le librerie NI Linux RT sono ELF glibc: **il formato è già supportato**. I blocchi sono altri:
moduli kernel NI (`NI-PAL`, `NiRtSystem`, FPGA/RIO: `/dev/nirio*`, `nifpga`), `sysfs`/`udev`, `/dev/bus/usb` e librerie NI
proprietarie. Con `tools/compat-scan.py elf <cartella>` si ottiene l'elenco degli accessi a dispositivi che NEOS non offre.

## Applicazioni Python (HiL Studio)

`make hiltest` esegue dentro NEOS `linuxtests/hil_smoke.py`: venti sonde delle funzioni che un'applicazione da banco prova
usa (thread periodici con jitter misurato, socket TCP/UDP, sqlite, asyncio, `multiprocessing`, ctypes, real-time, seriale, …).
Per il *proprio* progetto: `tools/compat-scan.py py <cartella>` e poi si installano le dipendenze con `pip install`.

Blocchi tipici che lo scanner segnala:

| Requisito dell'applicazione | Situazione |
|---|---|
| Qt/PySide/PyQt, Tk, wx, Kivy, pygame | **non c'è un server X11/Wayland**. Ci sono `/dev/fb0` (mmap, `FBIOGET_*SCREENINFO`) e `/dev/input/event0|1` (tastiera, puntatore): la base per Qt `linuxfb`+`evdev`; **non ancora provato con Qt reale**. Tk/wx/GTK (X11) non partono |
| Interfaccia web con JavaScript (Dash, Streamlit, React…) | il browser di NEOS mostra HTML e moduli, **non esegue JavaScript** |
| `nidaqmx`, `pyvisa` con NI-VISA | richiedono i driver NI (vedi sopra); `pyvisa-py` per TCP/IP e seriale è utilizzabile |
| `pyusb`/`libusb`, `hid`, `python-can` (SocketCAN) | mancano `/dev/bus/usb`, `sysfs`, `AF_CAN`: da fare |
| `ctypes.windll`, `pywin32`, COM | solo con il livello winrt e solo per ciò che è non grafico |
| Wheel con estensioni C (numpy, scipy, pandas…) | funzionano se esiste una wheel `manylinux` x86-64 per Python 3.13 |

## Cosa serve da chi sviluppa HiL Studio e dall'ambiente LabVIEW

- L'elenco `requirements.txt` e il tipo di interfaccia (Qt? web? console?) di HiL Studio, oppure il risultato di `compat-scan.py py`.
- Quali interfacce hardware usa (seriale, USB, CAN, Ethernet/UDP, NI-DAQmx, VISA).
- Per LabVIEW: versione esatta del RTE, se è desktop o RT, e se gli eseguibili hanno pannello frontale.
  Senza i binari reali ogni lavoro sul livello Windows resta ipotetico: lo scanner dice cosa manca dopo averli visti.
