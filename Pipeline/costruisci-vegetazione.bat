@echo off
rem =============================================================================
rem  costruisci-vegetazione.bat -- La vegetazione per un dataset del terreno.
rem
rem  Tre modi di usarlo:
rem    - doppio clic: chiede il dataset del terreno (il nome di una cartella in
rem      Pipeline\dataset, o la cartella trascinata qui dentro con il mouse) e
rem      il nome del dataset di vegetazione;
rem    - trascinare la cartella del dataset del terreno sopra questo file;
rem    - da riga di comando:   costruisci-vegetazione dted [nome]
rem
rem  La copertura del suolo (ESA WorldCover, 10 m, gratis, senza account) si
rem  legge da internet, solo per l'area del terreno. Senza rete: scarica i
rem  file ESA_WorldCover_10m_2021_v200_*_Map.tif (il messaggio d'errore dice
rem  quali e da dove) e mettili in Pipeline\dati\worldcover: se li trova li',
rem  usa quelli.
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
set "VEG_TERRAIN=%~1"
set "VEG_NAME=%~2"
set "VEG_INTERACTIVE="
set "VEG_EXIT=0"
set "VEG_LOCAL=%GEO_HOME%dati\worldcover"

if defined VEG_TERRAIN goto :have_terrain
set "VEG_INTERACTIVE=1"
echo.
echo  Costruisce la vegetazione (alberi e arbusti) per un dataset del terreno.
echo.
if not exist "%GEO_HOME%dataset\" goto :ask_terrain
echo  I dataset in Pipeline\dataset:
echo.
dir /b /ad "%GEO_HOME%dataset"
echo.
:ask_terrain
echo  Il dataset del TERRENO: il nome di una cartella qui sopra, o la cartella
echo  trascinata qui dentro con il mouse. Poi Invio:
set /p "VEG_TERRAIN=  > "
if not defined VEG_TERRAIN goto :no_terrain

:have_terrain
rem Via le virgolette (trascinando un percorso con spazi arrivano anche quelle)
rem e la barra finale.
set "VEG_TERRAIN=%VEG_TERRAIN:"=%"
if not defined VEG_TERRAIN goto :no_terrain
if "%VEG_TERRAIN:~-1%"=="\" set "VEG_TERRAIN=%VEG_TERRAIN:~0,-1%"
rem Solo il nome? Allora e' una cartella di Pipeline\dataset.
if exist "%VEG_TERRAIN%\" goto :terrain_found
if exist "%GEO_HOME%dataset\%VEG_TERRAIN%\" set "VEG_TERRAIN=%GEO_HOME%dataset\%VEG_TERRAIN%"
if not exist "%VEG_TERRAIN%\" goto :no_folder
:terrain_found
for %%I in ("%VEG_TERRAIN%") do set "VEG_TERRAIN=%%~fI"
if not exist "%VEG_TERRAIN%\manifest.json" goto :no_manifest
rem I dataset del terreno non dichiarano un "datasetKind"; gli altri si'
rem (ortofoto, strade, acqua, vegetazione): sarebbe la cartella sbagliata.
findstr /c:"datasetKind" "%VEG_TERRAIN%\manifest.json" >nul 2>&1
if not errorlevel 1 goto :not_terrain

rem --- il nome del dataset di vegetazione --------------------------------------
for %%I in ("%VEG_TERRAIN%") do set "VEG_TERRAIN_NAME=%%~nxI"
if defined VEG_NAME goto :have_name
set "VEG_NAME=vegetazione_%VEG_TERRAIN_NAME%"
if not defined VEG_INTERACTIVE goto :have_name
echo.
echo  Nome del dataset di vegetazione (Invio = vegetazione_%VEG_TERRAIN_NAME%):
set /p "VEG_NAME=  > "
:have_name
set "VEG_NAME=%VEG_NAME:"=%"
if not defined VEG_NAME set "VEG_NAME=vegetazione_%VEG_TERRAIN_NAME%"
rem Niente spazi nel nome della cartella del dataset: un problema in meno.
set "VEG_NAME=%VEG_NAME: =_%"
set "VEG_OUT=%GEO_HOME%dataset\%VEG_NAME%"

rem --- da dove: i file gia' scaricati, o la rete ----------------------------------
set "VEG_SOURCE="
set "VEG_LOCAL_COUNT=0"
for /f %%N in ('dir /s /b /a-d "%VEG_LOCAL%\*.tif" 2^>nul ^| find /c /v ""') do set "VEG_LOCAL_COUNT=%%N"
if not "%VEG_LOCAL_COUNT%"=="0" set "VEG_SOURCE=--worldcover"

echo.
echo  [costruisci-vegetazione] terreno:      "%VEG_TERRAIN%"
echo  [costruisci-vegetazione] vegetazione:  "%VEG_OUT%"
if defined VEG_SOURCE echo  [costruisci-vegetazione] WorldCover:   %VEG_LOCAL_COUNT% file in "%VEG_LOCAL%"
if not defined VEG_SOURCE echo  [costruisci-vegetazione] WorldCover:   da internet, solo i pezzi che servono
echo.

rem --- build e verifica (geoworld.bat prepara l'ambiente, se serve) -------------
if defined VEG_SOURCE goto :build_local
call "%GEO_HOME%geoworld.bat" build-vegetation --terrain "%VEG_TERRAIN%" -o "%VEG_OUT%" --name "%VEG_NAME%"
if errorlevel 1 goto :build_failed
goto :verify
:build_local
call "%GEO_HOME%geoworld.bat" build-vegetation --terrain "%VEG_TERRAIN%" -o "%VEG_OUT%" --name "%VEG_NAME%" --worldcover "%VEG_LOCAL%"
if errorlevel 1 goto :build_failed
:verify
echo.
call "%GEO_HOME%geoworld.bat" verify-vegetation -o "%VEG_OUT%"
if errorlevel 1 goto :verify_failed

rem --- il comando per Unreal, negli appunti ---------------------------------------
for %%I in ("%VEG_OUT%") do set "VEG_OUT_FULL=%%~fI"
set "VEG_UNREAL=geo.Vegetation.Open "%VEG_OUT_FULL%""
powershell -NoProfile -Command "Set-Clipboard -Value $env:VEG_UNREAL" >nul 2>&1
echo.
echo  ==========================================================================
echo   Fatto. In Unreal, con il terreno gia' aperto, apri la console (tasto ~),
echo   incolla con Ctrl+V e Invio:
echo.
echo     %VEG_UNREAL%
echo.
echo   (e' gia' negli appunti). Oppure tutto in un colpo, 250 m sopra il bosco:
echo.
echo     geo.Vegetation.Demo "%VEG_TERRAIN%" - "%VEG_OUT_FULL%"
echo.
echo   (al posto di - la cartella delle ortofoto, se ce l'hai).
echo   La prima volta, nell'editor: geo.Imagery.CreateMaterial e poi
echo   geo.Vegetation.CreateModels (i modelli Nanite degli alberi).
echo  ==========================================================================
goto :end

rem =============================================================================
rem  Errori
rem =============================================================================

:no_terrain
echo.
echo  Nessun dataset indicato.
set "VEG_EXIT=1"
goto :end

:no_folder
echo.
echo  Non trovo "%VEG_TERRAIN%", ne' come cartella ne' dentro Pipeline\dataset.
set "VEG_EXIT=1"
goto :end

:no_manifest
echo.
echo  "%VEG_TERRAIN%" non e' un dataset: manca manifest.json.
echo  Serve la cartella del dataset del TERRENO (quella fatta con costruisci-dted
echo  o con geoworld build), non quella dei file .dt2.
set "VEG_EXIT=1"
goto :end

:not_terrain
echo.
echo  "%VEG_TERRAIN%" e' un dataset, ma non del terreno (ortofoto, strade, acqua
echo  o vegetazione). Serve quello del TERRENO: la vegetazione prende la sua area.
set "VEG_EXIT=1"
goto :end

:build_failed
echo.
echo  [costruisci-vegetazione] Il build si e' fermato: l'errore e' qui sopra.
echo  [costruisci-vegetazione] Se dice "nessun file WorldCover raggiungibile", la rete non arriva al
echo  [costruisci-vegetazione] server: scarica con il browser i file elencati, mettili in
echo  [costruisci-vegetazione]   "%VEG_LOCAL%"
echo  [costruisci-vegetazione] e rilancia: li usa da soli.
echo  [costruisci-vegetazione] Altrimenti rilancia pure (riprende da dove era); se si ripete, mandami le righe sopra.
set "VEG_EXIT=1"
goto :end

:verify_failed
echo.
echo  [costruisci-vegetazione] Il dataset c'e', ma la verifica ha trovato problemi (qui sopra).
echo  [costruisci-vegetazione] Mandami le righe sopra prima di usarlo.
set "VEG_EXIT=1"
goto :end

:end
echo.
pause
endlocal & exit /b %VEG_EXIT%
