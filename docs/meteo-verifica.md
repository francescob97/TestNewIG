# Il meteo — come provarlo

> Le decisioni sono in `docs/meteo-design.md`; il racconto per chi impara in
> `docs/manuale/15-meteo.md`.

**Cosa è già provato qui, senza Unreal** (`geoweather_tests`, sezioni 6–10,
24 test sulla precipitazione):

- cosa cade per quota: con 9,75 °C al mare lo zero termico è a 1500 m e la
  quota neve a 1200 m; pioggia in valle, metà e metà alla quota neve, neve in
  cima; neve e pioggia imposte valgono a ogni temperatura; sopra la cima delle
  nuvole (6 km, 10 per i rovesci) non cade niente;
- i rovesci: chiesta una copertura del 20, 50, 80%, piove su quella frazione di
  suolo, e nei nuclei si arriva al massimo; viaggiano con il vento in quota; la
  grandine cade nei nuclei (60%) e mai ai margini;
- visibilità e caduta: pioggia debole 13,5 km, a dirotto 1 km, 1 mm/h di neve
  1 km; gocce a 4–7 m/s, fiocchi a ~1, un chicco da 1 cm a 14,5 m/s, da 2 cm a 20,5;
- il suolo: bagnato dopo 5 minuti di pioggia, asciutto due ore dopo; la neve
  scende alla quota neve in un'ora e risale fondendo dove fa più di zero; sotto
  zero non fonde;
- le particelle: ogni tipo alla sua velocità, il vento porta di più i fiocchi,
  la camera che si muove non le trascina, nessun salto fra un frame e l'altro;
  la mesh (4 vertici nello stesso punto, tutti nella scatola).

Più i formati delle stampe (`geocore_tests`, sezione 10): "pioggia 4,2 mm/h",
"neve e pioggia 1,0 mm/h", "350 m", "1,2 km".

**Cosa non ho potuto provare:** la compilazione in Unreal, i materiali, le
particelle e la nebbia a schermo. I controlli statici (sezione 4 di
`docs/consegna.md`) sono verdi.

---

## 1. Aggiorna, compila, rifai i materiali

`git pull`, compila (*Development Editor*), apri l'editor e lancia **una volta**:

```
geo.Imagery.CreateMaterial
```

Crea `MPC_GeoWeather` e `M_GeoPrecipitation` in `/GeoWorld/Materials`, e
**rifà** `M_GeoTerrain` e `M_GeoRoad` con il nodo "Meteo sul suolo". Deve
dire in verde tutti i materiali; un giallo "manca MPC_GeoWeather" vuol dire
che la collezione non si è creata (mandami il log).

Poi salva gli asset (*File → Save All*): sono asset del plugin.

## 2. Il sereno, prima

Apri un terreno come sempre (con l'acqua, se ce l'hai: serve il geoide per la
quota sul mare). Nelle stampe a destra, sotto il vento, ci sono due righe
nuove:

| Riga | Esempio | Cosa vuol dire |
|---|---|---|
| Meteo | `niente` | cosa cade alla tua quota |
| Temperatura | `13,4 °C, zero termico 2.308 m` | l'aria alla tua quota, e dove fa 0 °C |

Con il sereno **non cambia niente** rispetto a prima: nessun attore nuovo,
nessuna nebbia toccata.

## 3. La pioggia

```
geo.Weather.Rain 4
geo.Weather.Debug 1
```

Scendi a qualche decina di metri dal suolo.

1. **Le gocce**: strisce chiare e sottili che cadono, inclinate dal vento
   (`geo.Weather.Wind 15 270`: molto inclinate, verso est). Girando la camera
   restano ferme nel mondo; muovendoti ti scorrono addosso.
2. **Le stampe**: `Meteo  pioggia 3,4 mm/h` (fra 2 e 4: 4 è il cuore, e la
   precipitazione non è uniforme), poi `Visibilita'` intorno ai 6 km.
3. **Il suolo**: in un paio di minuti il terreno si **scurisce** e diventa
   **lucido** (riflessi del cielo e del sole sulle strade). La riga `Suolo`
   dice `bagnato 100%`.
4. **La nebbia** (in Play: nell'editor la nebbia del livello non si tocca,
   sezione 7): l'orizzonte si chiude un po'. Con `geo.Weather.Rain 50` (a
   dirotto) la visibilità scende a ~1 km e le gocce diventano fitte.
5. `geo.Weather.Clear`: le gocce spariscono, la nebbia torna com'era in pochi
   secondi; il suolo resta bagnato e si asciuga in 40 minuti
   (`geo.Weather.Wetness 0` per asciugarlo subito).

L'overlay (in alto a sinistra, sotto quello del vento) dice il tempo impostato,
la temperatura con zero termico, quota neve e cima delle nuvole, il suolo,
cosa cade qui e al suolo, le particelle (quante per tipo, a che velocità
cadono) e la visibilità con la nebbia usata.

## 4. La neve, e la quota

```
geo.Weather.Snow 1
```

Fiocchi bianchi, lenti, che ondeggiano; la visibilità scende a ~1 km. In una
decina di minuti il suolo **si imbianca**, prima i prati e i campi (chiari),
poi i boschi; i pendii ripidi restano scuri. La riga `Suolo` dice `neve sopra
...` con una quota che scende.

Adesso la cosa interessante: **pioggia in valle, neve in montagna**, con lo
stesso tempo.

```
geo.Weather.Clear
geo.Weather.SnowCover off
geo.Weather.Temperature 8
geo.Weather.Precipitation 3
```

Con 8 °C al mare lo zero termico è a 1.231 m e la quota neve a ~930 m.

- In pianura (Torino, 240 m): **pioggia**.
- Sali (o `geo.Goto` su una montagna): verso 800–1.100 m le gocce si mescolano
  ai fiocchi (`Meteo  pioggia e neve ...`, poi `neve e pioggia ...`), sopra
  solo **neve**.
- Col tempo, sul suolo la neve si posa da ~930 m in su: da lontano si vede la
  **linea della neve** sulle montagne, più bassa sui versanti dolci.
- `geo.Weather.Temperature 2`: la quota neve scende quasi al mare, nevica anche
  in pianura.
- `geo.Weather.Clear` con `geo.Weather.Temperature 15`: la neve fonde,
  la linea risale piano (20 m all'ora per grado).

`geo.Weather.SnowCover 1500` mette subito la neve sopra 1.500 m, senza
aspettare.

## 5. La grandine

```
geo.Weather.Hail
```

Temporali sparsi (40% del suolo, celle da 6 km) che **viaggiano** con il vento
in quota: guardando dall'alto (2–3 km) vedi la nebbia più fitta sotto i
temporali spostarsi. Sotto un temporale: pioggia forte e, **nei nuclei**,
chicchi bianchi che cadono molto più veloci delle gocce. Ai margini solo
pioggia. Se non sei sotto un temporale (`Meteo  niente`), aspetta o spostati:
i rovesci passano.

`geo.Weather.Hail 30 1` mette la grandine ovunque ci sia un nucleo, senza buchi
fra i temporali.

## 6. Da Blueprint

Nel Level Blueprint: *Get GeoWeatherSubsystem* → *Get Precipitation At* con una
posizione (es. quella del pawn) → *Break GeoPrecipitationInfo*: mm/h, le tre
frazioni, la temperatura, la visibilità.

## 7. Se qualcosa non va

| Sintomo | Causa probabile | Cosa fare |
|---|---|---|
| Errore di compilazione in `GeoPrecipitationRenderer.cpp` o `GeoWeatherSubsystem.cpp` | codice mai compilato in Unreal | mandami le righe dell'Output Log |
| In giallo "manca MPC_GeoWeather" o "manca M_GeoPrecipitation" | i materiali non sono stati rifatti | `geo.Imagery.CreateMaterial`, poi aspetta 2 s (si cercano da soli) |
| Le stampe dicono `pioggia` ma non si vede niente | il materiale delle particelle non compila | apri `M_GeoPrecipitation`: se il nodo Custom è rosso, mandami l'errore |
| Le gocce sono puntini fermi, o tutte in un punto | il World Position Offset non arriva | in `M_GeoPrecipitation` controlla che "Particelle (WPO)" vada in *World Position Offset* |
| Il suolo non si bagna né s'imbianca | `M_GeoTerrain` vecchio (fatto prima di questo aggiornamento) | `geo.Imagery.CreateMaterial` |
| La neve si posa alla quota sbagliata di ~50 m | manca il geoide (dataset d'acqua non aperto): quote sull'ellissoide | apri l'acqua; senza, lo scarto è il geoide (~47 m a Torino) |
| Nell'editor la visibilità non cala | voluto: la nebbia del livello nell'editor non si tocca (si salverebbe) | prova in Play; l'overlay dice "quella del livello non si tocca nell'editor" |
| La nebbia resta fitta dopo `geo.Weather.Clear` | i valori originali non sono stati ripristinati | `geo.Weather.Fog 0` li rimette; dimmelo |
| Da 8 km di quota non piove | voluto: sopra la cima delle nuvole (6 km, 10 per i temporali) non cade niente | scendi |
| Il suolo è bagnato anche dove non piove | voluto: il bagnato è uno per il mondo, guidato da sotto la camera | `docs/meteo-design.md`, sezione 4 |
