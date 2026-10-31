// =============================================================================
//  GeoVegetationModelFactory.cpp -- geo.Vegetation.CreateModels: le otto forme
//  degli alberi come static mesh NANITE, salvate in /GeoWorld/Vegetation.
//
//  PERCHE' NELL'EDITOR. I dati Nanite (i gruppi di triangoli e la loro
//  gerarchia) li costruisce solo l'editor: una mesh fatta mentre il gioco gira
//  non li ha. Le forme quindi si "cuociono" qui una volta, come i materiali
//  di geo.Imagery.CreateMaterial, e il runtime le carica dalla libreria dei
//  modelli (UGeoVegetationSettings, Project Settings -> Plugins).
//
//  LA FORMA e' il dettaglio Full di TreeMesh.h (500-3.500 triangoli, piu'
//  fitta della forma "vicina" a runtime): con Nanite non serve fare i LOD a
//  mano, il motore disegna di ogni albero solo i triangoli che si vedono.
//
//  Il materiale e' M_GeoVegetation: deve esistere prima (geo.Imagery.CreateMaterial).
// =============================================================================
#include "CoreMinimal.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GeoCoreModule.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Vegetation/GeoTreeModels.h"
#include "Vegetation/GeoVegetationSubsystem.h"
#include "Vegetation/TreeMesh.h"

namespace
{
	const TCHAR* TreeModelMaterialPath = TEXT("/GeoWorld/Materials/M_GeoVegetation.M_GeoVegetation");

	void ReportModels(const FString& Message, const FColor& Colour = FColor::Cyan)
	{
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 12.0f, Colour, Message); }
	}

	bool SaveModelPackage(UPackage* Package, UObject* Asset, bool bIsNew, const FString& PackagePath)
	{
		Package->MarkPackageDirty();
		if (bIsNew) { FAssetRegistryModule::AssetCreated(Asset); }
		const FString FileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Arguments;
		Arguments.TopLevelFlags = RF_Public | RF_Standalone;
		Arguments.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset, *FileName, Arguments);
	}

	/**
	 * Una forma come asset Nanite. Se l'asset c'e' gia' si rifa' sul posto:
	 * la libreria (Project Settings) continua a puntarlo.
	 *
	 * NOTA UE: la strada dell'editor per una static mesh fatta da codice e'
	 * "source model": la FMeshDescription si consegna al LOD 0
	 * (CreateMeshDescription + CommitMeshDescription), poi Build() fa i dati
	 * di disegno, e con NaniteSettings.bEnabled anche quelli Nanite. Normali e
	 * tangenti le diamo noi (bRecompute... spenti); niente UV per le lightmap:
	 * gli alberi sono Movable, la luce e' tutta dinamica.
	 */
	bool CreateTreeModel(int32 Species, int32 Variant, bool bFlipWinding, UMaterialInterface* Material, int32& OutTriangles)
	{
		const FString PackagePath = GeoVegetationModels::DefaultPackagePath(Species, Variant);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *GeoVegetationModels::DefaultObjectPath(Species, Variant), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		const bool bIsNew = (Mesh == nullptr);
		UPackage* Package = bIsNew ? CreatePackage(*PackagePath) : Mesh->GetOutermost();
		if (!Package)
		{
			ReportModels(FString::Printf(TEXT("%s: non riesco a creare il package."), *AssetName), FColor::Red);
			return false;
		}
		if (bIsNew) { Mesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone); }
		if (!Mesh)
		{
			ReportModels(FString::Printf(TEXT("%s: non riesco a creare la mesh."), *AssetName), FColor::Red);
			return false;
		}

		GeoWorld::Vegetation::FTreeMesh Shape;
		GeoWorld::Vegetation::BuildTreeMesh(static_cast<GeoWorld::Vegetation::ESpecies>(Species),
			GeoWorld::Vegetation::ETreeDetail::Full, static_cast<uint32>(Variant), Shape);
		OutTriangles = static_cast<int32>(Shape.TriangleCount());
		FMeshDescription Description;
		GeoVegetationModels::FillMeshDescription(Shape, bFlipWinding, Description);

		Mesh->Modify();
		Mesh->SetNumSourceModels(1);
		FStaticMeshSourceModel& Source = Mesh->GetSourceModel(0);
		Source.BuildSettings.bRecomputeNormals = false;
		Source.BuildSettings.bRecomputeTangents = false;
		Source.BuildSettings.bGenerateLightmapUVs = false;
		Source.BuildSettings.bRemoveDegenerates = false;
		Mesh->CreateMeshDescription(0, MoveTemp(Description));
		Mesh->CommitMeshDescription(0);
		Mesh->GetStaticMaterials().Reset();
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, GeoVegetationModels::SlotName(), GeoVegetationModels::SlotName()));
		Mesh->NaniteSettings.bEnabled = true;
		Mesh->Build(/*bInSilent=*/true);
		Mesh->PostEditChange();

		if (!SaveModelPackage(Package, Mesh, bIsNew, PackagePath))
		{
			ReportModels(FString::Printf(TEXT("%s costruito ma NON salvato: salvalo tu dal Content Browser."), *AssetName), FColor::Yellow);
		}
		return true;
	}
}

static FAutoConsoleCommand GeoVegetationCreateModelsCommand(
	TEXT("geo.Vegetation.CreateModels"),
	TEXT("geo.Vegetation.CreateModels [1] - salva in /GeoWorld/Vegetation le otto forme degli alberi come static mesh Nanite, la libreria di default. Con 1: facce invertite (se servi geo.Vegetation.FlipWinding 1). Prima: geo.Imagery.CreateMaterial."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TreeModelMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Material)
		{
			ReportModels(TEXT("Manca M_GeoVegetation: prima geo.Imagery.CreateMaterial, poi questo comando."), FColor::Red);
			return;
		}
		const bool bFlipWinding = Args.Num() >= 1 && FCString::Atoi(*Args[0]) != 0;

		int32 Made = 0;
		FString Counts;
		for (int32 Species = 0; Species < GeoWorld::Vegetation::SpeciesCount; ++Species)
		{
			for (int32 Variant = 0; Variant < GeoWorld::Vegetation::VariantCount; ++Variant)
			{
				int32 Triangles = 0;
				if (!CreateTreeModel(Species, Variant, bFlipWinding, Material, Triangles)) { continue; }
				++Made;
				if (Variant == 0)
				{
					Counts += FString::Printf(TEXT("%s%s %d"), Counts.IsEmpty() ? TEXT("") : TEXT(", "),
						UTF8_TO_TCHAR(GeoWorld::Vegetation::SpeciesName(static_cast<GeoWorld::Vegetation::ESpecies>(Species))), Triangles);
				}
			}
		}
		ReportModels(FString::Printf(TEXT("Modelli Nanite: %d su %d in /GeoWorld/Vegetation (triangoli: %s)%s"),
			Made, GeoWorld::Vegetation::ShapeCount, *Counts, bFlipWinding ? TEXT(", facce invertite") : TEXT("")),
			Made == GeoWorld::Vegetation::ShapeCount ? FColor::Cyan : FColor::Yellow);

		// I mondi aperti (l'editor e il Play) li prendono subito.
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* World = Context.World();
				if (UGeoVegetationSubsystem* Vegetation = World ? World->GetSubsystem<UGeoVegetationSubsystem>() : nullptr)
				{
					Vegetation->ReloadModels();
					ReportModels(FString::Printf(TEXT("Alberi: %s"), *Vegetation->GetModelsDescription()));
				}
			}
		}
	}));

#endif  // WITH_EDITOR
