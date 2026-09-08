# Pulizia warning build Windows (branch feature/sdl3-migration)

## Contesto

La build Debug|x64 di `platforms/windows/GearSF7000.sln` falliva con 3 errori
`C1060: spazio del compilatore per l'heap esaurito`, oltre a 236 avvisi
riportati da MSBuild. Nessuno dei due problemi esiste sulla build macOS: sono
entrambi limiti/comportamenti specifici del front-end di **cl.exe** (MSVC),
non bug nel codice C++ in sé - clang non li soffre.

## Gli errori C1060 (bloccanti)

Colpivano tre file di definizione tool MCP:
`mcp_tool_definitions_core.cpp`, `mcp_tool_definitions_input.cpp`,
`mcp_tool_definitions_debug.cpp`. Ognuno definisce una singola funzione con
decine di `tools.push_back({...})` di `nlohmann::json` innestati.

Ipotesi iniziale (funzione troppo grande) verificata e scartata: **spezzare
le funzioni in più funzioni statiche più piccole non ha risolto nulla**.
L'errore si spostava sempre sulla stessa singola espressione, non sulla
dimensione complessiva della funzione. La causa reale era la **profondità di
annidamento di una singola espressione literal** (`memory_ranges` -> `items`
-> `properties` -> ...), che fa esplodere la risoluzione degli overload dei
costruttori di `nlohmann::json` nel front-end di MSVC, indipendentemente da
quanto piccola sia la funzione che la contiene.

Fix: estratti i tre schemi coinvolti (`get_atomic_snapshot`,
`watch_set_condition`, `analyze_rewind_range`) in variabili `json` locali
costruite passo per passo, riducendo la profondità della singola
expression-tree - lo stesso schema già usato nel codice esistente per
`controller_macro_properties`. `/Zm500` era stato provato per primo e non ha
avuto alcun effetto (sui toolset recenti `/Zm` e' quasi un no-op).

Lo split in funzioni più piccole e' stato comunque mantenuto per leggibilità,
anche se non era la causa del C1060.

## I 236 avvisi

### Bug reale trovato e corretto

`src/definitions.h`, macro `GEARSF7000_TITLE_ASCII`: il banner ASCII-art
conteneva backslash letterali non escapati (`\` invece di `\\`), che il
compilatore interpretava come tentativi di escape sequence sconosciuta
(warning C4129, 6 istanze). Comportamento non definito dallo standard C++;
nel migliore dei casi il backslash veniva silenziosamente scartato dal
compilatore. Corretto raddoppiando i backslash del disegno (lasciando gli
`\n` finali intatti).

### Correzione FDC765: stato di write-protect reversibile

`src/NEC765.cpp::nec765SetReadOnly` ora usa il parametro `readonly`: imposta
`ST1_NW`/`ST3_WP` quando il drive e' protetto e li cancella quando torna
scrivibile. La catena
`mount_disk(write_protected=false)` -> `DiskWriteProtect` ->
`FDC765_SetReadOnly` puo' quindi rimuovere anche lo stato cached del FDC,
evitando che un disco sostituito con uno scrivibile erediti flag di
write-protect dal supporto precedente.

La regressione e' coperta da `tests/fdc_write_protect_tests.cpp`.

### Pulizia codice morto / parametri non usati (nessun cambio di comportamento)

File coinvolti: `platforms/desktop-shared/gui.cpp`,
`platforms/desktop-shared/gui_debug.cpp`,
`platforms/desktop-shared/renderer.cpp`, `src/Audio.cpp`,
`src/GearSF7000Core.cpp`, `src/MachineIOPorts.cpp`/`.h`,
`src/Memory_inline.h`, `src/SF7000.cpp`, `src/SegaBasicWavTape.cpp`,
`src/SP400.cpp`, `src/TapeMotor.cpp`, `src/TZXPlay.cpp`.

Pattern applicati:
- variabili locali mai lette, con inizializzazione priva di effetti
  collaterali osservabili (getter puri) -> dichiarazione rimossa;
- parametri di funzione mai letti nel corpo -> nome del parametro rimosso
  nella definizione (firma/tipo intatti, nessun impatto su chiamanti o su
  eventuali altre piattaforme);
- eccezione: `renderer.cpp::create_crt_pass_shader` - `metallib`/
  `metallibSize` sono usati solo dentro `#ifdef __APPLE__`. Non rimossi (sono
  necessari su macOS): silenziati con `(void)metallib; (void)metallibSize;`
  dentro `#ifndef __APPLE__`, innocuo su ogni piattaforma.

### Codici sistemati a livello di progetto (`GearSF7000.vcxproj`)

Aggiunti a `DisableSpecificWarnings` (tutte le configurazioni Debug/Release,
x64/ARM64) i codici pervasivi e innocui, in gran parte su codice audio/tape
di terze parti o porting Arduino (Blip_Buffer, Sms_Apu, TZXPlay/TZXDuino):

`4244` (conversione con perdita), `4267` (`size_t` -> tipo più piccolo),
`4291` (`operator new` con placement senza `delete` corrispondente), `4146`
(meno unario su `unsigned`), `4309`/`4838` (troncamento costante in
inizializzazione), `4805` (confronto `bool`/tipo intero), `4005`
(ridefinizione macro).

**Non toccati**: `4100` (parametro non referenziato) e `4189` (variabile
locale non referenziata). Il progetto li riattiva esplicitamente con
`-w34100 -w34189` in `AdditionalOptions`, con priorità sul `/wd` generato da
`DisableSpecificWarnings` - e' una scelta di qualità del codice del progetto,
non un'omissione: i siti che li generavano sono stati corretti uno per uno
(vedi sopra) invece di sopprimerli.

Anche `gui.cpp` ridefiniva `NOMINMAX` (già definito a livello di progetto):
aggiunta una guardia `#ifndef NOMINMAX`.

### Non file di progetto: pulizia locale

Un warning MSBuild `MSB8028` (directory intermedia `x64\Debug\` condivisa
con un altro progetto, `Gearcoleco.vcxproj` - il nome precedente di questo
stesso progetto prima del rename) è stato eliminato rimuovendo i file di
build residui (`Gearcoleco.*` in `platforms/windows/debug/` e
`platforms/windows/x64/Debug/`, più `Gearcoleco.vcxproj.user`). Tutti questi
file erano già esclusi da `.gitignore`: pulizia locale, non fa parte di
nessun commit.

## Statistica avvisi

| Fase                                              | Errori | Avvisi |
|----------------------------------------------------|:------:|:------:|
| Prima (build iniziale)                              |   3    |  236   |
| Dopo fix C1060 (schemi JSON estratti)               |   0    |  236   |
| Dopo `/Zm500` + split funzioni (nessun effetto reale)|   0    |  236   |
| Dopo `DisableSpecificWarnings` (senza 4100/4189)    |   0    |  118   |
| Dopo fix manuale dei siti C4100/C4189 restanti      |   0    |   1*   |
| Dopo pulizia `Gearcoleco.*` (MSB8028, non-compilatore)|   0    |   0    |

\* l'unico avviso rimasto dopo il fix dei siti C4100/C4189 era `MSB8028`
(MSBuild, non del compilatore), risolto con la pulizia dei residui di build.

Per codice, i 236 avvisi originali corrispondevano a 90 siti sorgente
distinti (file:riga), il resto erano ripetizioni dovute alla diagnostica
strutturata di MSBuild e alla compilazione batch di più file in un'unica
invocazione di `cl.exe`:

| Codice | Significato                                          | Siti | Trattamento |
|--------|-------------------------------------------------------|:----:|--------------|
| C4244  | conversione numerica con possibile perdita di dati     |  30  | soppresso a progetto (codice audio/tape) |
| C4267  | conversione `size_t` -> tipo più piccolo               |  11  | soppresso a progetto |
| C4100  | parametro di funzione non referenziato                 |  16  | corretto sito per sito |
| C4189  | variabile locale inizializzata e non referenziata      |  11  | corretto sito per sito |
| C4129  | escape sequence sconosciuta                            |   6  | bug reale corretto (definitions.h) |
| C4805  | confronto poco sicuro `bool`/tipo intero                |   6  | soppresso a progetto (TZXPlay, porting Arduino) |
| C4309/C4838 | troncamento di costante in inizializzazione       |   6  | soppresso a progetto (TZXDuino.h) |
| C4005  | ridefinizione di macro                                  |   2  | 1 corretto (guardia NOMINMAX), 1 soppresso (EOF in TZXDuino.h, uso interno coerente) |
| C4291  | `operator new` con placement senza `delete` corrispondente | 1 | soppresso a progetto (Effects_Buffer, libreria audio) |
| C4146  | meno unario applicato a tipo `unsigned`                | 1 | soppresso a progetto (Sms_Apu, libreria audio) |

## File toccati

- `platforms/windows/GearSF7000.vcxproj` - `DisableSpecificWarnings` e
  `AdditionalOptions` (`-Zm500`, mantenuto anche se non risolutivo per il
  C1060).
- `platforms/desktop-shared/mcp/mcp_tool_definitions_core.cpp`,
  `mcp_tool_definitions_input.cpp`, `mcp_tool_definitions_debug.cpp` - split
  in funzioni più piccole + estrazione schemi JSON annidati in variabili
  locali.
- `src/definitions.h` - escape dei backslash nel banner ASCII.
- `platforms/desktop-shared/gui.cpp`, `gui_debug.cpp`, `renderer.cpp` - codice
  morto rimosso, guardia `NOMINMAX`, `(void)` su parametri Metal-only.
- `src/Audio.cpp`, `src/GearSF7000Core.cpp`, `src/MachineIOPorts.cpp`/`.h`,
  `src/Memory_inline.h`, `src/NEC765.cpp` (solo nome parametro, non la
  logica), `src/SF7000.cpp`, `src/SegaBasicWavTape.cpp`, `src/SP400.cpp`,
  `src/TapeMotor.cpp`, `src/TZXPlay.cpp` - parametri/variabili non usate.
