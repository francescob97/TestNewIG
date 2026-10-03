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
//  LE STRADE (FASE 8)
//  Sopra la foto si compone una seconda texture, trasparente, con le strade:
//
//      colore = foto * (1 - Overlay.A * OverlayStrength) + Overlay.RGB * OverlayStrength
//
//  Overlay e' in alfa PREMOLTIPLICATO (il colore e' gia' moltiplicato per la
//  copertura: vedi Roads/RoadRasterizer.h), quindi la composizione e' una
//  moltiplicazione e una somma, senza divisioni. OverlayUv e' il ritaglio, come
//  DrapeUv. OverlayStrength vale 0 di default: una tile a cui nessuno ha dato
//  le strade mostra la foto e basta, anche se la texture di default del
//  parametro (quella del motore) non e' trasparente.
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
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionScalarParameter.h"
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

	bool SaveMaterialPackage(UPackage* Package, UMaterial* Material, bool bIsNew, const TCHAR* PackagePath)
	{
		Package->MarkPackageDirty();
		if (bIsNew) { FAssetRegistryModule::AssetCreated(Material); }

		const FString FileName = FPackageName::LongPackageNameToFilename(
			PackagePath, FPackageName::GetAssetPackageExtension());

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

		// --- Fase 8: le strade sopra la foto ----------------------------------
		auto Create = [Material](UClass* Class, int32 X, int32 Y)
		{
			return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
		};

		UMaterialExpressionVectorParameter* OverlayUvParameter = Cast<UMaterialExpressionVectorParameter>(
			Create(UMaterialExpressionVectorParameter::StaticClass(), -1100, 500));
		UMaterialExpressionComponentMask* OverlayMask = Cast<UMaterialExpressionComponentMask>(
			Create(UMaterialExpressionComponentMask::StaticClass(), -650, 500));
		UMaterialExpressionMultiply* OverlayScale = Cast<UMaterialExpressionMultiply>(
			Create(UMaterialExpressionMultiply::StaticClass(), -650, 350));
		UMaterialExpressionAdd* OverlayOffset = Cast<UMaterialExpressionAdd>(
			Create(UMaterialExpressionAdd::StaticClass(), -450, 350));
		UMaterialExpressionTextureSampleParameter2D* OverlaySampler =
			Cast<UMaterialExpressionTextureSampleParameter2D>(
				Create(UMaterialExpressionTextureSampleParameter2D::StaticClass(), -250, 350));
		UMaterialExpressionScalarParameter* Strength = Cast<UMaterialExpressionScalarParameter>(
			Create(UMaterialExpressionScalarParameter::StaticClass(), -250, 650));
		UMaterialExpressionMultiply* AlphaTimesStrength = Cast<UMaterialExpressionMultiply>(
			Create(UMaterialExpressionMultiply::StaticClass(), 0, 500));
		UMaterialExpressionOneMinus* PhotoWeight = Cast<UMaterialExpressionOneMinus>(
			Create(UMaterialExpressionOneMinus::StaticClass(), 150, 500));
		UMaterialExpressionMultiply* PhotoPart = Cast<UMaterialExpressionMultiply>(
			Create(UMaterialExpressionMultiply::StaticClass(), 300, 0));
		UMaterialExpressionMultiply* RoadPart = Cast<UMaterialExpressionMultiply>(
			Create(UMaterialExpressionMultiply::StaticClass(), 150, 350));
		UMaterialExpressionAdd* Composite = Cast<UMaterialExpressionAdd>(
			Create(UMaterialExpressionAdd::StaticClass(), 450, 150));

		if (!OverlayUvParameter || !OverlayMask || !OverlayScale || !OverlayOffset || !OverlaySampler ||
		    !Strength || !AlphaTimesStrength || !PhotoWeight || !PhotoPart || !RoadPart || !Composite)
		{
			return false;
		}

		OverlayUvParameter->ParameterName = TEXT("OverlayUv");
		OverlayUvParameter->DefaultValue = FLinearColor(0.0f, 0.0f, 1.0f, 1.0f);
		OverlayMask->R = true;
		OverlayMask->G = true;
		OverlayMask->B = false;
		OverlayMask->A = false;

		OverlaySampler->ParameterName = TEXT("Overlay");
		// Color = sRGB: il rasterizzatore scrive il colore premoltiplicato
		// codificato sRGB, e la texture e' creata con SRGB = true.
		OverlaySampler->SamplerType = SAMPLERTYPE_Color;

		Strength->ParameterName = TEXT("OverlayStrength");
		Strength->DefaultValue = 0.0f;

		// uv strade = TexCoord * OverlayUv.B + OverlayUv.RG  (come il drappeggio)
		UMaterialEditingLibrary::ConnectMaterialExpressions(Coordinates, TEXT(""), OverlayScale, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OverlayUvParameter, TEXT("B"), OverlayScale, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OverlayUvParameter, TEXT(""), OverlayMask, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OverlayScale, TEXT(""), OverlayOffset, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OverlayMask, TEXT(""), OverlayOffset, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OverlayOffset, TEXT(""), OverlaySampler, TEXT("UVs"));

		// colore = foto * (1 - A * forza) + RGB * forza
		UMaterialEditingLibrary::ConnectMaterialExpressions(OverlaySampler, TEXT("A"), AlphaTimesStrength, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Strength, TEXT(""), AlphaTimesStrength, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(AlphaTimesStrength, TEXT(""), PhotoWeight, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sampler, TEXT(""), PhotoPart, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(PhotoWeight, TEXT(""), PhotoPart, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OverlaySampler, TEXT(""), RoadPart, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Strength, TEXT(""), RoadPart, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(PhotoPart, TEXT(""), Composite, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(RoadPart, TEXT(""), Composite, TEXT("B"));

		UMaterialEditingLibrary::ConnectMaterialProperty(Composite, TEXT(""), MP_BaseColor);

		UMaterialEditingLibrary::RecompileMaterial(Material);
		return true;
	}
}

namespace
{
	const TCHAR* RoadMaterialPackagePath = TEXT("/GeoWorld/Materials/M_GeoRoad");
	const TCHAR* RoadMaterialAssetName = TEXT("M_GeoRoad");

	/**
	 * M_GeoRoad, il materiale delle strade 3D: l'atlante delle superfici
	 * (Roads/RoadMesh.h) letto con le UV della mesh, e una ruvidita' da asfalto.
	 *
	 *     TextureCoordinate ---> TextureSampleParameter2D ("RoadAtlas") ---> Base Color
	 *     Constant 0.85 ------------------------------------------------------> Roughness
	 *
	 * Le UV fanno tutto: la U sceglie la striscia dell'atlante (asfalto,
	 * binari...) e la attraversa da un bordo all'altro della strada, la V scorre
	 * lungo la strada e si ripete ogni 12 m.
	 */
	bool BuildRoadGraph(UMaterial* Material)
	{
		Material->MaterialDomain = MD_Surface;
		Material->SetShadingModel(MSM_DefaultLit);

		UMaterialExpressionTextureCoordinate* Coordinates =
			Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionTextureCoordinate::StaticClass(), -600, 0));
		UMaterialExpressionTextureSampleParameter2D* Atlas =
			Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -300, 0));
		UMaterialExpressionConstant* Roughness =
			Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionConstant::StaticClass(), -300, 250));
		if (!Coordinates || !Atlas || !Roughness) { return false; }

		Atlas->ParameterName = TEXT("RoadAtlas");
		Atlas->SamplerType = SAMPLERTYPE_Color;
		Roughness->R = 0.85f;

		UMaterialEditingLibrary::ConnectMaterialExpressions(Coordinates, TEXT(""), Atlas, TEXT("UVs"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Atlas, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);

		UMaterialEditingLibrary::RecompileMaterial(Material);
		return true;
	}

	/**
	 * Crea il materiale, o lo RIFA' sul posto se esiste: si svuota il grafo e
	 * lo si ricostruisce. Cancellarlo e crearne uno nuovo lascerebbe
	 * riferimenti rotti nelle istanze dinamiche gia' create, e costringerebbe a
	 * chiudere l'editor. Ritorna false se qualcosa e' andato storto (e lo dice).
	 */
	bool CreateOrRebuildMaterial(const TCHAR* PackagePath, const TCHAR* AssetName,
	                             bool (*BuildGraph)(UMaterial*))
	{
		UMaterial* Material = LoadObject<UMaterial>(nullptr, PackagePath);
		const bool bIsNew = (Material == nullptr);

		UPackage* Package = nullptr;
		if (bIsNew)
		{
			Package = CreatePackage(PackagePath);
			if (!Package)
			{
				Report(FString::Printf(TEXT("%s: non riesco a creare il package. Fallo a mano: docs/fase6-verifica.md"),
					AssetName), FColor::Red);
				return false;
			}
			Material = NewObject<UMaterial>(Package, AssetName, RF_Public | RF_Standalone);
			if (!Material)
			{
				Report(FString::Printf(TEXT("%s: non riesco a creare il materiale."), AssetName), FColor::Red);
				return false;
			}
		}
		else
		{
			Package = Material->GetOutermost();
			UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
			Report(FString::Printf(TEXT("%s esiste: lo rifaccio con il grafo corretto."), AssetName), FColor::Yellow);
		}

		if (!BuildGraph(Material))
		{
			Report(FString::Printf(TEXT("%s: creazione dei nodi fallita."), AssetName), FColor::Red);
			return false;
		}

		if (!SaveMaterialPackage(Package, Material, bIsNew, PackagePath))
		{
			Report(FString::Printf(TEXT("%s costruito ma NON salvato: salvalo tu dal Content Browser."), AssetName),
				FColor::Yellow);
			return false;
		}

		// NOTA UE: il formato di Printf DEVE essere un letterale TEXT("..."):
		// da UE 5.5 lo si controlla in compilazione, e un "a ? TEXT(x) :
		// TEXT(y)" non compila (C2664). Due chiamate, una per caso.
		if (bIsNew) { Report(FString::Printf(TEXT("%s creato in /GeoWorld/Materials."), AssetName)); }
		else { Report(FString::Printf(TEXT("%s rifatto."), AssetName)); }
		return true;
	}
}

static FAutoConsoleCommand GeoImageryCreateMaterialCommand(
	TEXT("geo.Imagery.CreateMaterial"),
	TEXT("Costruisce (o RIFA') M_GeoTerrain (foto e strade dipinte) e M_GeoRoad (strade 3D) in /GeoWorld/Materials."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		const bool bTerrain = CreateOrRebuildMaterial(MaterialPackagePath, MaterialAssetName, &BuildDrapeGraph);
		const bool bRoad = CreateOrRebuildMaterial(RoadMaterialPackagePath, RoadMaterialAssetName, &BuildRoadGraph);
		if (bTerrain && bRoad)
		{
			Report(TEXT("Materiali pronti. Riavvia il Play (o rilancia geo.Roads.Demo / geo.Imagery.Demo)."));
		}
		else if (!bTerrain)
		{
			Report(TEXT("M_GeoTerrain non fatto: puoi costruirlo a mano, docs/fase6-verifica.md"), FColor::Red);
		}
	}));

#endif  // WITH_EDITOR
