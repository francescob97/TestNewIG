#!/usr/bin/env bash
# =============================================================================
#  geoworld.sh -- La pipeline di GeoWorld senza installare niente a mano.
#                 (Linux e macOS; su Windows c'e' geoworld.bat, identico.)
#
#  La PRIMA volta scarica micromamba (versione fissata, controllata con la
#  sua impronta SHA-256) e prepara in Pipeline/ambiente/ un Python con GDAL,
#  PROJ e il resto, preso da environment.yml. Le volte dopo parte subito.
#  Per ricominciare da zero basta cancellare Pipeline/ambiente/.
#
#    ./geoworld.sh                  prepara l'ambiente e lo controlla (check-env)
#    ./geoworld.sh build -i "dati/*.dt2" -o dataset/sud --vertical-crs EPSG:5773
#    ./geoworld.sh test             i test della pipeline
#    ./geoworld.sh ambiente rifai   cancella e ricrea l'ambiente
#
#  E' "python run.py ..." con l'ambiente giusto.
#
#  GEOWORLD_MICROMAMBA=/percorso/micromamba usa un micromamba gia' presente
#  invece di scaricarlo (e serve ai test di questo script).
# =============================================================================

GEO_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEO_TOOLS="$GEO_HOME/ambiente"
GEO_MAMBA="${GEOWORLD_MICROMAMBA:-$GEO_TOOLS/micromamba}"
GEO_ROOT="$GEO_TOOLS/radice"
GEO_ENV="$GEO_TOOLS/env"
GEO_SPEC="$GEO_HOME/environment.yml"
GEO_STAMP="$GEO_ENV/geoworld-environment.yml"
GEO_MAMBA_VERSION="2.3.3-0"

say() { echo "[geoworld] $*" >&2; }

# La piattaforma e l'impronta pubblicata del micromamba fissato.
platform_and_hash() {
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64)               echo "linux-64 9496f94a8b78c536573c93d946ec9bba74bd9ff79ee55aaa4b546e30db8f511b" ;;
        Linux-aarch64|Linux-arm64)  echo "linux-aarch64 0289350c2c89f7410554041e25b444cce30df326b227e70ead3e4a1e0b316286" ;;
        Darwin-x86_64)              echo "osx-64 d61b4b752be3464e7162688f568327888cd826c5ec14462e44f42816660e77fd" ;;
        Darwin-arm64)               echo "osx-arm64 aa23d0e01d6f492f43aa86720c0f4c8db91978b81f8af46a852f6c4fcf6737d5" ;;
        *) return 1 ;;
    esac
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

download_mamba() {
    local info platform expected url part
    if ! info="$(platform_and_hash)"; then
        say "ERRORE: piattaforma $(uname -s)-$(uname -m) non prevista."
        say "Installa micromamba a mano e lancia con GEOWORLD_MICROMAMBA=/percorso/micromamba."
        return 1
    fi
    platform="${info%% *}"
    expected="${info##* }"
    url="https://github.com/mamba-org/micromamba-releases/releases/download/$GEO_MAMBA_VERSION/micromamba-$platform"
    part="$GEO_MAMBA.part"
    mkdir -p "$GEO_TOOLS"
    say "Scarico micromamba $GEO_MAMBA_VERSION ($platform) ..."
    if ! curl -L --fail --silent --show-error -o "$part" "$url"; then
        say "ERRORE: non riesco a scaricare $url"
        say "Serve internet (dietro un proxy: HTTPS_PROXY)."
        rm -f "$part"
        return 1
    fi
    # L'impronta: se il file non e' quello pubblicato, non lo si esegue.
    if [ "$(sha256_of "$part")" != "$expected" ]; then
        say "ERRORE: il micromamba scaricato non ha l'impronta attesa. Non lo uso."
        rm -f "$part"
        return 1
    fi
    chmod +x "$part"
    mv -f "$part" "$GEO_MAMBA"
}

create_environment() {
    say "Preparo l'ambiente in $GEO_ENV"
    say "(la prima volta: ~1 GB da scaricare, da 5 a 15 minuti; poi non serve piu')"
    rm -rf "$GEO_ENV"
    # --no-rc: si ignora un eventuale .condarc dell'utente; l'ambiente dipende
    # solo da environment.yml.
    if ! "$GEO_MAMBA" create --no-rc --yes --root-prefix "$GEO_ROOT" --prefix "$GEO_ENV" --file "$GEO_SPEC"; then
        say "ERRORE: l'ambiente non si e' creato (vedi sopra). Rilancia: riprende dai pacchetti gia' scaricati."
        return 1
    fi
    cp -f "$GEO_SPEC" "$GEO_STAMP"
    say "Ambiente pronto."
}

ensure_mamba() {
    [ -x "$GEO_MAMBA" ] || download_mamba
}

# --- geoworld ambiente [rifai] -------------------------------------------------
if [ "${1:-}" = "ambiente" ]; then
    if [ "${2:-}" = "rifai" ]; then
        ensure_mamba && create_environment
        exit $?
    fi
    echo "Ambiente della pipeline: $GEO_ENV"
    echo "  ./geoworld.sh ambiente rifai   lo cancella e lo ricrea da environment.yml"
    echo "  Per togliere tutto: cancella $GEO_TOOLS"
    exit 0
fi

# --- 1-2. micromamba e l'ambiente (si rifa' se manca o se environment.yml e' cambiato)
ensure_mamba || exit 1
if [ ! -f "$GEO_STAMP" ] || ! cmp -s "$GEO_SPEC" "$GEO_STAMP"; then
    create_environment || exit 1
fi

# --- 3. si attiva (come "conda activate") e si lancia run.py ----------------
export CONDA_PREFIX="$GEO_ENV"
export CONDA_DEFAULT_ENV="$GEO_ENV"
export PATH="$GEO_ENV/bin:$PATH"
# Gli script di attivazione dei pacchetti: GDAL_DATA, GDAL_DRIVER_PATH, PROJ_DATA...
for script in "$GEO_ENV"/etc/conda/activate.d/*.sh; do
    # shellcheck disable=SC1090
    [ -f "$script" ] && . "$script"
done

case "${1:-}" in
    "")
        "$GEO_ENV/bin/python" "$GEO_HOME/run.py" check-env
        status=$?
        say "Pronto. Tutti i comandi della documentazione: \"./geoworld.sh\" al posto di \"python run.py\"."
        exit $status
        ;;
    test)
        cd "$GEO_HOME" && exec "$GEO_ENV/bin/python" -m unittest discover -s tests
        ;;
    *)
        exec "$GEO_ENV/bin/python" "$GEO_HOME/run.py" "$@"
        ;;
esac
