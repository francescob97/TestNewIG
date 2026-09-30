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
//                                ^           ^          ("BaseColor")
//                                | B         | RG            |
//        VectorParameter --------+--> ComponentMask          v
//        ("DrapeUv")                                    Base Color
//
//  DrapeUv vale (offsetU, offsetV, scala, scala): le UV della mesh vengono
//  moltiplicate per la scala (componente B) e traslate dell'offset (componenti
//  R e G, estratte con una ComponentMask), che e' il ritaglio calcolato in
//  ImageryMapping.h.
//
//  IL BUG DELLA PRIMA VERSIONE, E PERCHE' IL PARAMETRO HA CAMBIATO NOME
//  La prima versione collegava all'Add l'uscita "R" del parametro. Un'uscita
//  di un solo canale e' uno SCALARE, e Unreal somma uno scalare a entrambe le
//  componenti di un float2: l'offset U finiva anche in V. Per ogni tile
//  vestita con un'immagine antenata (cioe' quasi tutte, con Sentinel a 10 m)
//  meta' dei ritagli prendeva il quarto sbagliato dell'immagine: un mosaico di
//  rettangoli fuori posto. Il parametro si chiamava UvOffsetScale; ora si
//  chiama DrapeUv, cosi' il provider riconosce un materiale vecchio (non ha
//  DrapeUv) e lo dice, invece di disegnare in silenzio il mosaico sbagliato.
// =============================================================================
#include "CoreMinimal.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "GeoCoreModule.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComponentMask.h"
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

	bool SaveMaterialPackage(UPackage* Package, UMaterial* Material, bool bIsNew)
	{
		Package->MarkPackageDirty();
		if (bIsNew) { FAssetRegistryModule::AssetCreated(Material); }

		const FString FileName = FPackageName::LongPackageNameToFilename(
			MaterialPackagePath, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Arguments;
		Arguments.TopLevelFlags = RF_Public | RF_Standalone;
		Arguments.SaveFlags = SAVE_NoError;

		return UPackage::SavePackage(Package, Material, *FileName, Arguments);
	}

	/** Costruisce il grafo del materiale. Ritorna false se un nodo non si crea. */
	bool BuildDrapeGraph(UMaterial* Material)
	{
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
					Material, UMaterialExpressionVectorParameter::StaticClass(), -1100, 200));

		UMaterialExpressionComponentMask* OffsetMask =
			Cast<UMaterialExpressionComponentMask>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					Material, UMaterialExpressionComponentMask::StaticClass(), -650, 200));

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

		if (!Coordinates || !UvParameter || !OffsetMask || !Scale || !Offset || !Sampler)
		{
			return false;
		}

		UvParameter->ParameterName = TEXT("DrapeUv");
		// Valore di riposo: nessun ritaglio. Cosi' il materiale e' guardabile
		// nell'editor anche senza che nessuno gli passi dei parametri.
		UvParameter->DefaultValue = FLinearColor(0.0f, 0.0f, 1.0f, 1.0f);

		// La maschera tiene R e G: l'offset come float2, (offsetU, offsetV).
		// E' il pezzo che mancava nella prima versione.
		OffsetMask->R = true;
		OffsetMask->G = true;
		OffsetMask->B = false;
		OffsetMask->A = false;

		Sampler->ParameterName = TEXT("BaseColor");
		Sampler->SamplerType = SAMPLERTYPE_Color;

		// uv = TexCoord * DrapeUv.B + DrapeUv.RG
		//
		// La scala e' la stessa sui due assi, quindi uno scalare (l'uscita "B")
		// va bene: moltiplicato per un float2 scala entrambe le componenti, ed
		// e' proprio quello che si vuole. Per l'offset invece serve un float2
		// VERO: da qui la maschera.
		UMaterialEditingLibrary::ConnectMaterialExpressions(Coordinates, TEXT(""), Scale, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(UvParameter, TEXT("B"), Scale, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(UvParameter, TEXT(""), OffsetMask, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Scale, TEXT(""), Offset, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OffsetMask, TEXT(""), Offset, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Offset, TEXT(""), Sampler, TEXT("UVs"));

		UMaterialEditingLibrary::ConnectMaterialProperty(Sampler, TEXT(""), MP_BaseColor);

		UMaterialEditingLibrary::RecompileMaterial(Material);
		return true;
	}
}

static FAutoConsoleCommand GeoImageryCreateMaterialCommand(
	TEXT("geo.Imagery.CreateMaterial"),
	TEXT("Costruisce (o RIFA') /GeoWorld/Materials/M_GeoTerrain, il materiale del drappeggio."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		// Se esiste lo si rifa' SUL POSTO: si svuota il grafo e lo si ricostruisce.
		// Cancellarlo e crearne uno nuovo lascerebbe riferimenti rotti nelle
		// istanze dinamiche gia' create, e costringerebbe a chiudere l'editor.
		UMaterial* Material = LoadObject<UMaterial>(nullptr, MaterialPackagePath);
		const bool bIsNew = (Material == nullptr);

		UPackage* Package = nullptr;
		if (bIsNew)
		{
			Package = CreatePackage(MaterialPackagePath);
			if (!Package)
			{
				Report(TEXT("Non riesco a creare il package. Fallo a mano: docs/fase6-verifica.md"), FColor::Red);
				return;
			}
			Material = NewObject<UMaterial>(Package, MaterialAssetName, RF_Public | RF_Standalone);
			if (!Material)
			{
				Report(TEXT("Non riesco a creare il materiale. Fallo a mano: docs/fase6-verifica.md"), FColor::Red);
				return;
			}
		}
		else
		{
			Package = Material->GetOutermost();
			UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
			Report(TEXT("M_GeoTerrain esiste: lo rifaccio con il grafo corretto."), FColor::Yellow);
		}

		if (!BuildDrapeGraph(Material))
		{
			Report(TEXT("Creazione dei nodi fallita. Fallo a mano: docs/fase6-verifica.md"), FColor::Red);
			return;
		}

		if (!SaveMaterialPackage(Package, Material, bIsNew))
		{
			Report(TEXT("Materiale costruito ma NON salvato: salvalo tu dal Content Browser."),
				FColor::Yellow);
			return;
		}

		Report(bIsNew
			? TEXT("M_GeoTerrain creato in /GeoWorld/Materials. Rilancia geo.Imagery.Demo.")
			: TEXT("M_GeoTerrain rifatto. Riavvia il Play (o rilancia geo.Imagery.Demo)."));
	}));

#endif  // WITH_EDITOR
