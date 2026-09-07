# Icona dell'app

`iconfile.icns` in `platforms/macos/` si rigenera da qui. Il master e' un solo
PNG 1024, `l_cog_colour.png`, prodotto da `icon.py`.

```bash
cd platforms/macos/icon
python3 icon.py l_cog_colour.png k_cog.png j_gearword.png   # le tre varianti
python3 sheet.py                                            # provino 512..16
```

Poi l'iconset e la conversione:

```bash
python3 - <<'EOF'
from PIL import Image
import os, shutil
src = Image.open("l_cog_colour.png").convert("RGBA")
shutil.rmtree("GearSF7000.iconset", ignore_errors=True)
os.makedirs("GearSF7000.iconset")
for logical in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        px = logical * scale
        tag = "@2x" if scale == 2 else ""
        src.resize((px, px), Image.LANCZOS).save(
            f"GearSF7000.iconset/icon_{logical}x{logical}{tag}.png")
EOF
iconutil -c icns GearSF7000.iconset -o ../iconfile.icns
```

macOS cachea le icone in modo aggressivo: dopo `make bundle` serve un
`touch GearSF7000.app` e spesso un `killall Finder` per vedere il cambiamento.

## Com'e' costruita

Niente font e nessun tracciato di terze parti, tutto primitive geometriche.

**Il lettering SF-7000** e' ricostruito glifo per glifo come unione di
rettangoli e poligoni, con i contatori degli zeri sottratti. Gli angoli
arrotondati vengono da un passaggio blur + soglia sulla maschera, che arrotonda
anche i raccordi interni come nel lettering originale della scatola. Il
`GLYPHS` contiene anche G, E, A, R per la variante `j_gearword.png`; la R ha la
gamba diagonale, perche' con il montante destro pieno usciva identica alla A.

**L'ingranaggio** e' disegnato, non ricalcato. Sul sistema c'e' U+2699 in Apple
Symbols e Material Icons e' gia' nel repo, ma il primo e' un font Apple che non
conviene redistribuire dentro un'icona e il secondo e' presente solo come
header C compresso da ImGui. Disegnarlo evita entrambe le cose e lascia
regolare la massa dei denti: i parametri sono in cima a `cog_mask()`, e la cosa
che conta e' che i denti si allarghino verso la punta (`tip_frac` maggiore di
`base_frac`) — rastremati verso l'esterno sembrano raggi, non denti.

**I colori** della striscia e dell'ingranaggio sono la palette TMS9918 vera,
copiata da `src/Video.h`.

## Le varianti

| file | |
|---|---|
| `l_cog_colour.png` | **quella in uso**: ingranaggio a palette |
| `k_cog.png` | ingranaggio bianco — regge meglio sotto i 32 px |
| `j_gearword.png` | scritta GEAR al posto dell'ingranaggio |
| `h_stack.png`, `i_stack_pal.png`, `g_line.png` | studi precedenti, senza ingranaggio |
| `cog_compare.png` | le forme d'ingranaggio provate |

`sheet.png` mostra le prime tre a 512, 128, 64, 32 e 16 px. Vale la pena
guardarlo prima di cambiare qualcosa: a 16 px il testo sparisce comunque e
restano solo l'ingranaggio e la striscia a fare da segnale.
