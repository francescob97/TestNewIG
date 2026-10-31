@echo off
rem =============================================================================
rem  costruisci-edifici.bat -- Gli edifici per un dataset del terreno.
rem
rem  Tre modi di usarlo:
rem    - doppio clic: chiede il dataset del terreno (il nome di una cartella in
rem      Pipeline\dataset, o la cartella trascinata qui dentro con il mouse),
rem      l'estratto OpenStreetMap e il nome del dataset di edifici;
rem    - trascinare la cartella del dataset del terreno sopra questo file;
rem    - da riga di comando:   costruisci-edifici dted [file.osm.pbf] [nome]
rem
rem  Le sagome, le altezze, i tetti e i colori vengono da OpenStreetMap: un
rem  estratto .osm.pbf di Geofabrik (gratis, senza account). Se in
rem  Pipeline\dati\osm non ce n'e' nessuno, chiede se scaricarlo e quale
rem  regione. Del file si usa solo l'area del terreno.
rem
rem  Costruisce Pipeline\dataset\NOME, lo verifica, e mette negli appunti il
rem  comando da incollare nella console di Unreal.
rem
rem  Usa geoworld.bat: la prima volta prepara anche l'ambiente (5-15 minuti).
rem  Interrotto? Rilancialo uguale: riprende da dove era.
rem
rem  NOTA. Niente blocchi fra parentesi con dentro dei percorsi (vedi
rem  geoworld.bat): si va avanti a etichette e goto.
rem =============================================================================
setlocal EnableExtensions

set "GEO_HOME=%~dp0"
set "BLD_TERRAIN=%~1"
set "BLD_OSM=%~2"
set "BLD_NAME=%~3"
set "BLD_INTERACTIVE="
set "BLD_EXIT=0"
set "BLD_LOCAL=%GEO_HOME%dati\osm"

if defined BLD_TERRAIN goto :have_terrain
set "BLD_INTERACTIVE=1"
echo.
echo  Costruisce gli edifici (sagome di OpenStreetMap) per un dataset del terreno.
echo.
if not exist "%GEO_HOME%dataset\" goto :ask_terrain
echo  I dataset in Pipeline\dataset:
echo.
dir /b /ad "%GEO_HOME%dataset"
echo.
:ask_terrain
echo  Il dataset del TERRENO: il nome di una cartella qui sopra, o la cartella
echo  trascinata qui dentro con il mouse. Poi Invio:
set /p "BLD_TERRAIN=  > "
if not defined BLD_TERRAIN goto :no_terrain

:have_terrain
rem Via le virgolette (trascinando un percorso con spazi arrivano anche quelle)
rem e la barra finale.
set "BLD_TERRAIN=%BLD_TERRAIN:"=%"
if not defined BLD_TERRAIN goto :no_terrain
if "%BLD_TERRAIN:~-1%"=="\" set "BLD_TERRAIN=%BLD_TERRAIN:~0,-1%"
rem Solo il nome? Allora e' una cartella di Pipeline\dataset.
if exist "%BLD_TERRAIN%\" goto :terrain_found
if exist "%GEO_HOME%dataset\%BLD_TERRAIN%\" set "BLD_TERRAIN=%GEO_HOME%dataset\%BLD_TERRAIN%"
if not exist "%BLD_TERRAIN%\" goto :no_folder
:terrain_found
for %%I in ("%BLD_TERRAIN%") do set "BLD_TERRAIN=%%~fI"
if not exist "%BLD_TERRAIN%\manifest.json" goto :no_manifest
rem I dataset del terreno non dichiarano un "datasetKind"; gli altri si'
rem (ortofoto, strade, acqua, vegetazione, edifici): sarebbe la cartella sbagliata.
findstr /c:"datasetKind" "%BLD_TERRAIN%\manifest.json" >nul 2>&1
if not errorlevel 1 goto :not_terrain

rem --- l'estratto OpenStreetMap ---------------------------------------------------
if defined BLD_OSM goto :have_osm
set "BLD_OSM_COUNT=0"
for /f %%N in ('dir /b /a-d "%BLD_LOCAL%\*.osm.pbf" 2^>nul ^| find /c /v ""') do set "BLD_OSM_COUNT=%%N"
if "%BLD_OSM_COUNT%"=="0" goto :ask_download
if "%BLD_OSM_COUNT%"=="1" goto :first_local
echo.
echo  Gli estratti OpenStreetMap in Pipeline\dati\osm:
echo.
dir /b /a-d /o:n "%BLD_LOCAL%\*.osm.pbf"
echo.
echo  Quale? Il nome di uno qui sopra (Invio = il primo):
set /p "BLD_OSM=  > "
if defined BLD_OSM goto :have_osm
:first_local
for /f "delims=" %%F in ('dir /b /a-d /o:n "%BLD_LOCAL%\*.osm.pbf" 2^>nul') do if not defined BLD_OSM set "BLD_OSM=%BLD_LOCAL%\%%F"
if not defined BLD_OSM goto :no_osm
goto :have_osm

:ask_download
echo.
echo  Nessun estratto OpenStreetMap in "%BLD_LOCAL%".
echo  Lo scarico da Geofabrik (gratis, senza account)? Scrivi la regione e Invio:
echo.
echo    nord-ovest   Piemonte, Valle d'Aosta, Liguria, Lombardia
echo    nord-est     Trentino-Alto Adige, Veneto, Friuli-Venezia Giulia, Emilia-Romagna
echo    centro       Toscana, Umbria, Marche, Lazio
echo    sud          Abruzzo, Molise, Campania, Puglia, Basilicata, Calabria
echo    isole        Sicilia e Sardegna
echo    italia       tutta (il file piu' grosso)
echo.
echo  Invio = nord-ovest; "no" per uscire.
set "BLD_AREA=nord-ovest"
set /p "BLD_AREA=  > "
set "BLD_AREA=%BLD_AREA:"=%"
if /i "%BLD_AREA%"=="no" goto :no_osm
echo.
call "%GEO_HOME%geoworld.bat" fetch-osm --area "%BLD_AREA%" -o "%BLD_LOCAL%"
if errorlevel 1 goto :fetch_failed
set "BLD_OSM="
goto :first_local

:have_osm
set "BLD_OSM=%BLD_OSM:"=%"
rem Solo il nome? Allora e' un file di Pipeline\dati\osm.
if exist "%BLD_OSM%" goto :osm_found
if exist "%BLD_LOCAL%\%BLD_OSM%" set "BLD_OSM=%BLD_LOCAL%\%BLD_OSM%"
if not exist "%BLD_OSM%" goto :no_osm_file
:osm_found
for %%I in ("%BLD_OSM%") do set "BLD_OSM=%%~fI"

rem --- il nome del dataset di edifici -------------------------------------------
for %%I in ("%BLD_TERRAIN%") do set "BLD_TERRAIN_NAME=%%~nxI"
if defined BLD_NAME goto :have_name
set "BLD_NAME=edifici_%BLD_TERRAIN_NAME%"
if not defined BLD_INTERACTIVE goto :have_name
echo.
echo  Nome del dataset di edifici (Invio = edifici_%BLD_TERRAIN_NAME%):
set /p "BLD_NAME=  > "
:have_name
set "BLD_NAME=%BLD_NAME:"=%"
if not defined BLD_NAME set "BLD_NAME=edifici_%BLD_TERRAIN_NAME%"
rem Niente spazi nel nome della cartella del dataset: un problema in meno.
set "BLD_NAME=%BLD_NAME: =_%"
set "BLD_OUT=%GEO_HOME%dataset\%BLD_NAME%"

echo.
echo  [costruisci-edifici] terreno:       "%BLD_TERRAIN%"
echo  [costruisci-edifici] OpenStreetMap: "%BLD_OSM%"
echo  [costruisci-edifici] edifici:       "%BLD_OUT%"
echo.

rem --- build e verifica (geoworld.bat prepara l'ambiente, se serve) -------------
call "%GEO_HOME%geoworld.bat" build-buildings --osm "%BLD_OSM%" --terrain "%BLD_TERRAIN%" -o "%BLD_OUT%" --name "%BLD_NAME%"
if errorlevel 1 goto :build_failed
echo.
call "%GEO_HOME%geoworld.bat" verify-buildings -o "%BLD_OUT%"
if errorlevel 1 goto :verify_failed

rem --- il comando per Unreal, negli appunti ---------------------------------------
for %%I in ("%BLD_OUT%") do set "BLD_OUT_FULL=%%~fI"
set "BLD_UNREAL=geo.Buildings.Open "%BLD_OUT_FULL%""
powershell -NoProfile -Command "Set-Clipboard -Value $env:BLD_UNREAL" >nul 2>&1
echo.
echo  ==========================================================================
echo   Fatto. In Unreal, con il terreno gia' aperto, apri la console (tasto ~),
echo   incolla con Ctrl+V e Invio:
echo.
echo     %BLD_UNREAL%
echo.
echo   (e' gia' negli appunti). Oppure tutto in un colpo, 300 m sopra la zona
echo   con piu' edifici:
echo.
echo     geo.Buildings.Demo "%BLD_TERRAIN%" - "%BLD_OUT_FULL%"
echo.
echo   (al posto di - la cartella delle ortofoto, se ce l'hai).
echo   La prima volta, nell'editor: geo.Imagery.CreateMaterial (crea M_GeoBuilding).
echo  ==========================================================================
goto :end

rem =============================================================================
rem  Errori
rem =============================================================================

:no_terrain
echo.
echo  Nessun dataset indicato.
set "BLD_EXIT=1"
goto :end

:no_folder
echo.
echo  Non trovo "%BLD_TERRAIN%", ne' come cartella ne' dentro Pipeline\dataset.
set "BLD_EXIT=1"
goto :end

:no_manifest
echo.
echo  "%BLD_TERRAIN%" non e' un dataset: manca manifest.json.
echo  Serve la cartella del dataset del TERRENO (quella fatta con costruisci-dted
echo  o con geoworld build), non quella dei file .dt2.
set "BLD_EXIT=1"
goto :end

:not_terrain
echo.
echo  "%BLD_TERRAIN%" e' un dataset, ma non del terreno (ortofoto, strade, acqua,
echo  vegetazione o edifici). Serve quello del TERRENO: gli edifici prendono la sua area.
set "BLD_EXIT=1"
goto :end

:no_osm
echo.
echo  Nessun estratto OpenStreetMap. Scaricane uno (.osm.pbf) da
echo  https://download.geofabrik.de, mettilo in "%BLD_LOCAL%" e rilancia.
set "BLD_EXIT=1"
goto :end

:no_osm_file
echo.
echo  Non trovo "%BLD_OSM%", ne' come file ne' dentro Pipeline\dati\osm.
set "BLD_EXIT=1"
goto :end

:fetch_failed
echo.
echo  [costruisci-edifici] Lo scaricamento si e' fermato: l'errore e' qui sopra.
echo  [costruisci-edifici] Senza rete: scarica il .osm.pbf con il browser da https://download.geofabrik.de
echo  [costruisci-edifici] (Europe, Italy), mettilo in "%BLD_LOCAL%" e rilancia: lo usa da solo.
set "BLD_EXIT=1"
goto :end

:build_failed
echo.
echo  [costruisci-edifici] Il build si e' fermato: l'errore e' qui sopra.
echo  [costruisci-edifici] Rilancia pure (riprende da dove era); se si ripete, mandami le righe sopra.
set "BLD_EXIT=1"
goto :end

:verify_failed
echo.
echo  [costruisci-edifici] Il dataset c'e', ma la verifica ha trovato problemi (qui sopra).
echo  [costruisci-edifici] Mandami le righe sopra prima di usarlo.
set "BLD_EXIT=1"
goto :end

:end
echo.
pause
endlocal & exit /b %BLD_EXIT%
