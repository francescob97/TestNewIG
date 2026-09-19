// =============================================================================
//  GeoPlaces.h -- Luoghi noti d'Italia, per nome. STRATO: UNREAL (usa FString).
//
//  PERCHE' UNA TABELLA E NON DELLE COSTANTI SPARSE
//  Serviva a due cose che erano gia' nate separate: i cubi di verifica della
//  Fase 1 (geo.SpawnMarkers) e il teletrasporto (geo.Goto). Averne due copie
//  significa che prima o poi divergono. Una tabella sola, due usi.
//
//  LE COORDINATE SONO VERIFICABILI. Sono prese da una mappa qualunque e si
//  ricontrollano in dieci secondi: e' esattamente il senso di un test visivo.
//  Se il Colosseo non finisce sul Colosseo, e' la geodesia a essere sbagliata,
//  non la tabella.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Geo/GeoTypes.h"

namespace GeoWorld::Places
{
	struct FNamedPlace
	{
		const TCHAR* Name;
		double LatitudeDeg;
		double LongitudeDeg;

		/**
		 * Quota del SUOLO in metri sul livello del mare (ortometrica), come la
		 * si legge su una mappa.
		 *
		 * ATTENZIONE ALL'INVARIANTE DEL PROGETTO: ovunque nel motore le quote
		 * sono ELLISSOIDICHE, non ortometriche. Qui si conservano ortometriche
		 * di proposito, perche' la tabella deve restare controllabile a mano su
		 * una mappa; la conversione avviene in un punto solo, ToGeodetic(),
		 * sommando l'ondulazione del geoide.
		 */
		double GroundElevationM;
	};

	/**
	 * Ondulazione del geoide in Italia: quanto il geoide (il livello del mare)
	 * sta SOPRA l'ellissoide WGS84. In Italia va da circa +42 m in Sicilia a
	 * +52 m sulle Alpi; 48 m e' una media con un errore di circa 5 m.
	 *
	 * Cinque metri su una camera posta a due chilometri di quota non si vedono.
	 * Se un giorno servisse la quota vera di un punto, non si usa questo numero:
	 * si campiona la griglia EGM2008, che la pipeline della Fase 2 sa gia' usare.
	 */
	inline constexpr double ItalyGeoidUndulationM = 48.0;

	inline const FNamedPlace Table[] =
	{
		// --- Citta' --------------------------------------------------------
		{ TEXT("Roma"),            41.902780, 12.496400,   21.0 },
		{ TEXT("Milano"),          45.464200,  9.190000,  120.0 },
		{ TEXT("Torino"),          45.070300,  7.686900,  239.0 },
		{ TEXT("Napoli"),          40.851800, 14.268100,   17.0 },
		{ TEXT("Firenze"),         43.769600, 11.255800,   50.0 },
		{ TEXT("Venezia"),         45.440800, 12.315500,    1.0 },
		{ TEXT("Bologna"),         44.494900, 11.342600,   54.0 },
		{ TEXT("Genova"),          44.405600,  8.946300,   20.0 },
		{ TEXT("Palermo"),         38.115700, 13.361300,   14.0 },
		{ TEXT("Bari"),            41.117200, 16.871900,    5.0 },
		{ TEXT("Cagliari"),        39.223800,  9.121600,    6.0 },
		{ TEXT("Trieste"),         45.649600, 13.776800,    2.0 },
		{ TEXT("Trento"),          46.067100, 11.121100,  194.0 },
		{ TEXT("Perugia"),         43.110700, 12.389200,  493.0 },
		{ TEXT("Aosta"),           45.734900,  7.313500,  583.0 },

		// --- Monumenti: servono a vedere se il terreno e' al posto giusto ---
		{ TEXT("Colosseo"),        41.890210, 12.492231,   21.0 },
		{ TEXT("Duomo"),           45.464200,  9.191900,  122.0 },
		{ TEXT("Pisa"),            43.722950, 10.396600,    4.0 },
		{ TEXT("Vesuvio"),         40.821000, 14.426000, 1281.0 },

		// --- Rilievo: qui si giudica la qualita' del DEM --------------------
		{ TEXT("MonteBianco"),     45.832600,  6.865200, 4808.0 },
		{ TEXT("Cervino"),         45.976400,  7.658600, 4478.0 },
		{ TEXT("Etna"),            37.751000, 14.993400, 3357.0 },
		{ TEXT("GranSasso"),       42.468900, 13.565600, 2912.0 },
		{ TEXT("Marmolada"),       46.433900, 11.851700, 3343.0 },
		{ TEXT("Stromboli"),       38.789200, 15.213100,  924.0 },
		{ TEXT("CinqueTerre"),     44.126900,  9.712600,  100.0 },
		{ TEXT("LagoDiGarda"),     45.634500, 10.660800,   65.0 },
		{ TEXT("Dolomiti"),        46.409700, 11.885600, 2500.0 },
	};

	/** Quota di default sopra il suolo, in metri. */
	inline constexpr double DefaultAglM = 2500.0;

	/**
	 * Converte un luogo in una coordinata geodetica ELLISSOIDICA, a una certa
	 * altezza SUL SUOLO.
	 *
	 * L'altezza e' sul suolo e non sull'ellissoide di proposito: "geo.Goto
	 * MonteBianco 2000" deve mettere la camera 2 km sopra la vetta, non 2 km
	 * sopra l'ellissoide, che sarebbe 2800 m DENTRO la montagna.
	 */
	inline GeoWorld::Core::FGeodetic ToGeodetic(const FNamedPlace& Place, double AboveGroundM)
	{
		return GeoWorld::Core::FGeodetic::FromDegrees(
			Place.LatitudeDeg, Place.LongitudeDeg,
			Place.GroundElevationM + ItalyGeoidUndulationM + AboveGroundM);
	}

	/**
	 * Cerca un luogo per nome. Confronto senza distinzione di maiuscole, e con
	 * gli spazi ignorati: "monte bianco", "MonteBianco" e "Monte Bianco" sono
	 * lo stesso posto. Chi scrive in console non deve indovinare la grafia.
	 *
	 * In seconda battuta accetta un prefisso, cosi' "gar" trova "LagoDiGarda".
	 */
	inline const FNamedPlace* Find(const FString& Query)
	{
		const FString Wanted = Query.Replace(TEXT(" "), TEXT("")).ToLower();
		if (Wanted.IsEmpty()) { return nullptr; }

		for (const FNamedPlace& Place : Table)
		{
			if (FString(Place.Name).ToLower() == Wanted) { return &Place; }
		}
		for (const FNamedPlace& Place : Table)
		{
			if (FString(Place.Name).ToLower().Contains(Wanted)) { return &Place; }
		}
		return nullptr;
	}
}
