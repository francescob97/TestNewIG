// =============================================================================
//  GeoEntitySubsystem.h -- Le entita' che arrivano da fuori, disegnate.
//  STRATO: UNREAL (Fase 7).
//
//      sorgenti (thread)  ->  coda MPSC  ->  registro  ->  attori
//
//  A OGNI FRAME, sul game thread:
//    1. si vuota la coda nel registro (Entities/EntityRegistry.h): gli
//       aggiornamenti sono gia' tradotti nella forma canonica dalle sorgenti;
//    2. ogni mezzo secondo se ne vanno quelle che tacciono da troppo;
//    3. ogni quarto di secondo si decide CHI ha un attore: le piu' vicine
//       alla camera entro il raggio, al massimo MaxActors. Il registro ha
//       tutte le entita', gli attori solo quelle che si vedono (sezione 11);
//    4. agli attori si da' la posa di adesso: estrapolata, con il rientro
//       morbido, dall'ECEF allo spazio di Unreal con l'origine di ADESSO.
//       Nessuna posizione Unreal si conserva: il rebase non tocca niente
//       (sezione 10), come per le tile.
//
//  IL SUOLO. Chi chiede di stare appoggiato al terreno (le auto, CIGI) sta
//  sul terreno DISEGNATO (UGeoPositionHudSubsystem::GroundHeightAt); se la
//  tile non c'e' ancora, con l'ultima quota nota, e si corregge quando
//  arriva (sezione 10: meglio approssimato subito che esatto fra tre frame).
//
//  LE QUOTE SUL LIVELLO DEL MARE si alzano dell'ondulazione del geoide: dal
//  dataset dell'acqua se c'e', altrimenti i 48 m dell'Italia (e lo si dice).
//
//  GLI ATTORI. Per ora forme fatte di scatole (il cubo del motore, colorato
//  per fazione) o un asset dalla tabella dei modelli. Chi non ha una regola
//  nella tabella prende la forma del suo dominio e il nome dice "?" e il
//  tipo: si vede che e' un ripiego.
//
//  I NOMI non sono testo nella scena (UTextRenderComponent e' una mesh: di
//  taglio non si legge, da lontano e' un puntino) ma scritte sul Canvas del
//  viewport, come il riquadro della posizione (UDebugDrawService): sempre
//  dritte verso la camera, sempre della stessa grandezza, anche
//  nell'editor senza Play.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/StrongObjectPtr.h"

#include "Entities/EntityModels.h"
#include "Entities/EntityRegistry.h"
#include "Sources/GeoEntitySource.h"

#include "GeoEntitySubsystem.generated.h"

class AActor;
class APlayerController;
class UCanvas;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;

USTRUCT()
struct FGeoEntityStats
{
	GENERATED_BODY()

	/** Nel registro, con un attore, con la forma di ripiego. */
	UPROPERTY() int32 Entita = 0;
	UPROPERTY() int32 Attori = 0;
	UPROPERTY() int32 Ripieghi = 0;
	/** Aggiornamenti applicati, e al secondo (media sull'ultimo secondo). */
	UPROPERTY() int64 Aggiornamenti = 0;
	UPROPERTY() float AggiornamentiAlSecondo = 0.0f;
	/** Arrivati in disordine (scartati), rientri saltati (errore oltre i 100 m). */
	UPROPERTY() int64 InRitardo = 0;
	UPROPERTY() int64 Salti = 0;
	/** Le quote sul mare corrette con i 48 m di ripiego (manca il geoide). */
	UPROPERTY() bool bGeoideDiRipiego = false;
};

UCLASS()
class GEOENTITIES_API UGeoEntitySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FEntityKey = GeoWorld::Entities::FEntityKey;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	// --- Sorgenti ----------------------------------------------------------

	/** In ascolto della memoria condivisa con questo nome (geo.Entities.Listen). */
	bool ListenSharedMemory(const FString& MemoryName, FString& OutError);
	/** Ferma tutte le sorgenti e toglie tutte le entita'. */
	void StopAll();
	bool IsListening() const { return Sources.Num() > 0; }
	/** Una riga per sorgente: nome, stato, quante entita'. */
	TArray<FString> DescribeSources() const;

	// --- Manopole ----------------------------------------------------------

	void SetBlendSeconds(double InSeconds);
	double GetBlendSeconds() const { return Registry.Settings.BlendSeconds; }
	void SetTimeoutSeconds(double InSeconds) { TimeoutSeconds = FMath::Clamp(InSeconds, 0.5, 600.0); }
	double GetTimeoutSeconds() const { return TimeoutSeconds; }
	void SetActorRadiusM(double InRadiusM) { ActorRadiusM = FMath::Clamp(InRadiusM, 100.0, 500000.0); NextSelection = 0.0; }
	double GetActorRadiusM() const { return ActorRadiusM; }
	void SetMaxActors(int32 InCount) { MaxActors = FMath::Clamp(InCount, 1, 10000); NextSelection = 0.0; }
	int32 GetMaxActors() const { return MaxActors; }
	void SetLabelsEnabled(bool bInLabels);
	bool AreLabelsEnabled() const { return bLabels; }
	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	// --- Modelli -----------------------------------------------------------

	/** Legge la tabella dei modelli (vuoto: quella di serie). Gli attori si rifanno. Errors: una riga per errore. */
	bool LoadModelTable(const FString& Path, TArray<FString>& OutErrors);
	const GeoWorld::Entities::FModelTable& GetModelTable() const { return Models; }
	const FString& GetModelTablePath() const { return ModelTablePath; }
	/** Gli asset della tabella che non si caricano. */
	const TArray<FString>& GetMissingAssets() const { return MissingAssets; }

	// --- Seguire -----------------------------------------------------------

	/** Segue un'entita' (il nome, o il numero) con la camera del Play. False se non la trova. */
	bool Follow(const FString& What, FString& OutDescription);
	void StopFollowing() { bFollowing = false; }
	bool IsFollowing() const { return bFollowing; }

	// --- Diagnostica -------------------------------------------------------

	FGeoEntityStats GetStats() const { return Stats; }
	/** Una riga per entita' (le piu' vicine per prime), per geo.Entities.List. */
	TArray<FString> DescribeEntities(int32 MaxLines) const;
	/** Per il riquadro: "7, 5 a schermo; memoria condivisa: ricevo". Vuota se non c'e' niente. */
	FString DescribeForHud() const;
	/** Chi si segue, per il riquadro. Vuota se nessuno. */
	FString DescribeFollowed() const;

private:
	/** Quello che si vede di un'entita'. */
	struct FVisual
	{
		TWeakObjectPtr<AActor> Actor;
		/** Il nome da scrivere (rifatto a ogni scelta, ogni quarto di secondo). */
		FString LabelText;
		/** La forma di ripiego (nessuna regola della tabella). */
		bool bFallback = false;
		/** Il modello, come lo dice la tabella. */
		FString ModelText;
		double ExtentM = 3.0;
		/** L'ultima quota del suolo nota, per chi sta appoggiato (sezione 10). */
		double GroundM = 0.0;
		bool bHasGround = false;
		/** La generazione della tabella dei modelli con cui e' stato fatto l'attore. */
		uint32 ModelGeneration = 0;
	};

	struct FCandidate
	{
		FEntityKey Key;
		double DistanceM = 0.0;
	};

	void DrainQueue(double Now);
	void SelectActors(double Now);
	void UpdateActors(double Now);
	void UpdateFollow(double Now);
	void DrawDebug(double Now) const;
	/** I nomi, sul Canvas di ogni viewport di questo mondo (UDebugDrawService). */
	void DrawLabels(UCanvas* Canvas, APlayerController* Controller);

	/** La posa dell'entita' nello spazio di Unreal di adesso. False se non c'e' una georeferenziazione. */
	bool ComputeTransform(const GeoWorld::Entities::FEntityRecord& Record, FVisual* Visual, double Now,
		FVector& OutLocation, FQuat& OutRotation, GeoWorld::Core::FGeodetic& OutGeodetic) const;

	AActor* SpawnActorFor(const GeoWorld::Entities::FEntityRecord& Record, FVisual& Visual);
	void DestroyVisual(const FEntityKey& Key);
	void DestroyAllVisuals();
	UMaterialInterface* MaterialFor(GeoWorld::Entities::EForce Force, bool bFallback);
	FString LabelText(const GeoWorld::Entities::FEntityRecord& Record, const FVisual& Visual) const;
	double UndulationAt(const GeoWorld::Core::FGeodetic& Where);

	TSharedPtr<FGeoEntityQueue, ESPMode::ThreadSafe> Queue;
	TArray<TUniquePtr<IGeoEntitySource>> Sources;
	uint32 NextSourceId = 1;

	GeoWorld::Entities::FEntityRegistry Registry;
	GeoWorld::Entities::FModelTable Models;
	FString ModelTablePath;
	TArray<FString> MissingAssets;
	uint32 ModelGeneration = 1;

	TMap<FEntityKey, FVisual> Visuals;
	/** La distanza di ogni entita' dalla camera all'ultima scelta (per List e il riquadro). */
	TMap<FEntityKey, double> Distances;

	TStrongObjectPtr<UStaticMesh> CubeMesh;
	TStrongObjectPtr<UMaterialInterface> BaseMaterial;
	/** Un materiale per fazione (0..3) e uno per i ripieghi (4). */
	TStrongObjectPtr<UMaterialInstanceDynamic> ForceMaterials[5];
	TMap<FString, TStrongObjectPtr<UStaticMesh>> AssetMeshes;

	double TimeoutSeconds = 5.0;
	double ActorRadiusM = 30000.0;
	int32 MaxActors = 400;
	bool bLabels = true;
	bool bShowDebugOverlay = false;

	bool bFollowing = false;
	FEntityKey FollowedKey;
	FString FollowedLabel;

	FDelegateHandle GameDrawHandle;
	FDelegateHandle EditorDrawHandle;
	uint64 LastDrawnFrame = 0;
	const void* LastDrawnView = nullptr;

	double NextExpire = 0.0;
	double NextSelection = 0.0;
	double RateWindowStart = 0.0;
	int64 RateWindowCount = 0;
	FGeoEntityStats Stats;
};
