// =============================================================================
//  GeoStreetLightsSubsystem.h -- I lampioni delle strade. STRATO: UNREAL.
//
//  Sta in mezzo a:
//    - UGeoVectorStreamingSubsystem, che carica le tile delle strade (lo
//      stesso dataset di geo.Roads.Open: non serve un dataset nuovo);
//    - UGeoTerrainSubsystem, da cui prende le tile VISIBILI: i pali stanno sul
//      terreno disegnato, come gli alberi;
//    - UGeoSkySubsystem, da cui legge l'ora, il sole e la luce del livello
//      (quando si accendono, quanta luce fanno); UGeoWeatherSubsystem per le
//      nuvole (con il cielo coperto si accendono prima).
//
//  TRE COSE A SCHERMO, dalla piu' lontana:
//    1. I PUNTI LUCE: un quadratino per lampione, aperto verso la camera dal
//       materiale (M_GeoStreetLight, Roads/StreetLightShader.inl), mai piu'
//       piccolo di un paio di pixel. Fino a 8 km: la citta' vista dall'alto,
//       di notte, fatta di file di puntini.
//    2. I PALI: le tre forme di Roads/StreetLights.h (sbraccio, palo,
//       lanterna) con il vetro acceso (M_GeoStreetLamp), fino a 700 m.
//    3. LE LUCI VERE: qualche decina di spot di Unreal, verso il basso, sui
//      lampioni accesi piu' vicini alla camera. Fanno le pozze di luce
//      sull'asfalto, sui muri, sugli alberi. Entrano ed escono sfumando.
//
//  IL LAVORO, OGNI QUARTO DI SECONDO (come la vegetazione):
//    1. le celle (livello 14, ~1,2 km) entro il raggio, dalla piu' vicina;
//    2. per ogni cella la tile delle strade e le tile di terreno visibili:
//       la loro FIRMA dice se la cella e' da rifare;
//    3. sui worker: dove sono i lampioni (PlaceTileLamps) e la loro quota
//       (BuildCellLamps);
//    4. sul game thread, poche celle per frame: i componenti di istanze.
//
//  ACCESI O SPENTI lo decide il materiale, con la luce che c'e' scritta in
//  MPC_GeoWeather (StreetLights): niente da rifare al tramonto.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"

#include "Georeference/GeoreferenceSnapshot.h"
#include "Mesh/GroundCells.h"
#include "Roads/StreetLights.h"
#include "Tiles/TileKey.h"

#include <vector>

#include "GeoStreetLightsSubsystem.generated.h"

class AActor;
class UGeoTerrainSubsystem;
class UGeoVectorStreamingSubsystem;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class UMaterialParameterCollection;
class USpotLightComponent;
class UStaticMesh;

USTRUCT()
struct FGeoStreetLightsStats
{
	GENERATED_BODY()

	/** Celle con i pali (vicine) e con i soli punti luce. */
	UPROPERTY() int32 CelleVicine = 0;
	UPROPERTY() int32 CelleLontane = 0;
	/** Celle senza lampioni (campagna, o fuori dal dataset). */
	UPROPERTY() int32 CelleVuote = 0;
	/** Celle che aspettano: la tile delle strade, il terreno, il worker. */
	UPROPERTY() int32 CelleInAttesa = 0;
	UPROPERTY() int32 CostruzioniInCorso = 0;
	/** I punti luce a schermo (uno per lampione). */
	UPROPERTY() int32 Lampioni = 0;
	/** I pali a schermo (solo le celle vicine). */
	UPROPERTY() int32 Pali = 0;
	/** Le luci vere accese adesso. */
	UPROPERTY() int32 LuciVere = 0;
	UPROPERTY() int32 Componenti = 0;
	UPROPERTY() float MillisecondiPerCella = 0.0f;
};

UCLASS()
class GEORENDER_API UGeoStreetLightsSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FTileKey = GeoWorld::Tiles::FTileKey;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	/** Da chiamare dopo aver aperto (o chiuso) il dataset delle strade: tutto si rifa' da capo. */
	void OnDatasetChanged();

	/** Fin dove ci sono i punti luce, m (default 8000). */
	void SetRadiusM(double InRadiusM);
	double GetRadiusM() const { return Bands.MidM; }
	/** Fin dove ci sono i pali, m (default 700). */
	void SetPoleRadiusM(double InRadiusM);
	double GetPoleRadiusM() const { return Bands.NearM; }

	/** Quante luci vere (spot) sui lampioni piu' vicini: 0 nessuna, default 48. */
	void SetLightCount(int32 InCount);
	int32 GetLightCount() const { return LightCount; }
	/** Le luci vere fanno ombra (costa: una mappa d'ombra per luce). */
	void SetShadows(bool bInShadows);
	bool AreShadowsEnabled() const { return bShadows; }

	/** Quanta luce: 1 = 1/50 del sole del livello al piede di un palo. */
	void SetBrightness(double InBrightness) { Brightness = FMath::Clamp(InBrightness, 0.0, 20.0); }
	double GetBrightness() const { return Brightness; }
	/** La riduzione del flusso nel cuore della notte, 0..1 (default 0,3). */
	void SetDimming(double InDepth) { DimmingDepth = FMath::Clamp(InDepth, 0.0, 1.0); }
	double GetDimming() const { return DimmingDepth; }

	/** -1 la fotocellula (default), 0 sempre spenti, 1 sempre accesi (per guardarli di giorno). */
	void SetForce(int32 InForce) { Force = FMath::Clamp(InForce, -1, 1); }
	int32 GetForce() const { return Force; }

	/** Le regole di Roads/StreetLights.h (densita', passo, LED): rifa' le celle. */
	void SetPlacement(const GeoWorld::StreetLights::FStreetLightSettings& InSettings);
	const GeoWorld::StreetLights::FStreetLightSettings& GetPlacement() const { return Placement; }

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	FGeoStreetLightsStats GetStats() const { return Stats; }

	/** La luce che vede una fotocellula adesso, lux (quella vera, non della scena). */
	double GetAmbientLux() const { return AmbientLux; }
	/** Quanto e' acceso un lampione con la soglia media (30 lux), 0..1. */
	double GetOnLevel() const;

	/** Per le stampe: "accesi, 1234 punti luce, 48 luci vere", "spenti (c'e' luce: 4000 lux)". Vuota senza dataset. */
	FString Describe() const;

	/** Vuota se i materiali ci sono; altrimenti la frase da dire in rosso. */
	FString GetMaterialProblem() const;

private:
	using FInstances = std::vector<GeoWorld::StreetLights::FLampInstance>;

	/** Il lavoro di una cella, dal worker al game thread. */
	struct FCellResult
	{
		FTileKey Key;
		int32 Generation = 0;
		uint64 Signature = 0;
		TSharedPtr<const FInstances, ESPMode::ThreadSafe> Instances;
		double Seconds = 0.0;
	};
	struct FCellQueue
	{
		TQueue<FCellResult, EQueueMode::Mpsc> Completed;
	};

	struct FCell
	{
		FTileKey Key;
		GeoWorld::Core::FGeodetic Origin;
		uint64 Signature = 0;
		/** La banda dei componenti a schermo (-1: nessuno). 0 pali e punti, 1 solo punti. */
		int32 Band = -1;
		int32 WantedBand = GeoWorld::Cells::BandOutside;
		bool bEmpty = false;
		TSharedPtr<const FInstances, ESPMode::ThreadSafe> Instances;
		TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> Sprites;
		TArray<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Poles;
		/** Dal frame della cella (m) al mondo (cm): per le luci vere. */
		FTransform WorldTransform = FTransform::Identity;
		double DistanceM = 0.0;
		/** Quando e' stata costruita l'ultima volta (le lontane non si rifanno a ogni affinamento del terreno). */
		double BuiltAt = -1000.0;
		/** La quota media del suolo, per l'overlay. */
		double GroundM = 0.0;
	};

	struct FVisibleTile
	{
		FTileKey Key;
		GeoWorld::Tiles::FTileBounds Bounds;
	};

	/** Una luce vera, presa in prestito da un lampione. */
	struct FRealLight
	{
		TWeakObjectPtr<USpotLightComponent> Light;
		/** Il lampione (FLampInstance::Id); 0 = libera. */
		uint64 Id = 0;
		/** Il frame della cella e la luce in quel frame (m): il rebase la rimette a posto. */
		GeoWorld::Core::FGeodetic Origin;
		FVector Local = FVector::ZeroVector;
		float HeightM = 7.5f;
		float Output = 1.0f;
		float SwitchLux = 30.0f;
		FLinearColor Colour = FLinearColor::White;
		/** Quanto e' accesa adesso, e quanto deve esserlo (sfuma verso Target). */
		float Weight = 0.0f;
		float Target = 0.0f;
	};

	void Synchronise();
	uint64 GatherGround(const GeoWorld::Tiles::FTileBounds& Bounds, GeoWorld::Cells::FGroundSurfaces* OutGround) const;
	FTileKey SourceKeyFor(const FTileKey& Cell) const;
	bool LaunchCell(FCell& Cell, uint64 Signature);
	void DrainFinished(int32 Budget);
	void ApplyCell(FCell& Cell);
	void DestroyComponents(FCell& Cell);
	void RemoveAllCells();
	void EnsureContainer();
	void CheckAssets();
	bool EnsureMeshes();
	/** La luce che c'e', la mattina o la sera, la riduzione: in MPC_GeoWeather. */
	void UpdateLightLevel();
	double CameraAboveGround(double LatDeg, double LonDeg, double HeightM) const;
	/** L'angolo di un pixel, radianti: dal campo visivo e dalla larghezza della finestra. */
	double PixelAngle() const;
	void ChooseRealLights();
	void UpdateRealLights(float DeltaTime);
	void PlaceRealLight(FRealLight& Slot, const FGeoreferenceSnapshot& Snapshot) const;
	void RemoveRealLights();
	void UpdateSpriteVisibility();
	void OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot);
	void DrawDebugOverlay() const;

	UPROPERTY(Transient) TObjectPtr<UGeoTerrainSubsystem> Terrain;
	UPROPERTY(Transient) TObjectPtr<UGeoVectorStreamingSubsystem> Streaming;

	/** L'attore che tiene i componenti (World Outliner: "GeoWorld Lampioni"). */
	TWeakObjectPtr<AActor> Container;
	TStrongObjectPtr<UStaticMesh> SpriteMesh;
	TStrongObjectPtr<UStaticMesh> PoleMeshes[GeoWorld::StreetLights::LampKindCount];
	TStrongObjectPtr<UMaterialInterface> SpriteMaterial;
	TStrongObjectPtr<UMaterialInterface> PoleMaterial;
	TStrongObjectPtr<UMaterialParameterCollection> WeatherCollection;
	bool bPoleMaterialIsFallback = false;
	bool bParametersMissing = false;
	double LastAssetLookup = -100.0;

	TSharedPtr<FCellQueue, ESPMode::ThreadSafe> Queue;
	TMap<uint64, UE::Tasks::FTask> InFlight;
	TArray<FCellResult> PendingResults;
	TMap<uint64, FCell> LampCells;
	TArray<FVisibleTile> VisibleTiles;
	TArray<FRealLight> RealLights;
	int32 MeshStep = 1;
	int32 Generation = 0;

	GeoWorld::StreetLights::FStreetLightSettings Placement;
	GeoWorld::Cells::FBandSettings Bands;
	bool bEnabled = true;
	bool bShadows = false;
	int32 LightCount = 48;
	double Brightness = 1.0;
	double DimmingDepth = 0.3;
	int32 Force = -1;
	bool bShowDebugOverlay = false;

	// La luce di adesso (UpdateLightLevel).
	double AmbientLux = 100000.0;
	bool bMorning = false;
	double Dimming = 1.0;
	double FootLux = 0.0;
	double LastMorningCheck = -100.0;
	bool bSpritesVisible = true;

	double LastSynchronise = -100.0;
	double LastLightChoice = -100.0;
	double CameraLatDeg = 0.0, CameraLonDeg = 0.0, CameraAboveGroundM = 0.0;
	bool bHaveCamera = false;
	FDelegateHandle RebaseHandle;

	int32 MaxJobsInFlight = 4;
	int32 MaxAppliesPerFrame = 2;
	int32 CompletedJobs = 0;
	double TotalJobSeconds = 0.0;
	FGeoStreetLightsStats Stats;
};
