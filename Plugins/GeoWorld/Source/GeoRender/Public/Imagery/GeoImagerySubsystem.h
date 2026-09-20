// =============================================================================
//  GeoImagerySubsystem.h -- Veste il terreno con le ortofoto. STRATO: UNREAL.
//
//  Sta in mezzo fra tre cose che non si conoscono fra loro:
//    - UGeoTerrainSubsystem, che sa quali tile hanno geometria;
//    - UGeoImageryStreamingSubsystem, che sa quali immagini sono in memoria;
//    - IGeoTerrainMeshProvider, che sa come mettere una texture su una tile.
//
//  Nessuno dei tre e' stato modificato per ospitare gli altri due: il terreno
//  ha guadagnato un accessore alle proprie chiavi, il provider un metodo. Tutta
//  la logica del drappeggio e' qui.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/StrongObjectPtr.h"

#include "Imagery/ImageryMapping.h"
#include "Tiles/TileKey.h"

#include "GeoImagerySubsystem.generated.h"

class UGeoImageryStreamingSubsystem;
class UGeoTerrainSubsystem;
class UTexture2D;

USTRUCT()
struct FGeoImageryStats
{
	GENERATED_BODY()

	UPROPERTY() int32 TileVestite = 0;
	UPROPERTY() int32 TileSenzaImmagine = 0;
	UPROPERTY() int32 TextureInMemoria = 0;
	UPROPERTY() float MemoriaTextureMB = 0.0f;
	UPROPERTY() int32 CreateQuestoFrame = 0;
	UPROPERTY() int32 RichiesteQuestoFrame = 0;

	/** Quante tile stanno usando un'immagine piu' grossolana del proprio livello. */
	UPROPERTY() int32 TileConImmagineGrossolana = 0;
	UPROPERTY() int32 LivelloImmagineMin = 0;
	UPROPERTY() int32 LivelloImmagineMax = 0;
};

UCLASS()
class GEORENDER_API UGeoImagerySubsystem : public UTickableWorldSubsystem
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
	bool IsEnabled() const { return bDrapeEnabled; }

	/**
	 * Sostituisce le ortofoto con una scacchiera generata sul momento.
	 *
	 * Non e' un giocattolo: su una scacchiera un disallineamento di mezzo pixel
	 * al bordo di una tile si vede a occhio nudo, su una foto di un bosco non lo
	 * nota nessuno. E' lo strumento con cui si verificano le UV.
	 */
	void SetCheckerboard(bool bInChecker);
	bool IsCheckerboard() const { return bCheckerboard; }

	void SetMaxTexturesPerFrame(int32 InMax) { MaxTexturesPerFrame = FMath::Clamp(InMax, 1, 256); }
	int32 GetMaxTexturesPerFrame() const { return MaxTexturesPerFrame; }

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	FGeoImageryStats GetStats() const { return Stats; }

	/** Butta tutte le texture. Serve quando si cambia dataset o modalita'. */
	void ReleaseAllTextures();

private:
	void SynchroniseWithTerrain();
	UTexture2D* GetOrCreateTexture(const FTileKey& ImageKey, int32& InOutBudget);
	UTexture2D* GetCheckerboardTexture();
	void DrawDebugOverlay();

	UPROPERTY(Transient)
	TObjectPtr<UGeoImageryStreamingSubsystem> Streaming = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UGeoTerrainSubsystem> Terrain = nullptr;

	/**
	 * Texture create, per chiave della tile di IMMAGINE (non di terreno): piu'
	 * tile di terreno adiacenti possono condividere lo stesso antenato, e
	 * creare la stessa texture piu' volte sarebbe sprecare memoria video in
	 * proporzione al numero di figli.
	 */
	TMap<uint64, TStrongObjectPtr<UTexture2D>> Textures;

	TStrongObjectPtr<UTexture2D> CheckerTexture;

	bool bDrapeEnabled = false;
	bool bCheckerboard = false;
	bool bShowDebugOverlay = false;
	int32 MaxTexturesPerFrame = 4;

	FGeoImageryStats Stats;
};
