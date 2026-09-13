#!/usr/bin/env bash
# =============================================================================
#  Controlli strutturali sul sorgente di GeoWorld.
#
#  Verifica due regole di progetto che nessun compilatore puo' imporre:
#
#   REGOLA 1 - Lo strato puro e' davvero puro.
#     Niente include di Unreal Engine sotto Public/Geo e Private/Geo.
#
#   REGOLA 2 - La conversione metri <-> unita' Unreal avviene in un punto solo.
#     Il fattore 100 puo' comparire solo in GeoUnits.h (dove e' definito) e
#     dentro FGeoreference (dove viene applicato).
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
PURE_VIOLATIONS=$(grep -rn -E '#include[[:space:]]*"(CoreMinimal|Engine/|UObject/|Components/|GameFramework/|Misc/|HAL/|Containers/|Math/)' \
	"$SRC/GeoCore/Public/Geo" "$SRC/GeoCore/Private/Geo" 2>/dev/null || true)

if [ -n "$PURE_VIOLATIONS" ]; then
	echo "  FALLITO - include di Unreal nello strato puro:"
	echo "$PURE_VIOLATIONS" | sed 's/^/    /'
	FAILURES=$((FAILURES + 1))
else
	echo "  ok - nessun include di Unreal sotto Public/Geo e Private/Geo"
fi

echo
echo "== Regola 2: il fattore metri->unita' vive in un punto solo =="
# Cerca il fattore 100 usato come conversione di unita', escludendo i due file
# autorizzati. Le percentuali, gli indici e i "1000.0" (km->m) non ci interessano.
UNIT_VIOLATIONS=$(grep -rn -E '(\*[[:space:]]*100\.0*[^0-9]|100\.0*[[:space:]]*\*|/[[:space:]]*100\.0*[^0-9])' \
	--include='*.h' --include='*.cpp' "$SRC" 2>/dev/null \
	| grep -v 'GeoUnits\.h' \
	| grep -v 'Georeference\.h' \
	| grep -v '//' \
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
