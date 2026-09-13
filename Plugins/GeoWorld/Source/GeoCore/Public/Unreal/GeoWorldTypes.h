// =============================================================================
//  GeoWorldTypes.h -- Tipi riflessi, esposti a Blueprint e ai Details panel.
//  STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Geo/Ellipsoid.h"

#include "GeoWorldTypes.generated.h"
// NOTA UE: il .generated.h e' prodotto da UnrealHeaderTool (UHT), che fa un
// pre-passaggio sui nostri header e genera il codice di riflessione. Deve
// SEMPRE essere l'ULTIMO include del file, altrimenti UHT emette un errore
// poco chiaro. Non esiste sul disco finche' non compili la prima volta.

/**
 * Coordinata geodetica in GRADI, adatta a essere mostrata e modificata.
 *
 * Perche' un tipo separato da FGeodetic: FGeodetic sta in radianti (giusto per
 * i calcoli, illeggibile in un pannello) e non e' riflettibile (vive nello
 * strato puro, che non conosce le macro di Unreal). Questa e' la sua controparte
 * per l'interfaccia utente: gradi, nomi espliciti, tooltip.
 *
 * NOTA UE: BlueprintType la rende usabile come variabile nei Blueprint.
 * Attenzione, i Blueprint non gestiscono il tipo double nativo come il C++:
 * espongono "float" nell'editor grafico ma il valore sottostante e' a doppia
 * precisione. Per la latitudine questo conta: un float ha ~7 cifre decimali,
 * che a queste latitudini vale circa un metro. Percio' la logica geodetica
 * resta in C++ e i Blueprint servono solo a impostare valori.
 */
USTRUCT(BlueprintType)
struct GEOCORE_API FGeoCoordinate
{
	GENERATED_BODY()

	/** Latitudine in gradi, positiva a Nord. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoWorld",
		meta = (ClampMin = "-90.0", ClampMax = "90.0", Delta = "0.0001"))
	double Latitude = 0.0;

	/** Longitudine in gradi, positiva a Est. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoWorld",
		meta = (ClampMin = "-180.0", ClampMax = "180.0", Delta = "0.0001"))
	double Longitude = 0.0;

	/**
	 * Quota ELLISSOIDICA in metri (sopra l'ellissoide WGS84), non ortometrica.
	 * In Italia la differenza fra le due vale fra -50 e +55 metri circa: non e'
	 * un dettaglio, ed e' il motivo per cui la pipeline di Fase 2 dovra'
	 * convertire le quote TINITALY con la griglia EGM2008.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoWorld")
	double HeightMeters = 0.0;

	FGeoCoordinate() = default;

	FGeoCoordinate(double InLatitude, double InLongitude, double InHeightMeters)
		: Latitude(InLatitude), Longitude(InLongitude), HeightMeters(InHeightMeters) {}

	/** Conversione verso lo strato puro (gradi -> radianti). */
	GeoWorld::Core::FGeodetic ToGeodetic() const
	{
		return GeoWorld::Core::FGeodetic::FromDegrees(Latitude, Longitude, HeightMeters);
	}

	static FGeoCoordinate FromGeodetic(const GeoWorld::Core::FGeodetic& Geodetic)
	{
		return FGeoCoordinate(Geodetic.LatDeg(), Geodetic.LonDeg(), Geodetic.HeightM);
	}

	FString ToDisplayString() const
	{
		return FString::Printf(TEXT("%.6f%s, %.6f%s, %.1f m"),
			FMath::Abs(Latitude),  Latitude  >= 0.0 ? TEXT("N") : TEXT("S"),
			FMath::Abs(Longitude), Longitude >= 0.0 ? TEXT("E") : TEXT("W"),
			HeightMeters);
	}
};
