// =============================================================================
//  GeoRoadsSubsystem.h -- Disegna strade, ferrovie e piste sul terreno (Fase 8).
//  STRATO: UNREAL.
//
//  E' il gemello del subsystem delle ortofoto, e sta in mezzo alle stesse cose:
//    - UGeoTerrainSubsystem, che sa quali tile di terreno esistono;
//    - UGeoVectorStreamingSubsystem, che carica le tile di linee dal disco;
//    - IGeoTerrainMeshProvider, che mette una texture sulla tile.
//
//  LA DIFFERENZA CON LE ORTOFOTO
//  Una foto esiste su disco gia' pronta: si carica e si usa. Le strade invece
//  si DISEGNANO, una volta per ogni tile di terreno, alla risoluzione che quella
//  tile richiede (Roads/RoadRasterizer.h). Il disegno gira sui thread del task
//  graph; qui si decide cosa disegnare per primo, si consegnano le texture e si
//  tiene il conto della memoria video.
//
//  TRE REGOLE, LE STESSE CHE HANNO TOLTO I FLASH ALLE ORTOFOTO
//  1. Una tile compare solo con le sue strade (predicato di vestizione).
//  2. Se le sue non sono pronte, prende quelle di un antenato, ritagliate.
//  3. Si lavora prima per cio' che si vede, poi per cio' che si sta per
//     vedere, poi per il resto.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"

#include "Roads/RoadRasterizer.h"
#include "Tiles/TileKey.h"

#include <cstdint>
#include <vector>

#include "GeoRoadsSubsystem.generated.h"

class UGeoTerrainSubsystem;
class UGeoVectorStreamingSubsystem;
class UTexture2D;

USTRUCT()
struct FGeoRoadsStats
{
	GENERATED_BODY()

	/** Tile con le proprie strade addosso. */
	UPROPERTY() int32 TileConStrade = 0;
	/** Tile con le strade di un antenato, in attesa delle proprie. */
	UPROPERTY() int32 TileConStradeDiAntenato = 0;
	/** Tile dove non c'e' niente da disegnare (nessuna linea, o livello troppo alto). */
	UPROPERTY() int32 TileSenzaStrade = 0;
	/** Tile che aspettano: la tile di linee dal disco o il disegno. */
	UPROPERTY() int32 TileInAttesa = 0;
	/** Tile rimaste senza strade perche' la memoria video per le strade e' finita. */
	UPROPERTY() int32 TileFuoriBudget = 0;

	UPROPERTY() int32 DisegniInCorso = 0;
	UPROPERTY() int32 DisegniQuestoFrame = 0;
	UPROPERTY() int32 TextureCreateQuestoFrame = 0;
	UPROPERTY() int32 TextureInMemoria = 0;
	UPROPERTY() float MemoriaVideoMB = 0.0f;
	UPROPERTY() float BudgetVideoMB = 0.0f;
	UPROPERTY() float MillisecondiPerDisegno = 0.0f;
};

UCLASS()
class GEORENDER_API UGeoRoadsSubsystem : public UTickableWorldSubsystem
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
	bool IsEnabled() const { return bRoadsEnabled; }

	/** Realistico (default) o "mappa": colori da carta stradale, per controllare l'allineamento. */
	void SetStyle(GeoWorld::Roads::ERoadStyle InStyle);
	GeoWorld::Roads::ERoadStyle GetStyle() const { return StyleKind; }

	/**
	 * Lato in pixel dell'immagine delle strade: `Base` per le tile normali,
	 * `Finest` per quelle al livello piu' profondo del terreno, cioe' le piu'
	 * vicine quando si vola bassi. Potenze di due fra 64 e 2048.
	 */
	void SetResolution(int32 Base, int32 Finest);
	int32 GetBaseResolution() const { return BaseResolution; }
	int32 GetFinestResolution() const { return FinestResolution; }

	/** Memoria video massima per le texture delle strade. */
	void SetVideoBudgetMB(int32 Megabytes);
	int32 GetVideoBudgetMB() const { return VideoBudgetMB; }

	/** Quanto si vedono: 0 = per niente, 1 = come disegnate. */
	void SetStrength(float InStrength);
	float GetStrength() const { return Strength; }

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	FGeoRoadsStats GetStats() const { return Stats; }

	/** Butta tutte le texture e i disegni in corso: si ridisegna tutto. */
	void InvalidateAll();

private:
	void Synchronise();
	void DrainFinishedJobs();
	bool LaunchJob(const FTileKey& TerrainKey, uint32 VectorLevel, int32 Size);
	int32 ResolutionFor(const FTileKey& TerrainKey) const;
	bool IsDressedOrUndressable(const FTileKey& Key) const;
	void RemoveAllOverlays();
	void DrawDebugOverlay();

	UPROPERTY(Transient)
	TObjectPtr<UGeoVectorStreamingSubsystem> Streaming = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UGeoTerrainSubsystem> Terrain = nullptr;

	/** Il disegno di una tile, come lo consegna il worker. */
	struct FRasterResult
	{
		FTileKey Key;
		int32 Generation = 0;
		int32 Size = 0;
		bool bAnything = false;
		std::vector<uint8_t> Pixels;
		std::vector<std::vector<uint8_t>> Mips;
		double Seconds = 0.0;
	};

	/** Coda condivisa con i worker: sopravvive al subsystem se un lavoro e' ancora in corso. */
	struct FRasterQueue
	{
		TQueue<FRasterResult, EQueueMode::Mpsc> Completed;
	};
	TSharedPtr<FRasterQueue, ESPMode::ThreadSafe> RasterQueue;

	/** Cosa sa il subsystem di una tile di terreno. */
	struct FOverlay
	{
		/** Nullo se il disegno era vuoto: niente strade qui, e va bene cosi'. */
		TStrongObjectPtr<UTexture2D> Texture;
		int32 Size = 0;
		/** La generazione del disegno: se e' vecchia, si ridisegna (tenendo questo addosso). */
		int32 Generation = 0;
	};

	/** Disegni finiti, per chiave di terreno. Senza texture = tile senza strade. */
	TMap<uint64, FOverlay> Overlays;

	/** Disegni finiti ma non ancora trasformati in texture (budget del frame). */
	TMap<uint64, TSharedPtr<FRasterResult>> PendingUploads;

	/** Disegni in corso, per chiave di terreno. */
	TMap<uint64, UE::Tasks::FTask> InFlight;

	/** Lavori di una generazione scaduta: non servono, ma vanno attesi alla chiusura. */
	TArray<UE::Tasks::FTask> Orphans;

	/** Le tile vestite all'ultimo passaggio (proprie, di un antenato, o senza strade). */
	TSet<uint64> Dressed;

	/** Cresce quando cambiano stile, risoluzione o dataset: i disegni vecchi si buttano. */
	int32 Generation = 0;

	bool bRoadsEnabled = false;
	bool bShowDebugOverlay = false;

	GeoWorld::Roads::ERoadStyle StyleKind = GeoWorld::Roads::ERoadStyle::Realistic;
	GeoWorld::Roads::FRoadStyle Style;

	int32 BaseResolution = 256;
	int32 FinestResolution = 512;
	int32 VideoBudgetMB = 384;
	float Strength = 1.0f;

	int32 MaxJobsInFlight = 4;
	int32 MaxTexturesPerFrame = 8;
	int32 WarmupTexturesPerFrame = 32;

	/** Il livello piu' profondo del dataset di quote: le sue tile hanno FinestResolution. */
	int32 DeepestTerrainLevel = MAX_int32;

	int32 TeleportsSeen = 0;
	int32 CompletedJobs = 0;
	double TotalJobSeconds = 0.0;

	FGeoRoadsStats Stats;
};
