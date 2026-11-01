// =============================================================================
//  GeoModelLibrary.h -- I modelli letti dai file, pronti da disegnare.
//  STRATO: UNREAL.
//
//  Un modello si legge UNA volta e lo usano tutte le entita' che lo vogliono
//  (docs/modelli-design.md, sezione 5). Il lavoro e' in due tempi:
//
//    1. su un thread di lavoro (UE::Tasks): il file, i riferimenti esterni, il
//       rig (Models/*.h, puri), le texture decodificate (SGI con il lettore
//       nostro, PNG/JPEG/BMP/TGA/... con l'ImageWrapper di Unreal) e le loro
//       mipmap;
//    2. sul game thread, un modello per frame: le UTexture2D, i materiali
//       (istanze di M_GeoEntity, M_GeoEntityMasked, M_GeoEntityTranslucent), una
//       UStaticMesh per parte (bFastBuild: funziona anche nel gioco
//       impacchettato, come gli alberi).
//
//  La CHIAVE di un modello e' il file piu' le correzioni della regola (scala,
//  assi...): lo stesso file con due correzioni sono due rig diversi.
//
//  La cartella dei modelli: i percorsi relativi delle regole partono da li', e
//  le texture che il modello nomina con il percorso di un'altra macchina si
//  cercano li' per nome (un indice fatto una volta, sul thread di lavoro).
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"

#include "Models/ModelFiles.h"
#include "Models/ModelRig.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class IImageWrapperModule;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;

enum class EGeoModelState : uint8 { Loading, Ready, Failed };

struct FGeoLoadedModel
{
	EGeoModelState State = EGeoModelState::Loading;
	FString Key;
	FString Path;
	FString Error;
	TSharedPtr<const GeoWorld::Models::FModelRig, ESPMode::ThreadSafe> Rig;
	/** Una mesh per parte del rig (vuota se la parte non ha geometria). */
	TArray<TStrongObjectPtr<UStaticMesh>> PartMeshes;
	/** Un materiale per materiale del rig. */
	TArray<TStrongObjectPtr<UMaterialInterface>> Materials;
	TArray<TStrongObjectPtr<UTexture2D>> Textures;
	TArray<FString> Warnings;
	int32 MissingTextures = 0;
	double LoadSeconds = 0.0;
};

class FGeoModelLibrary
{
public:
	FGeoModelLibrary();
	~FGeoModelLibrary();

	/** La cartella dei modelli (vuota: solo percorsi assoluti). I modelli gia' letti si buttano. */
	void SetFolder(const FString& InFolder);
	const FString& GetFolder() const { return Folder; }

	/** Il percorso di un file di una regola: relativo alla cartella, o assoluto. */
	FString ResolvePath(const FString& File) const;

	/**
	 * Il modello con questa chiave: se non c'e', comincia a leggerlo. Torna
	 * subito; lo stato dice se e' pronto.
	 */
	TSharedPtr<FGeoLoadedModel> Request(const FString& Path, const GeoWorld::Models::FRigOptions& Options, const FString& Key);

	/** Sul game thread, una volta per frame: i modelli letti diventano mesh e texture. Ritorna quanti sono pronti adesso. */
	int32 Tick();

	/** Butta tutto (i modelli in volo finiscono e si ignorano). */
	void Clear();

	void ForEach(TFunctionRef<void(const FGeoLoadedModel&)> Function) const;
	int32 NumLoading() const;

	/** Vuota se i materiali ci sono; altrimenti la frase da dire in rosso. */
	FString GetMaterialProblem() const;

	/** Le regole dei materiali scelti per un materiale del rig: 0 opaco, 1 tagliato a due facce, 2 trasparente. */
	static int32 MaterialMode(const GeoWorld::Models::FModelMaterial& Material, bool bTextureHasAlpha);

	/** I file per i lettori puri, con IFileManager (da qualsiasi thread). */
	static GeoWorld::Models::FFileAccess FileAccess();

	/** L'indice per nome dei file della cartella; lo fa adesso se non c'e' (puo' metterci un po'). Nullo senza cartella. */
	const std::unordered_map<std::string, std::string>* GetFolderIndex() const;

private:
	struct FCookedTexture
	{
		bool bOk = false;
		int32 Width = 0, Height = 0;
		std::vector<uint8_t> Bgra;
		std::vector<std::vector<uint8_t>> Mips;
		bool bHasAlpha = false;
		FString Error;
	};

	struct FCooked
	{
		FString Key;
		/** La generazione della libreria quando il lavoro e' partito (Clear la cambia). */
		uint32 Generation = 0;
		FString Error;
		TSharedPtr<const GeoWorld::Models::FModelRig, ESPMode::ThreadSafe> Rig;
		TArray<FCookedTexture> Textures;
		TArray<FString> Warnings;
		double Seconds = 0.0;
	};

	/** L'indice dei file della cartella per nome (minuscolo), fatto una volta sul thread di lavoro. */
	struct FFolderIndex
	{
		FCriticalSection Lock;
		bool bBuilt = false;
		FString Folder;
		std::unordered_map<std::string, std::string> ByName;
	};

	using FCookedQueue = TQueue<TSharedPtr<FCooked, ESPMode::ThreadSafe>, EQueueMode::Mpsc>;

	static const std::unordered_map<std::string, std::string>* BuildIndex(FFolderIndex& FolderIndex);

	static TSharedPtr<FCooked, ESPMode::ThreadSafe> Cook(const FString& Key, const FString& Path, const GeoWorld::Models::FRigOptions& Options,
		const TSharedPtr<FFolderIndex, ESPMode::ThreadSafe>& Index, IImageWrapperModule* Wrappers);
	void Finish(FCooked& Cooked);
	void EnsureMaterials();

	FString Folder;
	/**
	 * Cresce a ogni Clear: un lavoro partito prima consegna un risultato vecchio
	 * (il file com'era), che non deve finire nel modello richiesto dopo con la
	 * stessa chiave (ReloadModels dopo aver cambiato il file).
	 */
	uint32 Generation = 0;
	TMap<FString, TSharedPtr<FGeoLoadedModel>> Loaded;
	TSharedPtr<FCookedQueue, ESPMode::ThreadSafe> Completed;
	TArray<UE::Tasks::FTask> Tasks;
	TSharedPtr<FFolderIndex, ESPMode::ThreadSafe> Index;
	IImageWrapperModule* Wrappers = nullptr;

	/** M_GeoEntity, M_GeoEntityMasked, M_GeoEntityTranslucent (o il materiale di base del motore). */
	TStrongObjectPtr<UMaterialInterface> BaseMaterials[3];
	bool bMaterialsMissing = false;
	double LastMaterialLookup = -1000.0;
};
