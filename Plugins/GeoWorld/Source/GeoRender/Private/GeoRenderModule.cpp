// =============================================================================
//  Modulo GeoRender. In Fase 1 contiene solo gli strumenti di verifica visiva;
//  dalla Fase 4 ospitera' il quadtree, la selezione LOD e la generazione mesh.
// =============================================================================
#include "Modules/ModuleManager.h"

#include "GeoMarkerActor.h"
#include "Unreal/GeoTransformComponent.h"
#include "Unreal/GeoreferenceSubsystem.h"
#include "Unreal/GeoWorldTypes.h"
#include "GeoCoreModule.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, GeoRender)

namespace GeoMarkers
{
	struct FNamedPlace
	{
		const TCHAR* Name;
		double Latitude;
		double Longitude;
		double HeightMeters;
	};

	// Punti di verifica sull'Italia. Le coordinate sono controllabili su una
	// qualunque mappa: e' proprio questo il senso del test visivo.
	static const FNamedPlace Places[] =
	{
		{ TEXT("Colosseo"),          41.890210, 12.492231,   40.0 },
		{ TEXT("Duomo di Milano"),   45.464200,  9.191900,  120.0 },
		{ TEXT("Torre di Pisa"),     43.722950, 10.396600,   20.0 },
		{ TEXT("Monte Bianco"),      45.832600,  6.865200, 4808.0 },
		{ TEXT("Etna"),              37.751000, 14.993400, 3357.0 },
	};
}

// --- geo.SpawnMarkers -------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoSpawnMarkersCommand(
	TEXT("geo.SpawnMarkers"),
	TEXT("Piazza i cubi di verifica sui punti noti dell'Italia."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		int32 SpawnedCount = 0;

		for (const GeoMarkers::FNamedPlace& Place : GeoMarkers::Places)
		{
			// NOTA UE: SpawnActor richiede i parametri di spawn se vogliamo
			// impostare proprieta' PRIMA che l'attore sia completamente
			// inizializzato. Qui ci basta il caso semplice: spawn e poi
			// configurazione, perche' UGeoTransformComponent si riposiziona da
			// solo appena gli si cambia la coordinata.
			AGeoMarkerActor* Marker = World->SpawnActor<AGeoMarkerActor>();
			if (!Marker)
			{
				continue;
			}

			Marker->Label = Place.Name;

#if WITH_EDITOR
			// NOTA UE: SetActorLabel (il nome leggibile nel World Outliner)
			// esiste SOLO nelle build con editor. Senza la guardia il progetto
			// compila nell'editor e poi si rompe al packaging.
			Marker->SetActorLabel(Place.Name);
#endif
			Marker->GetGeoTransform()->SetGeoCoordinate(
				FGeoCoordinate(Place.Latitude, Place.Longitude, Place.HeightMeters));

			++SpawnedCount;
		}

		const FString Message = FString::Printf(TEXT("[GeoWorld] %d marker piazzati."), SpawnedCount);
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Cyan, Message);
		}
	}));

// --- geo.ClearMarkers -------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoClearMarkersCommand(
	TEXT("geo.ClearMarkers"),
	TEXT("Rimuove tutti i marker di verifica."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		// NOTA UE: TActorIterator e' il modo corretto di iterare gli attori di
		// un mondo. Non si puo' distruggere mentre si itera, quindi si raccoglie
		// prima e si distrugge dopo.
		TArray<AActor*> ToDestroy;
		for (TActorIterator<AGeoMarkerActor> It(World); It; ++It)
		{
			ToDestroy.Add(*It);
		}

		for (AActor* Actor : ToDestroy)
		{
			Actor->Destroy();
		}

		UE_LOG(LogGeoWorld, Log, TEXT("[GeoWorld] %d marker rimossi."), ToDestroy.Num());
	}));
