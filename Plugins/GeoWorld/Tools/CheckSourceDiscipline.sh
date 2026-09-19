#!/usr/bin/env bash
# =============================================================================
#  Controlli strutturali sul sorgente di GeoWorld.
#
#  Verifica due regole di progetto che nessun compilatore puo' imporre:
#
#   REGOLA 1 - Lo strato puro e' davvero puro.
#     Niente include di Unreal Engine negli strati puri di GeoCore (Geo/) e
#     GeoTiles (Tiles/).
#
#   REGOLA 2 - Nessun parametro nasconde un membro.
#     Unreal compila con lo shadowing come ERRORE, non come avvertimento:
#     e' un file che non compila, non uno stile discutibile. Il controllo sta
#     in CheckShadowedParameters.py.
#
#   REGOLA 3 - Nessun simbolo pubblico invisibile agli altri moduli.
#     In Unreal ogni modulo e' una DLL: un simbolo definito in un .cpp non e'
#     visibile fuori se non esportato. Lo strato puro e' quindi header-only.
#     Il controllo sta in CheckModuleExports.py.
#
#   REGOLA 4 - La conversione metri <-> unita' Unreal avviene in un punto solo.
#     Il fattore 100 puo' comparire solo in GeoUnits.h (dove e' definito) e
#     dentro FGeoreference (dove viene applicato).
#
#     Un 100 che NON e' una conversione di unita' (una percentuale, per esempio)
#     si annota con il marcatore "non-unita" sulla stessa riga. La regola resta
#     stretta e le eccezioni restano visibili con un grep, invece di allentare
#     il controllo fino a renderlo inutile.
#
#  Una regola che nessuno controlla decade in tre settimane. Questo script gira
#  in un decimo di secondo: mettilo in CI.
#
#  Uso:  ./Plugins/GeoWorld/Tools/CheckSourceDiscipline.sh
# =============================================================================
set -uo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../Source" && pwd)"
FAILURES=0

echo "== Regola 1: lo strato puro non deve conoscere Unreal =="
PURE_DIRS=(
	"$SRC/GeoCore/Public/Geo"
	"$SRC/GeoTiles/Public/Tiles"
	"$SRC/GeoRender/Public/Quadtree"
)
PURE_VIOLATIONS=$(grep -rn -E '#include[[:space:]]*"(CoreMinimal|Engine/|UObject/|Components/|GameFramework/|Misc/|HAL/|Containers/|Math/|Modules/|Subsystems/)' \
	"${PURE_DIRS[@]}" 2>/dev/null || true)

if [ -n "$PURE_VIOLATIONS" ]; then
	echo "  FALLITO - include di Unreal nello strato puro:"
	echo "$PURE_VIOLATIONS" | sed 's/^/    /'
	FAILURES=$((FAILURES + 1))
else
	echo "  ok - nessun include di Unreal negli strati puri (GeoCore, GeoTiles, Quadtree)"
fi

echo
echo "== Regola 2: nessun parametro nasconde un membro =="
if python3 "$(dirname "${BASH_SOURCE[0]}")/CheckShadowedParameters.py" "$SRC"; then
	:
else
	FAILURES=$((FAILURES + 1))
fi

echo
echo "== Regola 3: nessun simbolo invisibile agli altri moduli =="
if python3 "$(dirname "${BASH_SOURCE[0]}")/CheckModuleExports.py" "$SRC"; then
	:
else
	FAILURES=$((FAILURES + 1))
fi

echo
echo "== Regola 4: il fattore metri->unita' vive in un punto solo =="
# Cerca il fattore 100 usato come conversione di unita', escludendo i due file
# autorizzati. Le percentuali, gli indici e i "1000.0" (km->m) non ci interessano.
UNIT_VIOLATIONS=$(grep -rn -E '(\*[[:space:]]*100\.0*[^0-9]|100\.0*[[:space:]]*\*|/[[:space:]]*100\.0*[^0-9])' \
	--include='*.h' --include='*.cpp' "$SRC" 2>/dev/null \
	| grep -v 'GeoUnits\.h' \
	| grep -v 'Georeference\.h' \
	| grep -v '//' \
	| grep -v 'non-unita' \
	|| true)

if [ -n "$UNIT_VIOLATIONS" ]; then
	echo "  FALLITO - conversione di unita' fuori da GeoUnits.h / Georeference.h:"
	echo "$UNIT_VIOLATIONS" | sed 's/^/    /'
	echo "    -> usare GeoWorld::Units::MetersToUu / UuToMeters"
	FAILURES=$((FAILURES + 1))
else
	echo "  ok - nessuna conversione di unita' sparsa nel codice"
fi

echo
if [ "$FAILURES" -eq 0 ]; then
	echo "TUTTI I CONTROLLI SUPERATI"
	exit 0
else
	echo "$FAILURES CONTROLLO/I FALLITO/I"
	exit 1
fi
