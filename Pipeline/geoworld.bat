@echo off
rem =============================================================================
rem  geoworld.bat -- La pipeline di GeoWorld senza installare niente a mano.
rem
rem  La PRIMA volta scarica micromamba (un solo .exe, ~15 MB, versione fissata
rem  e controllata con la sua impronta SHA-256) e prepara in Pipeline\ambiente\
rem  un Python con GDAL, PROJ e il resto, preso da environment.yml (~1 GB, da
rem  5 a 15 minuti secondo la rete). Le volte dopo parte subito.
rem
rem  Niente Miniforge, niente "conda activate", niente pip, niente variabili di
rem  sistema toccate: tutto sta in Pipeline\ambiente\. Per ricominciare da zero
rem  basta cancellare quella cartella.
rem
rem    geoworld                      prepara l'ambiente e lo controlla (check-env)
rem    geoworld check-env
rem    geoworld build -i "D:\dati\*.dt2" -o dataset\sud --vertical-crs EPSG:5773
rem    geoworld test                 i test della pipeline
rem    geoworld ambiente rifai       cancella e ricrea l'ambiente
rem
rem  E' "python run.py ..." con l'ambiente giusto: ogni comando della
rem  documentazione funziona scrivendo "geoworld" al posto di "python run.py".
rem
rem  NOTA. Niente blocchi fra parentesi con dentro dei percorsi: cmd li legge
rem  prima di eseguirli, e un percorso con una ")" (C:\Program Files (x86)\...)
rem  chiuderebbe il blocco a meta'. Si va avanti a etichette e goto.
rem =============================================================================
setlocal EnableExtensions

set "GEO_HOME=%~dp0"
set "GEO_TOOLS=%GEO_HOME%ambiente"
set "GEO_MAMBA=%GEO_TOOLS%\micromamba.exe"
set "GEO_ROOT=%GEO_TOOLS%\radice"
set "GEO_ENV=%GEO_TOOLS%\env"
set "GEO_SPEC=%GEO_HOME%environment.yml"
set "GEO_STAMP=%GEO_ENV%\geoworld-environment.yml"
set "GEO_MAMBA_VERSION=2.3.3-0"
set "GEO_MAMBA_SHA256=90085767d811e08a0fe240f7cb7a713915e1e5f24e3b0acfad9b37daa64c98fe"
set "GEO_MAMBA_URL=https://github.com/mamba-org/micromamba-releases/releases/download/%GEO_MAMBA_VERSION%/micromamba-win-64"
set "GEO_EXIT=0"

if /i "%~1"=="ambiente" goto :environment_command

rem --- 1. micromamba ----------------------------------------------------------
if exist "%GEO_MAMBA%" goto :have_mamba
call :download_mamba
if errorlevel 1 goto :failed
:have_mamba

rem --- 2. l'ambiente: si (ri)fa se manca o se environment.yml e' cambiato -----
if not exist "%GEO_STAMP%" goto :need_environment
fc /b "%GEO_SPEC%" "%GEO_STAMP%" >nul 2>&1
if errorlevel 1 goto :need_environment
goto :run

:need_environment
call :create_environment_now
if errorlevel 1 goto :failed

rem --- 3. si attiva (come "conda activate") e si lancia run.py -----------------
:run
set "CONDA_PREFIX=%GEO_ENV%"
set "CONDA_DEFAULT_ENV=%GEO_ENV%"
set "PATH=%GEO_ENV%;%GEO_ENV%\Library\mingw-w64\bin;%GEO_ENV%\Library\usr\bin;%GEO_ENV%\Library\bin;%GEO_ENV%\Scripts;%GEO_ENV%\bin;%PATH%"
rem Gli script di attivazione dei pacchetti: GDAL_DATA, GDAL_DRIVER_PATH,
rem PROJ_DATA... gli stessi che lancerebbe "conda activate".
for %%F in ("%GEO_ENV%\etc\conda\activate.d\*.bat") do call "%%~fF"

if "%~1"=="" goto :first_run
if /i "%~1"=="test" goto :tests
"%GEO_ENV%\python.exe" "%GEO_HOME%run.py" %*
set "GEO_EXIT=%ERRORLEVEL%"
goto :end

:first_run
"%GEO_ENV%\python.exe" "%GEO_HOME%run.py" check-env
set "GEO_EXIT=%ERRORLEVEL%"
echo.
echo [geoworld] Pronto. Da una finestra di comando, nella cartella Pipeline:
echo [geoworld]     geoworld build -i "cartella\*.dt2" -o dataset\nome --vertical-crs EPSG:5773
echo [geoworld] Tutti i comandi della documentazione: "geoworld" al posto di "python run.py".
pause
goto :end

:tests
pushd "%GEO_HOME%"
"%GEO_ENV%\python.exe" -m unittest discover -s tests
set "GEO_EXIT=%ERRORLEVEL%"
popd
goto :end

rem --- geoworld ambiente [rifai] ------------------------------------------------
:environment_command
if /i not "%~2"=="rifai" goto :environment_help
if exist "%GEO_MAMBA%" goto :rebuild_have_mamba
call :download_mamba
if errorlevel 1 goto :failed
:rebuild_have_mamba
call :create_environment_now
if errorlevel 1 goto :failed
goto :end

:environment_help
echo Ambiente della pipeline: "%GEO_ENV%"
echo   geoworld ambiente rifai     lo cancella e lo ricrea da environment.yml
echo   Per togliere tutto: cancella la cartella "%GEO_TOOLS%"
goto :end

rem =============================================================================
rem  Sottoprogrammi (si chiamano con call; tornano con exit /b)
rem =============================================================================

:download_mamba
if not exist "%GEO_TOOLS%" mkdir "%GEO_TOOLS%"
set "GEO_PART=%GEO_MAMBA%.part"
if exist "%GEO_PART%" del /q "%GEO_PART%"
echo [geoworld] Scarico micromamba %GEO_MAMBA_VERSION% ...
rem curl.exe c'e' in Windows 10 e 11; se manca o fallisce, PowerShell.
curl.exe -L --fail --silent --show-error -o "%GEO_PART%" "%GEO_MAMBA_URL%"
if not errorlevel 1 goto :check_mamba
echo [geoworld] curl non ce l'ha fatta: riprovo con PowerShell ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -UseBasicParsing -Uri $env:GEO_MAMBA_URL -OutFile $env:GEO_PART"
if errorlevel 1 goto :download_failed
:check_mamba
rem L'impronta: se il file non e' quello pubblicato (scaricamento rotto, proxy
rem che lo sostituisce), non lo si esegue.
powershell -NoProfile -ExecutionPolicy Bypass -Command "if ((Get-FileHash -Algorithm SHA256 -LiteralPath $env:GEO_PART).Hash -ne $env:GEO_MAMBA_SHA256) { exit 1 }"
if errorlevel 1 goto :checksum_failed
move /y "%GEO_PART%" "%GEO_MAMBA%" >nul
exit /b 0

:download_failed
echo [geoworld] ERRORE: non riesco a scaricare micromamba da
echo [geoworld]   %GEO_MAMBA_URL%
echo [geoworld] Serve internet (dietro un proxy aziendale: imposta HTTPS_PROXY).
exit /b 1

:checksum_failed
echo [geoworld] ERRORE: il micromamba scaricato non ha l'impronta attesa. Non lo uso.
if exist "%GEO_PART%" del /q "%GEO_PART%"
exit /b 1

:create_environment_now
echo [geoworld] Preparo l'ambiente in "%GEO_ENV%"
echo [geoworld] (la prima volta: ~1 GB da scaricare, da 5 a 15 minuti; poi non serve piu')
if exist "%GEO_ENV%" rmdir /s /q "%GEO_ENV%"
rem --no-rc: si ignora un eventuale .condarc dell'utente (canali diversi,
rem impostazioni di Miniforge): l'ambiente dipende solo da environment.yml.
"%GEO_MAMBA%" create --no-rc --yes --root-prefix "%GEO_ROOT%" --prefix "%GEO_ENV%" --file "%GEO_SPEC%"
if errorlevel 1 goto :environment_failed
copy /y "%GEO_SPEC%" "%GEO_STAMP%" >nul
echo [geoworld] Ambiente pronto.
exit /b 0

:environment_failed
echo [geoworld] ERRORE: l'ambiente non si e' creato (vedi sopra). Rilancia: riprende
echo [geoworld] dai pacchetti gia' scaricati. Dietro un proxy aziendale: HTTPS_PROXY.
exit /b 1

:failed
set "GEO_EXIT=1"
if "%~1"=="" pause

:end
endlocal & exit /b %GEO_EXIT%
