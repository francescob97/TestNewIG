@echo off
rem =============================================================================
rem  costruisci-dted.bat -- Dai file DTED (.dt2) a un dataset del terreno.
rem
rem  Tre modi di usarlo:
rem    - doppio clic: chiede la cartella (ci si puo' trascinare dentro la
rem      cartella con il mouse) e il nome del dataset;
rem    - trascinare la cartella dei .dt2 sopra questo file;
rem    - da riga di comando:   costruisci-dted "D:\dati\dted" [nome]
rem
rem  Cerca i .dt2 anche nelle sottocartelle (dted\E016\N38.dt2...), costruisce
rem  il dataset in Pipeline\dataset\NOME con il datum verticale dei DTED
rem  (EGM96, EPSG:5773), lo verifica, e alla fine mette negli appunti il
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
set "DTED_DIR=%~1"
set "DTED_NAME=%~2"
set "DTED_INTERACTIVE="
set "DTED_EXIT=0"

if defined DTED_DIR goto :have_dir
set "DTED_INTERACTIVE=1"
echo.
echo  Costruisce un dataset del terreno dai file DTED (.dt2).
echo.
echo  Cartella con i file .dt2 (puoi trascinarla qui dentro con il mouse), poi Invio:
set /p "DTED_DIR=  > "
if not defined DTED_DIR goto :no_dir

:have_dir
rem Via le virgolette (trascinando un percorso con spazi arrivano anche quelle)
rem e la barra finale.
set "DTED_DIR=%DTED_DIR:"=%"
if not defined DTED_DIR goto :no_dir
if "%DTED_DIR:~-1%"=="\" set "DTED_DIR=%DTED_DIR:~0,-1%"
if exist "%DTED_DIR%\" goto :folder_ok
if not exist "%DTED_DIR%" goto :no_folder
rem E' un file (uno dei .dt2): si prende la sua cartella.
for %%I in ("%DTED_DIR%") do set "DTED_DIR=%%~dpI"
if "%DTED_DIR:~-1%"=="\" set "DTED_DIR=%DTED_DIR:~0,-1%"
:folder_ok

rem --- quanti .dt2 ci sono, sottocartelle comprese -----------------------------
set "DTED_COUNT=0"
for /f %%N in ('dir /s /b /a-d "%DTED_DIR%\*.dt2" 2^>nul ^| find /c /v ""') do set "DTED_COUNT=%%N"
if "%DTED_COUNT%"=="0" goto :no_files

rem --- il nome del dataset -------------------------------------------------------
if defined DTED_NAME goto :have_name
set "DTED_NAME=dted"
if not defined DTED_INTERACTIVE goto :have_name
echo.
echo  Trovati %DTED_COUNT% file .dt2 in "%DTED_DIR%" (sottocartelle comprese).
echo  Nome del dataset (Invio = dted):
set /p "DTED_NAME=  > "
:have_name
set "DTED_NAME=%DTED_NAME:"=%"
if not defined DTED_NAME set "DTED_NAME=dted"
rem Niente spazi nel nome della cartella del dataset: un problema in meno.
set "DTED_NAME=%DTED_NAME: =_%"
set "DTED_OUT=%GEO_HOME%dataset\%DTED_NAME%"

echo.
echo  [costruisci-dted] %DTED_COUNT% file .dt2 da "%DTED_DIR%"
echo  [costruisci-dted] dataset in "%DTED_OUT%"
echo  [costruisci-dted] quote sul livello del mare dei DTED: EGM96 (EPSG:5773)
if %DTED_COUNT% GTR 100 echo  [costruisci-dted] Sono tanti file: ci vorranno ore. Si puo' interrompere e rilanciare.
echo.

rem --- build e verifica (geoworld.bat prepara l'ambiente, se serve) -------------
call "%GEO_HOME%geoworld.bat" build -i "%DTED_DIR%\**\*.dt2" -o "%DTED_OUT%" --vertical-crs EPSG:5773 --name "DTED2 %DTED_NAME%" --source-description "DTED livello 2 da %DTED_DIR%"
if errorlevel 1 goto :build_failed
echo.
call "%GEO_HOME%geoworld.bat" verify -o "%DTED_OUT%"
if errorlevel 1 goto :verify_failed

rem --- il comando per Unreal, negli appunti ---------------------------------------
for %%I in ("%DTED_OUT%") do set "DTED_OUT_FULL=%%~fI"
set "DTED_UNREAL=geo.Terrain.Demo "%DTED_OUT_FULL%""
powershell -NoProfile -Command "Set-Clipboard -Value $env:DTED_UNREAL" >nul 2>&1
echo.
echo  ==========================================================================
echo   Fatto. In Unreal apri la console (tasto ~), incolla con Ctrl+V e Invio:
echo.
echo     %DTED_UNREAL%
echo.
echo   (e' gia' negli appunti). Ti mette a 6 km sopra il centro del dataset;
echo   per spostarti: geo.Goto ^<lat^> ^<lon^>
echo  ==========================================================================
goto :end

rem =============================================================================
rem  Errori
rem =============================================================================

:no_dir
echo.
echo  Nessuna cartella indicata.
set "DTED_EXIT=1"
goto :end

:no_folder
echo.
echo  Non trovo la cartella "%DTED_DIR%".
set "DTED_EXIT=1"
goto :end

:no_files
echo.
echo  In "%DTED_DIR%" (sottocartelle comprese) non ci sono file .dt2.
echo  Ecco cosa c'e' dentro:
echo.
dir /b "%DTED_DIR%"
echo.
echo  Se i file non hanno l'estensione .dt2 (per esempio si chiamano solo N_38),
echo  rinominali in N_38.dt2: GDAL li riconosce lo stesso, ma la ricerca no.
set "DTED_EXIT=1"
goto :end

:build_failed
echo.
echo  [costruisci-dted] Il build si e' fermato: l'errore e' qui sopra.
echo  [costruisci-dted] Rilancia pure: riprende da dove era. Se si ripete, mandami le righe sopra.
set "DTED_EXIT=1"
goto :end

:verify_failed
echo.
echo  [costruisci-dted] Il dataset c'e', ma la verifica ha trovato problemi (qui sopra).
echo  [costruisci-dted] Mandami le righe sopra prima di usarlo.
set "DTED_EXIT=1"
goto :end

:end
echo.
pause
endlocal & exit /b %DTED_EXIT%
