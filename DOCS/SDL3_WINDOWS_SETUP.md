# Setup SDL3 per la build Windows (branch `feature/sdl3-migration`)

Note operative per preparare una macchina Windows/VS2022 a compilare
`platforms/windows/GearSF7000.vcxproj` su questo branch. Completa
[`SDL3_MIGRATION_PLAN.md`](SDL3_MIGRATION_PLAN.md), che descrive il piano di
migrazione ma non i passi macchina-per-macchina.

## Percorso atteso

Il progetto referenzia SDL3 con path relativo al progetto, non assoluto:

```text
AdditionalIncludeDirectories: dependencies\SDL3\include;dependencies\SDL3\include\SDL3
AdditionalLibraryDirectories: dependencies\SDL3\lib\x64
```

Va quindi popolata:

```text
platforms\windows\dependencies\SDL3\
├── include\
│   └── SDL3\        (SDL.h e tutti gli altri header)
└── lib\
    └── x64\
        ├── SDL3.lib
        └── SDL3.dll
```

Il pacchetto da scaricare e' l'asset `SDL3-devel-<versione>-VC.zip` dalle
release ufficiali di libsdl-org/SDL (pacchetto Visual Studio, non mingw).
Estratto lo zip, si copia il contenuto di `include\` e `lib\x64\` dentro
`dependencies\SDL3\...` come sopra, senza portare la versione nel nome della
cartella.

Nessuna modifica al `.vcxproj` e' necessaria: il path relativo funziona su
qualunque macchina indipendentemente dalla lettera di disco. Un commit
precedente sostituiva qui un path assoluto (`D:\SDL3\...`), valido solo sulla
macchina dove SDL3 risiedeva su quel disco; e' stato reso relativo per questo
motivo.

## Tracciamento in git

Come per SDL2 (`dependencies/SDL2-2.28.5`, `dependencies/SDL2-2.30.6`), i
binari SDL3 (`.lib`/`.dll`) sono committati nel repository sotto
`platforms\windows\dependencies\SDL3\`. Non c'e' nessuna regola in
`.gitignore` che la esclude: la cartella e' tracciata per intero, headers e
binari x64 compresi. Chi clona il branch la trova gia' pronta e non deve
scaricare nulla a parte.

## Nota indipendente: Keyword Qt residuo

`GearSF7000.vcxproj` eredita da un vecchio template Qt un
`<Keyword>Qt4VSv1.0</Keyword>` e un blocco `ProjectExtensions` con proprieta'
Qt, benche' il progetto non usi Qt (SDL2/SDL3 + Dear ImGui). Su una macchina
senza l'estensione Qt VS Tools installata, VS2022 puo' non riuscire a
caricare il progetto con l'errore *"Impossibile trovare l'applicazione su cui
si basa questo tipo di progetto"*. Non e' legato a SDL3: e' un residuo da
pulire separatamente rimuovendo quel `Keyword` e il blocco `ProjectExtensions`
Qt (il progetto `WavetableManagerTests` usa correttamente `Win32Proj`).
