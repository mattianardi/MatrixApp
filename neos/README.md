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
make test        # self-test RT + sessioni di shell scriptate (interop) + rete/pacchetti (anche manomissione)
```

## Stato e cosa manca (onestamente)

Fatto e testato in QEMU: boot, logo, scheduler RT, timer/jitter, console asincrona, registro impostazioni, API
syscall, shell unificata (VGA + seriale + tastiera PS/2), dock live, bus di variabili, rete, pacchetti, FS in RAM.

Non ancora fatto:

| Area | Stato |
|---|---|
| Modalità utente (ring 3), memoria virtuale per processo, `syscall/sysret` | da fare — prerequisito per eseguire programmi |
| Loader ELF + ABI syscall Linux; loader PE + sottosistema Win32/NT | da fare — i *dati* (variabili, percorsi, ambiente) sono già pronti |
| Storage persistente (ATA/NVMe/virtio-blk) e filesystem su disco | da fare — oggi FS in RAM; le impostazioni “boot” si passano da riga di comando |
| TLS / firme digitali dei pacchetti | da fare — oggi solo HTTP + SHA-256 + pin dell’indice |
| SMP/APIC, isolamento dei core, tickless, priority inheritance sui mutex | da fare |
| Grafica framebuffer / interfaccia a finestre | da fare — oggi modo testo 80×25 |
| USB, audio, GPU, driver di rete oltre e1000 | da fare |

Il non-determinismo noto: il kernel gira ora su QEMU/TCG, quindi le misure di jitter (`rt`) riflettono l’host;
su hardware reale con TSC invariante le statistiche diventano significative.
