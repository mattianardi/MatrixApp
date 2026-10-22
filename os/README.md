# MatrixOS

Kernel x86_64 da zero, pensato per applicazioni professionali: **deterministico**,
**scalabile**, con l'obiettivo di eseguire binari Linux e Windows tramite livelli di compatibilità.

## Stato attuale (v0.1)

| Componente | Stato |
|---|---|
| Boot multiboot → long mode, identity map 1 GiB | fatto |
| Console seriale + VGA, `kprintf`, panic | fatto |
| IDT, PIC, PIT a 1000 Hz, frame di contesto uniforme | fatto |
| Allocatore di pagine fisiche (bitmap, da mmap del bootloader) | fatto |
| Scheduler a priorità fisse, preemptive, round-robin per livello, selezione O(N) limitata, nessuna allocazione nel percorso di dispatch | fatto |
| Self-test automatico in QEMU (`make test`) | fatto |

## Build / test

Requisiti: `gcc`, `nasm`, `ld`, `objcopy`, `qemu-system-x86_64`.

```
make run    # avvia in QEMU (output su seriale)
make test   # esegue il self-test; exit code 0 se SELFTEST PASS
```

Il self-test verifica: preemption per priorità, latenza di risveglio di un task periodico real-time
pari a 0 tick con la CPU saturata da thread a priorità inferiore, starvation corretta del livello più basso,
equità del round-robin dentro lo stesso livello.

## Roadmap (ordine proposto)

1. **Memoria virtuale**: page table per processo, user mode (ring 3), TSS, heap del kernel.
2. **Syscall** (`syscall/sysret`) e processi; IPC a messaggi.
3. **Determinismo**: timer a risoluzione TSC/APIC, scheduling tickless, priority inheritance/ceiling sui mutex, CPU pinning/isolamento core, limiti di tempo per task.
4. **Scalabilità**: ACPI/APIC, SMP con run-queue per core, NUMA-awareness.
5. **Storage e rete**: driver virtio-blk/virtio-net → NVMe/e1000, VFS, filesystem (ext2 in sola lettura → scrittura/journaling), stack TCP/IP.
6. **Compatibilità Linux**: loader ELF statico, ABI delle syscall Linux (`read/write/mmap/futex/clone/...`), poi ELF dinamico e glibc/musl.
7. **Compatibilità Windows**: loader PE, sottosistema NT/Win32 sopra lo strato Linux (strada Wine/Proton) — il pezzo più grande dell'intero progetto.
8. Sicurezza e affidabilità: capability/permessi, ASLR/NX, log, watchdog.

Le fasi 6 e 7 sono di gran lunga le più costose: la compatibilità reale con applicazioni esistenti
richiede migliaia di syscall/API da replicare con la semantica esatta.
