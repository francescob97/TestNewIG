// =============================================================================
//  GeoTerrainMaterialFactory.cpp -- Crea M_GeoTerrain da codice.
//
//  PERCHE' ESISTE QUESTO FILE
//  Il materiale del drappeggio e' un .uasset, cioe' un file binario. Non puo'
//  nascere da codice sorgente, e chi ha scritto questo progetto non aveva
//  l'editor a disposizione per crearlo a mano. La via d'uscita e' fargli
//  costruire il materiale dall'editor stesso, una volta sola.
//
//  Se questo file non compila -- usa API di editor che non sono state provate --
//  NON e' un problema bloccante: il materiale si fa a mano in cinque nodi, e le
//  istruzioni sono in docs/fase6-verifica.md. E' per questo che sta in un file
//  suo e non dentro il modulo principale.
//
//  IL MATERIALE CHE COSTRUISCE
//
//      TextureCoordinate ---> Multiply ---> Add ---> TextureSampleParameter2D
//                                 ^          ^         ("BaseColor")
//        VectorParameter ---------+----------+              |
//        ("UvOffsetScale")                                  v
//                                                      Base Color
//
//  UvOffsetScale vale (offsetU, offsetV, scala, scala): le UV della mesh
//  vengono moltiplicate per la scala e traslate dell'offset, che e' il ritaglio
//  calcolato in ImageryMapping.h. La moltiplicazione usa le componenti BA e la
//  somma le componenti RG -- per questo il subsystem ripete la scala due volte.
// =============================================================================
#include "CoreMinimal.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "GeoCoreModule.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "MaterialEditingLibrary.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	const TCHAR* MaterialPackagePath = TEXT("/GeoWorld/Materials/M_GeoTerrain");
	const TCHAR* MaterialAssetName = TEXT("M_GeoTerrain");

	void Report(const FString& Message, const FColor& Colour = FColor::Cyan)
	{
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 12.0f, Colour, Message); }
	}

	bool SaveMaterialPackage(UPackage* Package, UMaterial* Material)
	{
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Material);

		const FString FileName = FPackageName::LongPackageNameToFilename(
			MaterialPackagePath, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Arguments;
		Arguments.TopLevelFlags = RF_Public | RF_Standalone;
		Arguments.SaveFlags = SAVE_NoError;

		return UPackage::SavePackage(Package, Material, *FileName, Arguments);
	}
}

static FAutoConsoleCommand GeoImageryCreateMaterialCommand(
	TEXT("geo.Imagery.CreateMaterial"),
	TEXT("Costruisce /GeoWorld/Materials/M_GeoTerrain, il materiale del drappeggio."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, MaterialPackagePath))
		{
			Report(TEXT("M_GeoTerrain esiste gia'. Per rifarlo, cancellalo prima."), FColor::Yellow);
			return;
		}

		UPackage* Package = CreatePackage(MaterialPackagePath);
		if (!Package)
		{
			Report(TEXT("Non riesco a creare il package. Fallo a mano: docs/fase6-verifica.md"), FColor::Red);
			return;
		}

		UMaterial* Material = NewObject<UMaterial>(
			Package, MaterialAssetName, RF_Public | RF_Standalone);
		if (!Material)
		{
			Report(TEXT("Non riesco a creare il materiale. Fallo a mano: docs/fase6-verifica.md"), FColor::Red);
			return;
		}

		// Lit, non Unlit. Unlit mostrerebbe i colori esatti dell'ortofoto senza
		// dipendere dalle luci, il che e' comodo; ma toglierebbe ogni
		// ombreggiatura, e il rilievo costruito nella Fase 5 sparirebbe
		// visivamente. Con MD_Surface la luce direzionale fa il suo lavoro.
		Material->MaterialDomain = MD_Surface;
		Material->SetShadingModel(MSM_DefaultLit);

		UMaterialExpressionTextureCoordinate* Coordinates =
			Cast<UMaterialExpressionTextureCoordinate>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					Material, UMaterialExpressionTextureCoordinate::StaticClass(), -900, 0));

		UMaterialExpressionVectorParameter* UvParameter =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					Material, UMaterialExpressionVectorParameter::StaticClass(), -900, 200));

		UMaterialExpressionMultiply* Scale = Cast<UMaterialExpressionMultiply>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionMultiply::StaticClass(), -650, 0));

		UMaterialExpressionAdd* Offset = Cast<UMaterialExpressionAdd>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionAdd::StaticClass(), -450, 0));

		UMaterialExpressionTextureSampleParameter2D* Sampler =
			Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -250, 0));

		if (!Coordinates || !UvParameter || !Scale || !Offset || !Sampler)
		{
			Report(TEXT("Creazione dei nodi fallita. Fallo a mano: docs/fase6-verifica.md"), FColor::Red);
			return;
		}

		UvParameter->ParameterName = TEXT("UvOffsetScale");
		// Valore di riposo: nessun ritaglio. Cosi' il materiale e' guardabile
		// nell'editor anche senza che nessuno gli passi dei parametri.
		UvParameter->DefaultValue = FLinearColor(0.0f, 0.0f, 1.0f, 1.0f);

		Sampler->ParameterName = TEXT("BaseColor");
		Sampler->SamplerType = SAMPLERTYPE_Color;

		// uv = TexCoord * UvOffsetScale.BA + UvOffsetScale.RG
		UMaterialEditingLibrary::ConnectMaterialExpressions(Coordinates, TEXT(""), Scale, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(UvParameter, TEXT("B"), Scale, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Scale, TEXT(""), Offset, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(UvParameter, TEXT("R"), Offset, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Offset, TEXT(""), Sampler, TEXT("UVs"));

		UMaterialEditingLibrary::ConnectMaterialProperty(Sampler, TEXT(""), MP_BaseColor);

		UMaterialEditingLibrary::RecompileMaterial(Material);

		if (!SaveMaterialPackage(Package, Material))
		{
			Report(TEXT("Materiale creato ma NON salvato: salvalo tu dal Content Browser."),
				FColor::Yellow);
			return;
		}

		Report(TEXT("M_GeoTerrain creato in /GeoWorld/Materials. Rilancia geo.Imagery.Demo."));
	}));

#endif  // WITH_EDITOR
