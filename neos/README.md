# NEOS

**NEOS** — sistema operativo x86_64 scritto da zero, orientato a applicazioni **real-time** e professionali.
Autore: **Mattia Nardi**.

Al boot mostra il logo NEOS con nome, autore e un riepilogo del sistema (CPU, memoria, scheduler, rete, moduli).
Ogni modulo può aggiungere righe al riepilogo con `splash_info()`.

## Principi

1. **Deterministico**: scheduler a priorità fisse con prelazione, scelta O(N) limitata, nessuna allocazione nel
   percorso di dispatch, log asincrono (la stampa non blocca mai un thread RT), allocatore kernel O(1).
2. **Tutto è regolabile**: ogni modulo registra le proprie impostazioni (`tune_int/bool/str`). Sono *live*
   (effetto immediato) oppure *boot* (effetto al prossimo avvio, impostabili da riga di comando del kernel
   `neos.<modulo>.<nome>=<valore>`). I programmi le raggiungono con la stessa API: syscall `int 0x80`
   (`neos_api.h`) o la tabella `struct neos_api`.
3. **Un solo ambiente**: nessun salto tra “GUI” e “terminale”. Una schermata unica con barra di stato viva,
   viste live fissate (dock) e flusso dei comandi. I comandi restituiscono **tabelle**, non testo:
   `ps | where prio<5 | sort ticks desc | cols name prio ticks | pin Thread` lascia la vista aggiornata sullo schermo.
4. **Linux e Windows nello stesso posto**: un *bus di variabili* con un interprete in background mantiene un unico
   valore canonico e lo mostra a ciascun mondo nella sua convenzione.

## Interoperabilità Linux ⇄ Windows (bus di variabili)

| Scrivo da… | Valore | Lo leggo da Linux | Lo leggo da Windows |
|---|---|---|---|
| Linux `export PATH=/usr/bin:/mnt/c/tools` | lista di percorsi | `/usr/bin:/mnt/c/tools` | `\\neos\root\usr\bin;C:\tools` |
| Windows `set Path=C:\bin;D:\apps` | lista di percorsi | `/mnt/c/bin:/mnt/d/apps` | `C:\bin;D:\apps` |

- Sintassi accettate in qualsiasi riga: `$VAR`, `${VAR}`, `${VAR:-default}`, `%VAR%`, e le viste esplicite
  `${PATH@win}` / `%PATH@lin%`.
- Nomi equivalenti: `HOME`↔`USERPROFILE`, `USER`↔`USERNAME`, `TMPDIR`↔`TEMP`/`TMP`, `PWD`↔`CD`, ecc.
- Come ciascun mondo: `$VAR` non definita → stringa vuota; `%VAR%` non definita → resta letterale.
- Namespace `NEOS.*`: ogni impostazione è anche una variabile (`${NEOS.sched.quantum}`, `NEOS.timer.hz=2000`).
- `env lin|win` produce il blocco d’ambiente per un processo dell’uno o dell’altro mondo.


## Agenti AI come parte del kernel

Gli agenti sono *principal* del sistema operativo, non programmi qualsiasi. Il kernel non esegue il modello
(un LLM gira fuori); fornisce **identità, permessi, controllo, traccia e annullamento** a chiunque agisca:

- **Agenti nativi**: thread del kernel (es. `tuner`, un autotuner a ciclo chiuso). **Agenti remoti**: un modello
  esterno collegato alla seconda seriale con il protocollo a righe **NAP/1** (`tools/neos_agent.py`, usabile
  direttamente in un ciclo di tool-calling; `tools` restituisce lo schema JSON degli strumenti consentiti).
- **Un solo punto di ingresso** (`agent_call`): ogni azione passa da identità → capability → limite di frequenza →
  *scope* → politica di rischio → esecuzione → audit. Gli strumenti coprono osservazione (thread, memoria, jitter,
  impostazioni, variabili), azione (impostazioni, priorità, variabili Linux/Windows, file, pacchetti, rete) e
  **sviluppo di app dentro NEOS** (`fs.write` → `pkg.build` → `pkg.install_file`).
- **Capability con ambito, concesse solo dall’operatore**: `agent grant bot tune:timer.hz[500..4000],sched.quantum[2..20]
  fs.write:/src/ thread:minprio=4`. Intervalli di valori, prefissi di nome/percorso, banda di priorità. Un agente non può
  allargare i propri permessi. Un intervallo malformato non concede nulla (fail-closed).
- **Cosa nessun agente può toccare** (`TF_NOAGENT`): `pkg.allow_insecure`, `pkg.repo`, `pkg.index_sha256`, tutte le
  impostazioni `agent.*`; i thread di sistema (idle, console, net, shell, ui…) e quelli real-time sotto la sua banda.
- **Umano nel ciclo**: le azioni irreversibili (`fs.rm`, `thread.kill`, `pkg.remove`) chiedono conferma
  (`agent pending` / `approve` / `deny`; la barra di stato mostra `APPROVE: n`). Politica per rischio regolabile.
- **Tracciabilità**: registro di audit con **catena di hash** (`agent log`, `agent log verify`).
- **Annullamento**: ogni modifica reversibile finisce in un journal (`agent rollback <nome> [n]`, o `rollback` dall’agente).
- **Interruttore di emergenza**: `agent freeze` oppure **Ctrl+\** da tastiera, gestito a livello di interrupt, ferma tutti
  gli agenti anche se ogni thread è occupato.
- **Ottimizzazione sicura**: `experiment.run <metrica> <ms> nome=valore…` applica, misura, e **mantiene solo se migliora
  oltre `agent.min_gain_pct`**, altrimenti ripristina. Metriche: `rt.cost`, `rt.bound_us`, `rt.overhead_x100`,
  `timer.jitter_us`, `cpu.busy_pct`. L’agente nativo `tuner` lo usa per cercare i valori migliori entro i limiti concessi.

```
agent add coder remote                 # stampa il token una sola volta (RDRAND, oppure secret=<12+ caratteri>)
agent grant coder observe tune:sched.quantum[2..20] fs.write:/src/ pkg:myapp
agent tuner run rt.cost                # autotuner nativo (dopo: agent add tuner native + grant)
```

Limiti attuali, detti chiaramente:

- Il canale remoto è una **seriale fisica** con token (conservato solo come SHA-256, con ritardo anti-indovinello). Non c’è
  ancora un canale di rete perché manca TLS.
- Gli agenti **scrivono, impacchettano ed eseguono** app (`run` su un ELF x86-64 per NEOS); la compilazione va fatta
  fuori dal sistema finché non esiste una toolchain interna. Un ELF Linux standard gira grazie all'ABI Linux (sezione dedicata).
- Audit e journal sono in RAM: la catena è verificabile, non ancora persistente né firmata.
- La misura delle ottimizzazioni su QEMU è rumorosa; significativa solo su hardware reale con TSC invariante.

## Processi in modalità utente (ring 3)

- **Kernel nella metà alta** (`0xFFFFFFFF80000000`) con mappa diretta della RAM (`0xFFFF800000000000`); l'intera metà
  bassa è dei processi. Per questo i binari con indirizzi fissi classici (`0x400000`) funzionano senza trucchi.
- **Un address space per processo** (paging a 4 livelli, NX, SMEP/SMAP quando la CPU li ha, TSS, segmenti utente),
  stato FPU/SSE salvato per thread, limite di memoria per processo (`proc.max_pages`), memoria restituita alla morte.
- **Loader ELF64**: `ET_EXEC` agli indirizzi propri e `ET_DYN` (static-PIE, `R_X86_64_RELATIVE`), stack in stile
  System V (argv, envp, auxv). **L'ambiente del processo viene dal bus di variabili** nella convenzione del suo mondo
  (`run -w win` gli passa `HOME=C:\Users\neos` e `PATH` con `;`).
- **Isolamento verificato**: un programma che scrive o legge memoria del kernel, usa il puntatore nullo, esegue
  `cli`/`in`, esegue dati (NX) o divide per zero viene ucciso da solo; puntatori non validi nelle syscall danno `-EFAULT`.
- **Capacità = agenti**: ogni processo è un principal del gate degli agenti, parte con la sola osservazione e ottiene
  altro solo dall'operatore (`run -g tune:sched.quantum[2..20] programma`). Per ottimizzare o configurare il sistema
  a runtime un programma usa `USYS_TOOL`, cioè gli stessi strumenti (con gli stessi permessi, audit, approvazioni e
  rollback) di un agente AI. Nessuna syscall "privilegiata" parallela.
- Scrivere un programma: `user/neos.h` (nessuna libc). Comandi: `run [-b] [-g cap[:scope]] [-w lin|win] [-p prio] <prog>`,
  `proc`, `pkill`. Il filesystem iniziale arriva da un `initrd` (archivio NPK passato come modulo multiboot).

## Programmi Linux e Windows non modificati

**Linux (ELF x86-64)**: ABI syscall Linux completa per l'uso comune (`syscall` + ~120 chiamate: `fork/vfork/clone`
con thread, `execve` con shebang, `wait4`, pipe, `mmap/munmap/mprotect` con demand paging, `futex`, `poll`, `/proc`
sintetico, `arch_prctl`, FS/GS per thread). Girano senza modifiche binari **glibc, musl, Rust e Go** statici e
**BusyBox** (shell `sh`, pipeline, 82 applet linkati in `/bin`). `binfmt` associa estensioni a interpreti (`.nio`, `.sh`).

**Windows (PE64)**: il kernel riconosce i file `MZ/PE` e li avvia con `/bin/winrt`, il runtime utente di NEOS:
loader PE (rilocazioni, import, TLS implicito, DLL), TEB/PEB via GS, ~230 API **kernel32/ntdll/msvcrt** (file,
thread, sezioni critiche, eventi, mutex, semafori, `CreateProcess` con pipe, console ANSI, ambiente, tempo, `printf`
compatibile Windows). I percorsi `C:\dir\f.txt` sono gli **stessi file** di `/mnt/c/dir/f.txt`; l'ambiente
arriva dal bus di variabili nella forma Windows. Verificato con eseguibili compilati da `mingw`: un programma Windows
scrive un file che Linux legge, un altro lancia un figlio con pipe, una variabile impostata in un mondo appare
convertita nell'altro. Un'API non implementata viene segnalata solo se il programma la chiama davvero.

**Rete per i programmi Linux**: `socket/bind/listen/accept/connect/send/recv` (TCP e UDP su IPv4, anche verso `127.0.0.1`),
`poll`, `epoll` (anche edge-triggered) ed `eventfd`. Verificato con glibc (client/server in loopback, UDP, `getaddrinfo`),
 BusyBox `wget` e con un server **Go `net/http` non modificato** che gira dentro NEOS e viene mostrato dal browser di NEOS.

## Nio, il linguaggio di NEOS

`lang/nio.c` (interprete in C, gira su host e su NEOS): valori `nil/bool/int/float/str/list/map/fn`, closure,
pipeline `|>`, argomenti impliciti (`filter(.v >= 2)`), interpolazione `"{expr}"`, `try/catch` anche come espressione,
JSON, tabelle, `fs.*`, `path.win/lin`, `env.lin/win`, `run/sh/capture`, `sys.*` (stessi strumenti degli agenti) e `ai()`.
Convive con i linguaggi noti: ogni programma Linux/Windows si avvia dalla stessa shell.

## AIH - AI Help

`aih <cosa vuoi fare, in linguaggio naturale>`: un agente AI interpreta la richiesta e produce un piano di comandi
NEOS, Linux o Windows (anche traducendo i comandi dell'altro mondo). Il piano si rivede e si conferma
(`[y/N]`; i passi ad alto rischio richiedono `YES`). L'agente è quello collegato a NEOS via NAP (`SUB aih` / `EVT`,
risposta con lo strumento `aih.answer`); se nessuno risponde entro `aih.timeout_s` subentra un pianificatore locale
bilingue. Bottone **AI Help** nel workspace grafico.

## Spazio di lavoro grafico e browser

Framebuffer Bochs VBE 1024×768: pannello, barra delle applicazioni, finestre trascinabili con mouse PS/2:
**Terminale** (la stessa shell/dock), **Monitor di sistema** (CPU, memoria, jitter), **Agenti** (approva/nega con un clic,
blocco di emergenza) e **Browser**. Il browser carica `http://` (senza TLS), `file://` e `neos://`, disegna un
sottoinsieme di HTML (titoli, liste, tabelle, link, form con campi di testo/checkbox/pulsanti), con cronologia e scorrimento.
Le **app web di NEOS** (`neos://system|settings|agents|files`) sono servite dal kernel e modificano il sistema da form
(ad es. cambiare un'impostazione); i programmi Linux/Windows possono scrivere pagine in `/www` e vederle in `neos://www/...`.
Una pagina `http://` non può aprire link `neos://`. Limiti: niente JavaScript/CSS, niente HTTPS, form inviati con GET.

## Archiviazione persistente (NeoFS)

Driver ATA e filesystem a istantanee **sicure contro i crash** (due slot, intestazione con SHA-256 scritta per ultima;
se l'ultima istantanea è danneggiata si torna alla precedente). Sono persistenti `neofs.roots`
(`/data /home /root /opt /var /mnt/c /etc/neos`): file, unità `C:` di Windows, pacchetti e impostazioni salvate
(`tune save`). Comandi: `disk [status|sync|format]`; salvataggio automatico ogni `neofs.sync_s` secondi e allo spegnimento.
Si attiva con `-drive file=build/neos.disk,format=raw,if=ide` (`make run` lo crea).

## Rete e pacchetti

Driver e1000, ARP/IPv4/ICMP/UDP/TCP, DHCP, DNS, client HTTP. Pacchetti NPK con indice e SHA-256:
`pkg update | list | info | install | remove` (installa in `/opt/<nome>`). Repository: `tools/mkrepo.py`.

**Sicurezza**: il trasporto è HTTP semplice finché non esiste TLS, quindi `pkg` lo rifiuta finché non si imposta
`pkg.allow_insecure on`. Per un’integrità reale fissare l’indice con `pkg.index_sha256` (hash comunicato fuori banda).
I pacchetti sono verificati per dimensione e SHA-256 e validati per intero prima di scrivere qualsiasi file.

## Build e test

Requisiti: `gcc`, `nasm`, `ld`, `objcopy`, `qemu-system-x86_64`, `python3`.

```
make run         # avvia in QEMU (seriale su stdio, rete e1000 user-mode)
make test        # self-test RT, shell/interop, rete/pacchetti, ring 3, Linux, Windows, Nio, AIH, agenti, GUI, browser, disco
```

## Stato e cosa manca (onestamente)

Fatto e verificato in QEMU con test automatici (`make test`): kernel higher-half con scheduler RT, VM con demand paging,
processi/thread/fork/exec, ABI Linux, loader e runtime Windows, shell unificata a tabelle, bus di variabili Linux⇄Windows,
agenti nel kernel (capability, approvazioni, audit con hash, rollback, canale NAP), Nio, AIH, rete e pacchetti,
persistenza su disco, workspace grafico e browser.

| Area | Stato |
|---|---|
| TLS / HTTPS / firme dei pacchetti | non c'è: HTTP + SHA-256 + pin dell'indice |
| Rete per programmi Linux | socket IPv4 TCP/UDP (client e server, loopback), `epoll`, `eventfd`, `getaddrinfo` (`/etc/hosts`, `/etc/resolv.conf`); niente IPv6, socket Unix, raw socket |
| Segnali Linux | `sigaction`/`sigprocmask`/`sigsuspend`/`alarm`/`setitimer`, SIGCHLD, SIGSEGV recuperabile (`siglongjmp`), stato FPU preservato; maschera per processo (non per thread), niente `sigaltstack` né segnali in tempo reale con coda |
| Windows: solo PE64 con API Win32 di base | niente GUI Win32, DirectX, servizi, registro completo, PE a 32 bit |
| Browser: niente JavaScript/CSS/HTTPS | le web-app moderne con JS non funzionano |
| SMP/APIC (un solo core), tickless, priority inheritance | da fare |
| USB, audio, GPU, altri driver di rete (oggi e1000, ATA PIO, PS/2, Bochs VBE) | da fare |
| Hardware reale | provato solo su QEMU |

Il non-determinismo noto: il kernel gira su QEMU/TCG, quindi le misure di jitter (`rt`) riflettono l'host;
su hardware reale con TSC invariante le statistiche diventano significative.
