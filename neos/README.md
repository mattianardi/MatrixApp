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
- Gli agenti **scrivono e impacchettano** app ma NEOS **non può ancora eseguirle** (manca modalità utente + loader);
  la compilazione va fatta dall’agente fuori dal sistema o attende una toolchain interna.
- Audit e journal sono in RAM (nessun disco): la catena è verificabile, non ancora persistente né firmata.
- La misura delle ottimizzazioni su QEMU è rumorosa; significativa solo su hardware reale con TSC invariante.

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
make test        # self-test RT + shell/interop + rete/pacchetti (anche manomissione) + agenti (permessi, approvazioni, tuner)
```

## Stato e cosa manca (onestamente)

Fatto e testato in QEMU: boot, logo, scheduler RT, timer/jitter, console asincrona, registro impostazioni, API
syscall, shell unificata (VGA + seriale + tastiera PS/2), dock live, bus di variabili, rete, pacchetti, FS in RAM.

Non ancora fatto:

| Area | Stato |
|---|---|
| Modalità utente (ring 3), memoria virtuale per processo, `syscall/sysret` | da fare — prerequisito per eseguire programmi (e per agenti come processi isolati) |
| Loader ELF + ABI syscall Linux; loader PE + sottosistema Win32/NT | da fare — i *dati* (variabili, percorsi, ambiente) sono già pronti |
| Storage persistente (ATA/NVMe/virtio-blk) e filesystem su disco | da fare — oggi FS in RAM; le impostazioni “boot” si passano da riga di comando |
| TLS / firme digitali dei pacchetti | da fare — oggi solo HTTP + SHA-256 + pin dell’indice |
| SMP/APIC, isolamento dei core, tickless, priority inheritance sui mutex | da fare |
| Grafica framebuffer / interfaccia a finestre | da fare — oggi modo testo 80×25 |
| USB, audio, GPU, driver di rete oltre e1000 | da fare |

Il non-determinismo noto: il kernel gira ora su QEMU/TCG, quindi le misure di jitter (`rt`) riflettono l’host;
su hardware reale con TSC invariante le statistiche diventano significative.
