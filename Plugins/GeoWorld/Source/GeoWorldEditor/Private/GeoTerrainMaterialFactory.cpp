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
//  LE TRANSIZIONI MORBIDE
//  Quando una tile si affina, le figlie nascono con la forma, la luce e la
//  foto del padre e diventano se stesse in mezzo secondo. Il materiale lo fa
//  con due float per componente (Custom Primitive Data): Morph (0) sposta i
//  vertici sulla superficie del padre e ne mescola le normali, Fade (1)
//  mescola la foto attuale con la "precedente" (BaseColorPrevious...). Il
//  provider riconosce il materiale vecchio perche' non ha BaseColorPrevious.
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
//
//  L'ACQUA (FASE 9)
//  Lo stesso comando costruisce anche M_GeoWater, la collezione dei parametri
//  delle onde MPC_GeoWater e una texture di riposo lineare (T_GeoWaterDefault).
//  Il grafo dell'acqua e' spiegato sopra BuildWaterGraph, e il perche' delle
//  scelte in docs/fase9-design.md.
// =============================================================================
#include "CoreMinimal.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Geo/GeoUnits.h"
#include "GeoCoreModule.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
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

	/** Salva un asset (materiale, collezione, texture) nel suo package. */
	bool SaveAssetPackage(UPackage* Package, UObject* Asset, bool bIsNew, const TCHAR* PackagePath)
	{
		Package->MarkPackageDirty();
		if (bIsNew) { FAssetRegistryModule::AssetCreated(Asset); }

		const FString FileName = FPackageName::LongPackageNameToFilename(
			PackagePath, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Arguments;
		Arguments.TopLevelFlags = RF_Public | RF_Standalone;
		Arguments.SaveFlags = SAVE_NoError;

		return UPackage::SavePackage(Package, Asset, *FileName, Arguments);
	}

	// -------------------------------------------------------------------------
	//  Piccoli attrezzi per costruire i grafi senza ripetere ogni volta tre
	//  righe di cast. Un nodo che non si crea lascia Ok a false: il grafo si
	//  costruisce lo stesso fino in fondo, e alla fine si dice che e' fallito.
	// -------------------------------------------------------------------------
	struct FGraph
	{
		UMaterial* Material = nullptr;
		bool bOk = true;

		template <typename T>
		T* Make(int32 X, int32 Y)
		{
			T* Node = Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
			if (!Node) { bOk = false; }
			return Node;
		}

		void Connect(UMaterialExpression* From, const TCHAR* Output, UMaterialExpression* To, const TCHAR* Input)
		{
			if (!From || !To) { bOk = false; return; }
			UMaterialEditingLibrary::ConnectMaterialExpressions(From, Output, To, Input);
		}

		void Connect(UMaterialExpression* From, const TCHAR* Output, EMaterialProperty Property)
		{
			if (!From) { bOk = false; return; }
			UMaterialEditingLibrary::ConnectMaterialProperty(From, Output, Property);
		}

		/** Un parametro scalare letto dai Custom Primitive Data del componente. */
		UMaterialExpressionScalarParameter* PrimitiveData(const TCHAR* Name, uint8 Index, int32 X, int32 Y)
		{
			UMaterialExpressionScalarParameter* Node = Make<UMaterialExpressionScalarParameter>(X, Y);
			if (Node)
			{
				Node->ParameterName = Name;
				Node->DefaultValue = 0.0f;
				Node->bUseCustomPrimitiveData = true;
				Node->PrimitiveDataIndex = Index;
			}
			return Node;
		}

		/**
		 * uv = TexCoord[0] * Param.B + Param.RG, e il campionamento della texture
		 * `TextureName` con quelle uv. Ritorna il campionatore.
		 *
		 * La scala e' la stessa sui due assi, quindi uno scalare (l'uscita "B")
		 * va bene: moltiplicato per un float2 scala entrambe le componenti. Per
		 * l'offset invece serve un float2 VERO, da qui la maschera RG: e' il
		 * pezzo che mancava nella prima versione (vedi in cima al file).
		 */
		UMaterialExpressionTextureSampleParameter2D* WindowedSample(
			UMaterialExpression* Coordinates, const TCHAR* WindowName, const TCHAR* TextureName, int32 X, int32 Y)
		{
			UMaterialExpressionVectorParameter* Window = Make<UMaterialExpressionVectorParameter>(X - 850, Y + 150);
			UMaterialExpressionComponentMask* OffsetMask = Make<UMaterialExpressionComponentMask>(X - 400, Y + 150);
			UMaterialExpressionMultiply* Scale = Make<UMaterialExpressionMultiply>(X - 400, Y);
			UMaterialExpressionAdd* Offset = Make<UMaterialExpressionAdd>(X - 200, Y);
			UMaterialExpressionTextureSampleParameter2D* Sampler = Make<UMaterialExpressionTextureSampleParameter2D>(X, Y);
			if (!bOk) { return nullptr; }

			// Valore di riposo: nessun ritaglio. Cosi' il materiale e' guardabile
			// nell'editor anche senza che nessuno gli passi dei parametri.
			Window->ParameterName = WindowName;
			Window->DefaultValue = FLinearColor(0.0f, 0.0f, 1.0f, 1.0f);
			OffsetMask->R = true;
			OffsetMask->G = true;
			OffsetMask->B = false;
			OffsetMask->A = false;
			Sampler->ParameterName = TextureName;
			// Color = sRGB: foto e strade dipinte sono codificate sRGB.
			Sampler->SamplerType = SAMPLERTYPE_Color;

			Connect(Coordinates, TEXT(""), Scale, TEXT("A"));
			Connect(Window, TEXT("B"), Scale, TEXT("B"));
			Connect(Window, TEXT(""), OffsetMask, TEXT(""));
			Connect(Scale, TEXT(""), Offset, TEXT("A"));
			Connect(OffsetMask, TEXT(""), Offset, TEXT("B"));
			Connect(Offset, TEXT(""), Sampler, TEXT("UVs"));
			return Sampler;
		}

		/**
		 * La foto con le strade dipinte sopra:
		 *     colore = foto * (1 - strade.A * forza) + strade.RGB * forza
		 * Le strade sono in alfa PREMOLTIPLICATO (Roads/RoadRasterizer.h): una
		 * moltiplicazione e una somma, senza divisioni.
		 */
		UMaterialExpression* PhotoWithRoads(UMaterialExpression* Coordinates,
		                                    const TCHAR* DrapeWindow, const TCHAR* DrapeTexture,
		                                    const TCHAR* OverlayWindow, const TCHAR* OverlayTexture,
		                                    const TCHAR* StrengthName, int32 X, int32 Y)
		{
			UMaterialExpression* Photo = WindowedSample(Coordinates, DrapeWindow, DrapeTexture, X, Y);
			UMaterialExpression* Roads = WindowedSample(Coordinates, OverlayWindow, OverlayTexture, X, Y + 350);
			UMaterialExpressionScalarParameter* Strength = Make<UMaterialExpressionScalarParameter>(X, Y + 650);
			UMaterialExpressionMultiply* AlphaTimesStrength = Make<UMaterialExpressionMultiply>(X + 250, Y + 500);
			UMaterialExpressionOneMinus* PhotoWeight = Make<UMaterialExpressionOneMinus>(X + 400, Y + 500);
			UMaterialExpressionMultiply* PhotoPart = Make<UMaterialExpressionMultiply>(X + 550, Y);
			UMaterialExpressionMultiply* RoadPart = Make<UMaterialExpressionMultiply>(X + 400, Y + 350);
			UMaterialExpressionAdd* Composite = Make<UMaterialExpressionAdd>(X + 700, Y + 150);
			if (!bOk) { return nullptr; }

			// Vale 0 di default: una tile a cui nessuno ha dato le strade mostra
			// la foto e basta, anche se la texture di default del parametro
			// (quella del motore) non e' trasparente.
			Strength->ParameterName = StrengthName;
			Strength->DefaultValue = 0.0f;

			Connect(Roads, TEXT("A"), AlphaTimesStrength, TEXT("A"));
			Connect(Strength, TEXT(""), AlphaTimesStrength, TEXT("B"));
			Connect(AlphaTimesStrength, TEXT(""), PhotoWeight, TEXT(""));
			Connect(Photo, TEXT(""), PhotoPart, TEXT("A"));
			Connect(PhotoWeight, TEXT(""), PhotoPart, TEXT("B"));
			Connect(Roads, TEXT(""), RoadPart, TEXT("A"));
			Connect(Strength, TEXT(""), RoadPart, TEXT("B"));
			Connect(PhotoPart, TEXT(""), Composite, TEXT("A"));
			Connect(RoadPart, TEXT(""), Composite, TEXT("B"));
			return Composite;
		}

		/**
		 * Il GEOMORPHING nella posizione: il vertice scende di Delta * Morph
		 * lungo l'alto LOCALE della tile, cioe' torna sulla superficie del padre
		 * quando Morph = 1. Delta e' in TexCoord[2].G, in metri; la
		 * trasformazione locale -> mondo del componente ha gia' dentro la scala
		 * metri -> unita' Unreal, e la rotazione del frame NEU della tile.
		 */
		UMaterialExpression* MorphOffset(UMaterialExpression* Morph, int32 X, int32 Y)
		{
			UMaterialExpressionTextureCoordinate* MorphData = Make<UMaterialExpressionTextureCoordinate>(X - 800, Y);
			UMaterialExpressionComponentMask* Delta = Make<UMaterialExpressionComponentMask>(X - 600, Y);
			UMaterialExpressionMultiply* Scaled = Make<UMaterialExpressionMultiply>(X - 400, Y);
			UMaterialExpressionMultiply* Down = Make<UMaterialExpressionMultiply>(X - 250, Y);
			UMaterialExpressionConstant2Vector* Horizontal = Make<UMaterialExpressionConstant2Vector>(X - 250, Y + 150);
			UMaterialExpressionAppendVector* LocalOffset = Make<UMaterialExpressionAppendVector>(X - 100, Y);
			UMaterialExpressionTransform* ToWorld = Make<UMaterialExpressionTransform>(X, Y);
			if (!bOk) { return nullptr; }

			MorphData->CoordinateIndex = 2;
			Delta->R = false;
			Delta->G = true;
			Delta->B = false;
			Delta->A = false;
			Down->ConstB = -1.0f;
			Horizontal->R = 0.0f;
			Horizontal->G = 0.0f;
			ToWorld->TransformSourceType = TRANSFORMSOURCE_Local;
			ToWorld->TransformType = TRANSFORM_World;

			Connect(MorphData, TEXT(""), Delta, TEXT(""));
			Connect(Delta, TEXT(""), Scaled, TEXT("A"));
			Connect(Morph, TEXT(""), Scaled, TEXT("B"));
			Connect(Scaled, TEXT(""), Down, TEXT("A"));
			Connect(Horizontal, TEXT(""), LocalOffset, TEXT("A"));
			Connect(Down, TEXT(""), LocalOffset, TEXT("B"));
			Connect(LocalOffset, TEXT(""), ToWorld, TEXT(""));
			return ToWorld;
		}
	};

	/**
	 * Costruisce il grafo di M_GeoTerrain. Ritorna false se un nodo non si crea.
	 *
	 * DALLE TRANSIZIONI MORBIDE il grafo fa tre cose in piu':
	 *
	 *  1. la FOTO PRECEDENTE: un secondo "foto + strade" con i parametri
	 *     ...Previous, mescolato al primo con Fade (Custom Primitive Data 1).
	 *     Quando una tile si affina nasce con la foto del padre e sfuma verso
	 *     la propria;
	 *  2. la FORMA del padre: World Position Offset di -Delta * Morph (Custom
	 *     Primitive Data 0);
	 *  3. la LUCE del padre: la normale e' un lerp fra quella del vertice e
	 *     quella del padre (TexCoord[1].RG + TexCoord[2].R), in spazio MONDO
	 *     (Tangent Space Normal spento). Senza, la forma scivolerebbe ma il
	 *     pendio cambierebbe luce di colpo: meta' del lampo resterebbe.
	 */
	bool BuildDrapeGraph(UMaterial* Material)
	{
		// Lit, non Unlit. Unlit mostrerebbe i colori esatti dell'ortofoto senza
		// dipendere dalle luci, il che e' comodo; ma toglierebbe ogni
		// ombreggiatura, e il rilievo costruito nella Fase 5 sparirebbe
		// visivamente. Con MD_Surface la luce direzionale fa il suo lavoro.
		Material->MaterialDomain = MD_Surface;
		Material->SetShadingModel(MSM_DefaultLit);
		// La normale arriva gia' in spazio mondo (punto 3 qui sopra).
		Material->bTangentSpaceNormal = false;

		FGraph G{ Material };

		UMaterialExpressionTextureCoordinate* Coordinates = G.Make<UMaterialExpressionTextureCoordinate>(-2200, 0);

		// 1. Foto e strade, attuali e precedenti, e la dissolvenza fra le due.
		UMaterialExpression* Current = G.PhotoWithRoads(Coordinates,
			TEXT("DrapeUv"), TEXT("BaseColor"), TEXT("OverlayUv"), TEXT("Overlay"), TEXT("OverlayStrength"), -1200, 0);
		UMaterialExpression* Previous = G.PhotoWithRoads(Coordinates,
			TEXT("DrapeUvPrevious"), TEXT("BaseColorPrevious"), TEXT("OverlayUvPrevious"), TEXT("OverlayPrevious"),
			TEXT("OverlayPreviousStrength"), -1200, 900);
		UMaterialExpressionScalarParameter* Fade = G.PrimitiveData(TEXT("Fade"), 1, -300, 700);
		UMaterialExpressionLinearInterpolate* Colour = G.Make<UMaterialExpressionLinearInterpolate>(0, 300);
		G.Connect(Current, TEXT(""), Colour, TEXT("A"));
		G.Connect(Previous, TEXT(""), Colour, TEXT("B"));
		G.Connect(Fade, TEXT(""), Colour, TEXT("Alpha"));
		G.Connect(Colour, TEXT(""), MP_BaseColor);

		// 2. La forma del padre.
		UMaterialExpressionScalarParameter* Morph = G.PrimitiveData(TEXT("Morph"), 0, -1200, 1900);
		G.Connect(G.MorphOffset(Morph, 0, 1900), TEXT(""), MP_WorldPositionOffset);

		// 3. La luce del padre.
		UMaterialExpressionTextureCoordinate* ParentXY = G.Make<UMaterialExpressionTextureCoordinate>(-1000, 2300);
		UMaterialExpressionTextureCoordinate* ParentZ = G.Make<UMaterialExpressionTextureCoordinate>(-1000, 2450);
		UMaterialExpressionComponentMask* ParentZMask = G.Make<UMaterialExpressionComponentMask>(-800, 2450);
		UMaterialExpressionAppendVector* ParentLocal = G.Make<UMaterialExpressionAppendVector>(-600, 2300);
		UMaterialExpressionTransform* ParentWorld = G.Make<UMaterialExpressionTransform>(-400, 2300);
		UMaterialExpressionVertexNormalWS* OwnNormal = G.Make<UMaterialExpressionVertexNormalWS>(-400, 2150);
		UMaterialExpressionLinearInterpolate* Blend = G.Make<UMaterialExpressionLinearInterpolate>(-200, 2200);
		UMaterialExpressionNormalize* Normal = G.Make<UMaterialExpressionNormalize>(0, 2200);
		if (!G.bOk) { return false; }

		ParentXY->CoordinateIndex = 1;
		ParentZ->CoordinateIndex = 2;
		ParentZMask->R = true;
		ParentZMask->G = false;
		ParentZMask->B = false;
		ParentZMask->A = false;
		ParentWorld->TransformSourceType = TRANSFORMSOURCE_Local;
		ParentWorld->TransformType = TRANSFORM_World;

		G.Connect(ParentZ, TEXT(""), ParentZMask, TEXT(""));
		G.Connect(ParentXY, TEXT(""), ParentLocal, TEXT("A"));
		G.Connect(ParentZMask, TEXT(""), ParentLocal, TEXT("B"));
		G.Connect(ParentLocal, TEXT(""), ParentWorld, TEXT(""));
		G.Connect(OwnNormal, TEXT(""), Blend, TEXT("A"));
		G.Connect(ParentWorld, TEXT(""), Blend, TEXT("B"));
		G.Connect(Morph, TEXT(""), Blend, TEXT("Alpha"));
		G.Connect(Blend, TEXT(""), Normal, TEXT(""));
		G.Connect(Normal, TEXT(""), MP_Normal);

		if (!G.bOk) { return false; }
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
	 * (Roads/RoadMesh.h) letto con le UV della mesh.
	 *
	 *     TextureCoordinate ---> TextureSampleParameter2D ("RoadAtlas") --RGB--> Base Color
	 *                                                                   \--A----> Roughness
	 *     Morph (Custom Primitive Data 0) ---> -Delta * Morph -------------------> World Position Offset
	 *
	 * Le UV fanno tutto: la U sceglie la striscia dell'atlante (asfalto,
	 * binari, cordolo...) e la attraversa da un bordo all'altro, la V scorre
	 * lungo la strada e si ripete ogni 12 m. L'ALFA dell'atlante e' la
	 * ruvidita': l'asfalto e' opaco, le rotaie e il guardrail luccicano. La
	 * prima versione aveva una ruvidita' fissa a 0,85: tutto della stessa
	 * plastica grigia, "manca il materiale".
	 *
	 * Il World Position Offset e' lo stesso del terreno: quando una tile si
	 * affina e scivola dalla forma del padre alla propria, la sua strada
	 * scivola con lei.
	 */
	bool BuildRoadGraph(UMaterial* Material)
	{
		Material->MaterialDomain = MD_Surface;
		Material->SetShadingModel(MSM_DefaultLit);

		FGraph G{ Material };
		UMaterialExpressionTextureCoordinate* Coordinates = G.Make<UMaterialExpressionTextureCoordinate>(-600, 0);
		UMaterialExpressionTextureSampleParameter2D* Atlas = G.Make<UMaterialExpressionTextureSampleParameter2D>(-300, 0);
		UMaterialExpressionScalarParameter* Morph = G.PrimitiveData(TEXT("Morph"), 0, -900, 500);
		if (!G.bOk) { return false; }

		Atlas->ParameterName = TEXT("RoadAtlas");
		Atlas->SamplerType = SAMPLERTYPE_Color;

		G.Connect(Coordinates, TEXT(""), Atlas, TEXT("UVs"));
		G.Connect(Atlas, TEXT("RGB"), MP_BaseColor);
		G.Connect(Atlas, TEXT("A"), MP_Roughness);
		G.Connect(G.MorphOffset(Morph, 0, 500), TEXT(""), MP_WorldPositionOffset);

		if (!G.bOk) { return false; }
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

		if (!SaveAssetPackage(Package, Material, bIsNew, PackagePath))
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

// =============================================================================
//  L'ACQUA (FASE 9): MPC_GeoWater, T_GeoWaterDefault, M_GeoWater
// =============================================================================
namespace
{
	const TCHAR* WaterMaterialPackagePath = TEXT("/GeoWorld/Materials/M_GeoWater");
	const TCHAR* WaterMaterialAssetName = TEXT("M_GeoWater");
	const TCHAR* WaveCollectionPackagePath = TEXT("/GeoWorld/Materials/MPC_GeoWater");
	const TCHAR* WaveCollectionAssetName = TEXT("MPC_GeoWater");
	const TCHAR* WaterDefaultTexturePackagePath = TEXT("/GeoWorld/Materials/T_GeoWaterDefault");
	const TCHAR* WaterDefaultTextureAssetName = TEXT("T_GeoWaterDefault");

	/** Quante onde di Gerstner: lo stesso numero di Water::GerstnerWaveCount (WaveSpectrum.h). */
	constexpr int32 WaterWaveCount = 6;

	/**
	 * La collezione dei parametri delle onde. Il subsystem dell'acqua la
	 * scrive una volta per frame (UGeoWaterSubsystem::UpdateWaves), tutti i
	 * materiali dell'acqua la leggono: una scrittura per il mondo, non una
	 * per tile.
	 *
	 *   Wave{i}A = (direzione nord, direzione est, numero d'onda k, fase)
	 *   Wave{i}B = (ampiezza m, ripidita' Q, lunghezza d'onda m, 0)
	 *   WaterWind   = (verso nord, verso est, velocita' m/s, schiuma 0..1)
	 *   WaterDetail = (scorrimento delle due normali di dettaglio)
	 *   WaterFlow   = (fase A, fase B, ciclo in secondi, 0) della corrente
	 *
	 * Se esiste gia' si AGGIUNGONO solo i parametri mancanti: quelli che ci
	 * sono tengono il loro Id, e i materiali che li usano restano collegati.
	 */
	UMaterialParameterCollection* CreateOrUpdateWaveCollection()
	{
		UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr,
			*FString::Printf(TEXT("%s.%s"), WaveCollectionPackagePath, WaveCollectionAssetName), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		const bool bIsNew = (Collection == nullptr);
		UPackage* Package = nullptr;
		if (bIsNew)
		{
			Package = CreatePackage(WaveCollectionPackagePath);
			if (!Package) { Report(TEXT("MPC_GeoWater: non riesco a creare il package."), FColor::Red); return nullptr; }
			Collection = NewObject<UMaterialParameterCollection>(Package, WaveCollectionAssetName, RF_Public | RF_Standalone);
			if (!Collection) { Report(TEXT("MPC_GeoWater: non riesco a crearla."), FColor::Red); return nullptr; }
		}
		else
		{
			Package = Collection->GetOutermost();
		}

		TArray<TPair<FName, FLinearColor>> Wanted;
		for (int32 Index = 0; Index < WaterWaveCount; ++Index)
		{
			// Valori di riposo: onde ferme, ampiezza zero (mare piatto finche'
			// il subsystem non scrive).
			Wanted.Emplace(FName(*FString::Printf(TEXT("Wave%dA"), Index)), FLinearColor(1.0f, 0.0f, 0.1f, 0.0f));
			Wanted.Emplace(FName(*FString::Printf(TEXT("Wave%dB"), Index)), FLinearColor(0.0f, 0.0f, 60.0f, 0.0f));
		}
		Wanted.Emplace(FName(TEXT("WaterWind")), FLinearColor(1.0f, 0.0f, 5.0f, 0.0f));
		Wanted.Emplace(FName(TEXT("WaterDetail")), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
		Wanted.Emplace(FName(TEXT("WaterFlow")), FLinearColor(0.0f, 0.5f, 4.0f, 0.0f));

		int32 Added = 0;
		for (const TPair<FName, FLinearColor>& Parameter : Wanted)
		{
			const bool bPresent = Collection->VectorParameters.ContainsByPredicate(
				[&Parameter](const FCollectionVectorParameter& Existing) { return Existing.ParameterName == Parameter.Key; });
			if (bPresent) { continue; }
			// Il costruttore di FCollectionVectorParameter crea gia' un Id nuovo.
			FCollectionVectorParameter& New = Collection->VectorParameters.AddDefaulted_GetRef();
			New.ParameterName = Parameter.Key;
			New.DefaultValue = Parameter.Value;
			++Added;
		}

		if (bIsNew || Added > 0)
		{
			Collection->PostEditChange();
			if (!SaveAssetPackage(Package, Collection, bIsNew, WaveCollectionPackagePath))
			{
				Report(TEXT("MPC_GeoWater costruita ma NON salvata: salvala tu dal Content Browser."), FColor::Yellow);
			}
		}
		if (bIsNew) { Report(TEXT("MPC_GeoWater creata in /GeoWorld/Materials.")); }
		else if (Added > 0) { Report(FString::Printf(TEXT("MPC_GeoWater: aggiunti %d parametri."), Added)); }
		return Collection;
	}

	/**
	 * T_GeoWaterDefault: 4x4 pixel LINEARI (non sRGB). Serve solo come valore
	 * di riposo dei parametri texture di M_GeoWater, che leggono DATI: il
	 * campionatore "Linear Color" esige una texture lineare anche come
	 * default, e quella di default del motore e' sRGB (il materiale non
	 * compilerebbe). A runtime ogni tile riceve le sue texture vere.
	 *
	 * BGRA = (255, 128, 128, 128): come normale e' "piatta", come raster
	 * d'acqua e' "sulla riva, onde a meta'".
	 */
	UTexture2D* CreateOrLoadWaterDefaultTexture()
	{
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr,
			*FString::Printf(TEXT("%s.%s"), WaterDefaultTexturePackagePath, WaterDefaultTextureAssetName), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		if (Texture) { return Texture; }

		UPackage* Package = CreatePackage(WaterDefaultTexturePackagePath);
		if (!Package) { Report(TEXT("T_GeoWaterDefault: non riesco a creare il package."), FColor::Red); return nullptr; }
		Texture = NewObject<UTexture2D>(Package, WaterDefaultTextureAssetName, RF_Public | RF_Standalone);
		if (!Texture) { Report(TEXT("T_GeoWaterDefault: non riesco a crearla."), FColor::Red); return nullptr; }

		constexpr int32 Side = 4;
		TArray<uint8> Pixels;
		Pixels.Reserve(Side * Side * 4);
		for (int32 Index = 0; Index < Side * Side; ++Index)
		{
			Pixels.Add(255);   // B
			Pixels.Add(128);   // G
			Pixels.Add(128);   // R
			Pixels.Add(128);   // A
		}
		Texture->Source.Init(Side, Side, 1, 1, TSF_BGRA8, Pixels.GetData());
		Texture->SRGB = false;
		// Non compressa: i dati restano quelli scritti, byte per byte.
		Texture->CompressionSettings = TC_VectorDisplacementmap;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->VirtualTextureStreaming = false;
		Texture->PostEditChange();

		if (!SaveAssetPackage(Package, Texture, /*bIsNew=*/true, WaterDefaultTexturePackagePath))
		{
			Report(TEXT("T_GeoWaterDefault creata ma NON salvata: salvala tu dal Content Browser."), FColor::Yellow);
		}
		else
		{
			Report(TEXT("T_GeoWaterDefault creata in /GeoWorld/Materials."));
		}
		return Texture;
	}

	/** Un'uscita precisa di un nodo: il nodo e l'indice dell'uscita. */
	struct FPin
	{
		UMaterialExpression* Node = nullptr;
		int32 Output = 0;
	};

	/** Un ingresso di un nodo Custom: il nome che ha nell'HLSL e cosa ci arriva. */
	struct FNamedPin
	{
		const TCHAR* Name = nullptr;
		FPin Pin;
	};

	/**
	 * L'uscita con questi canali (per esempio RGBA = tutti e quattro), o
	 * INDEX_NONE. Si cerca per MASCHERA e non per nome: i nomi delle uscite
	 * ("RGBA", "RGB"...) non sono un contratto del motore, le maschere si'.
	 */
	int32 FindOutputByChannels(UMaterialExpression* Node, bool bR, bool bG, bool bB, bool bA)
	{
		if (!Node) { return INDEX_NONE; }
		const TArray<FExpressionOutput>& Outputs = Node->GetOutputs();
		for (int32 Index = 0; Index < Outputs.Num(); ++Index)
		{
			const FExpressionOutput& Output = Outputs[Index];
			if (Output.Mask == 0) { continue; }
			if ((Output.MaskR != 0) == bR && (Output.MaskG != 0) == bG
				&& (Output.MaskB != 0) == bB && (Output.MaskA != 0) == bA)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/**
	 * Un nodo Custom (HLSL scritto a mano) con gli ingressi dati per nome.
	 * Gli ingressi si collegano direttamente (FExpressionInput::Connect),
	 * senza cercarli per nome: cosi' non dipende da come l'editor chiama i
	 * pin.
	 */
	UMaterialExpressionCustom* MakeCustom(FGraph& G, const TCHAR* Description, const TCHAR* Code,
	                                      ECustomMaterialOutputType OutputType,
	                                      const TArray<FNamedPin>& Inputs, int32 X, int32 Y)
	{
		UMaterialExpressionCustom* Custom = G.Make<UMaterialExpressionCustom>(X, Y);
		if (!Custom) { return nullptr; }
		Custom->Description = Description;
		Custom->Code = Code;
		Custom->OutputType = OutputType;
		Custom->Inputs.Reset();
		for (const FNamedPin& Input : Inputs)
		{
			if (!Input.Pin.Node)
			{
				G.bOk = false;
				Report(FString::Printf(TEXT("M_GeoWater: manca il nodo per l'ingresso %s di %s."), Input.Name, Description),
					FColor::Red);
				continue;
			}
			FCustomInput& New = Custom->Inputs.AddDefaulted_GetRef();
			New.InputName = FName(Input.Name);
			New.Input.Connect(Input.Pin.Output, Input.Pin.Node);
		}
		return Custom;
	}

	// -------------------------------------------------------------------------
	//  L'HLSL dei due nodi Custom. E' lo stesso conto di WaveSpectrum.h
	//  (FWaveField::Displacement), che e' provato nei test: se cambi uno,
	//  cambia l'altro.
	//
	//  Il raster della tile (WaterMesh.h, BuildWaterRasterPixels):
	//    R = distanza dalla riva: 128 + 8 * pixel (positiva in acqua)
	//    G = esposizione alle onde (1 mare aperto, ~0 canale)
	//    B = corrente verso est,  centesimi di m/s + 128
	//    A = corrente verso sud,  centesimi di m/s + 128
	//  Le coordinate: X del mondo = nord, Y = est, in unita' Unreal.
	// -------------------------------------------------------------------------

	/** World Position Offset: le onde di Gerstner sulla geometria. */
	const TCHAR* WaterVertexCode = TEXT(R"HLSL(
// Coordinate in metri: x = nord, y = est.
float2 Ground = Pos.xy / UuPerMetre;
float shoreMetres = (Raster.r * 255.0 - 128.0) / 8.0 * MetresPerPixel;
// Le onde muoiono negli ultimi 15 m prima della riva.
float exposure = Raster.g * saturate(shoreMetres / 15.0);
float4 WA[6] = { A0, A1, A2, A3, A4, A5 };
float4 WB[6] = { B0, B1, B2, B3, B4, B5 };
float3 Offset = 0;
for (int i = 0; i < 6; i++)
{
	// Un'onda piu' corta di 8 celle della mesh non si puo' disegnare con la
	// geometria (sarebbe aliasing): resta solo nella luce (nodo dei pixel).
	float keep = saturate(WB[i].z / (4.0 * CellMetres) - 1.0);
	float amplitude = WB[i].x * exposure * keep;
	float theta = WA[i].z * dot(WA[i].xy, Ground) + WA[i].w;
	float s, c;
	sincos(theta, s, c);
	Offset.xy += WB[i].y * amplitude * WA[i].xy * c;
	Offset.z += amplitude * s;
}
return Offset * UuPerMetre;
)HLSL");

	/** Pixel: la normale (xyz, nello spazio del mondo) e la schiuma (w). */
	const TCHAR* WaterPixelCode = TEXT(R"HLSL(
float2 Ground = Pos.xy / UuPerMetre;
// Quanti metri copre un pixel dello schermo, di piu' a sguardo radente.
// (Da profondita' e angolo, non con ddx: questo codice deve compilare anche
// negli shader dove le derivate non esistono.)
float footprint = max(Depth / UuPerMetre * 0.0015 / max(abs(ToCamera.z), 0.08), 0.001);

float shoreMetres = (Raster.r * 255.0 - 128.0) / 8.0 * MetresPerPixel;
float exposure = Raster.g * saturate(shoreMetres / 15.0);
// La corrente in m/s, (nord, est).
float2 flow = float2(128.0 - Raster.a * 255.0, Raster.b * 255.0 - 128.0) * 0.01;

// 1. Le onde di Gerstner, TUTTE: anche quelle troppo corte per la geometria
//    vivono nella luce. Si spengono solo quando sono lunghe pochi pixel.
float4 WA[6] = { A0, A1, A2, A3, A4, A5 };
float4 WB[6] = { B0, B1, B2, B3, B4, B5 };
float2 slope = 0;
float crest = 0;
float crestMax = 0;
for (int i = 0; i < 6; i++)
{
	float aa = saturate(WB[i].z / (6.0 * footprint) - 1.0);
	float ka = WA[i].z * WB[i].x * exposure * aa;
	float theta = WA[i].z * dot(WA[i].xy, Ground) + WA[i].w;
	float s, c;
	sincos(theta, s, c);
	slope += WA[i].xy * ka * c;
	crest += WB[i].y * ka * s;
	crestMax += WB[i].y * ka;
}
// Normale di Gerstner (GPU Gems 1, cap. 1), come pendenza: n.xy / n.z.
float2 gerstner = -slope / max(1.0 - crest, 0.2);

// 2. L'increspatura: due normali di dettaglio (lato 9 m e 31 m) che scorrono
//    col vento. Sui fiumi la prima scorre anche con la corrente, a due fasi
//    sfalsate di mezzo ciclo: quando una ricomincia ha peso zero.
float2 uvFine = Ground / 9.0 + Detail.xy;
float2 shiftA = flow * (Flow.x * Flow.z) / 9.0;
float2 shiftB = flow * (Flow.y * Flow.z) / 9.0;
float phaseB = abs(1.0 - 2.0 * Flow.x);
float3 fineA = Texture2DSample(NormalTex, NormalTexSampler, uvFine - shiftA).rgb * 2.0 - 1.0;
float3 fineB = Texture2DSample(NormalTex, NormalTexSampler, uvFine - shiftB).rgb * 2.0 - 1.0;
float3 fine = lerp(fineA, fineB, phaseB);
float3 coarse = Texture2DSample(NormalTex, NormalTexSampler, Ground / 31.0 + Detail.zw).rgb * 2.0 - 1.0;
float strength = (0.25 + 0.35 * saturate(Wind.z / 12.0)) * lerp(0.6, 1.0, saturate(Raster.g * 2.0));
float2 ripple = 0.6 * fine.xy / max(fine.z, 0.3) + 0.4 * coarse.xy / max(coarse.z, 0.3);
float3 SurfaceNormal = normalize(float3(gerstner + strength * ripple, 1.0));

// 3. La schiuma: lungo la riva (di piu' col vento e con la corrente) e sulle
//    creste, quando il vento le rompe.
float shoreFoam = saturate(1.0 - shoreMetres / 6.0)
                * (0.15 + 0.85 * exposure * saturate(Wind.z / 8.0) + 0.3 * saturate(length(flow)));
// Le creste si misurano come frazione del massimo (tutte le onde in cima
// insieme): la somma di Q k A di un mare vero e' piccola, circa 0,15.
float crestFoam = saturate((crest / max(crestMax, 0.0001) - 0.6) * 2.5) * Wind.w * saturate(exposure * 2.0);
float foamA = Texture2DSample(FoamTex, FoamTexSampler, uvFine - shiftA).r;
float foamB = Texture2DSample(FoamTex, FoamTexSampler, uvFine - shiftB).r;
float foam = saturate((shoreFoam + crestFoam) * lerp(foamA, foamB, phaseB) * 1.6);
return float4(SurfaceNormal, foam);
)HLSL");

	/**
	 * M_GeoWater. Ritorna false se un nodo non si crea.
	 *
	 * SHADING MODEL "SINGLE LAYER WATER". E' il modello d'acqua del motore:
	 * il colore dell'acqua non e' un BaseColor ma viene dalla luce che
	 * attraversa uno spessore d'acqua, con un coefficiente di ASSORBIMENTO e
	 * uno di DIFFUSIONE per canale (per metro). L'acqua bassa e' trasparente
	 * e mostra il fondale (il terreno scavato), quella profonda diventa blu
	 * o verde da sola, senza dipingerla. Il BaseColor resta per la schiuma.
	 *
	 * MASKED con il raster R come maschera: la riva e' dove la distanza dalla
	 * riva passa per zero, con la risoluzione del raster (256 pixel per
	 * tile), non con quella della mesh.
	 *
	 *   WorldPosition (senza offset) --+
	 *   raster (mip 0, nel vertice) ---+--> Custom "Onde" ------------------> World Position Offset
	 *   MPC_GeoWater Wave0A..Wave5B ---+
	 *
	 *   WorldPosition, raster, MPC, ---+--> Custom "Superficie" --xyz------> Normal (spazio mondo)
	 *   normale e schiuma (texture),   |                         \--w (schiuma)--> BaseColor, Roughness,
	 *   profondita', verso la camera --+                                          ColorScaleBehindWater
	 *   raster.R -----------------------------------------------------------> Opacity Mask
	 *   raster.G (1 = mare) --> lerp(acqua di mare, acqua interna) ---------> Assorbimento, Diffusione
	 */
	bool BuildWaterGraph(UMaterial* Material)
	{
		UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr,
			*FString::Printf(TEXT("%s.%s"), WaveCollectionPackagePath, WaveCollectionAssetName), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		UTexture2D* DefaultTexture = LoadObject<UTexture2D>(nullptr,
			*FString::Printf(TEXT("%s.%s"), WaterDefaultTexturePackagePath, WaterDefaultTextureAssetName), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		if (!Collection || !DefaultTexture)
		{
			Report(TEXT("M_GeoWater: mancano MPC_GeoWater o T_GeoWaterDefault."), FColor::Red);
			return false;
		}

		Material->MaterialDomain = MD_Surface;
		Material->SetShadingModel(MSM_SingleLayerWater);
		Material->BlendMode = BLEND_Masked;
		Material->OpacityMaskClipValue = 0.5f;
		// La normale esce gia' in spazio mondo dal nodo Custom.
		Material->bTangentSpaceNormal = false;

		FGraph G{ Material };

		// --- Il raster della tile: le stesse uv per il vertice e per il pixel.
		UMaterialExpressionTextureCoordinate* Coordinates = G.Make<UMaterialExpressionTextureCoordinate>(-2600, -400);
		UMaterialExpressionVectorParameter* Window = G.Make<UMaterialExpressionVectorParameter>(-2600, -250);
		UMaterialExpressionComponentMask* OffsetMask = G.Make<UMaterialExpressionComponentMask>(-2350, -250);
		UMaterialExpressionMultiply* Scale = G.Make<UMaterialExpressionMultiply>(-2350, -400);
		UMaterialExpressionAdd* Uv = G.Make<UMaterialExpressionAdd>(-2150, -400);
		UMaterialExpressionTextureSampleParameter2D* RasterPixel = G.Make<UMaterialExpressionTextureSampleParameter2D>(-1900, -400);
		UMaterialExpressionTextureSampleParameter2D* RasterVertex = G.Make<UMaterialExpressionTextureSampleParameter2D>(-1900, -1100);
		if (!G.bOk) { return false; }

		Window->ParameterName = TEXT("WaterUv");
		Window->DefaultValue = FLinearColor(0.0f, 0.0f, 1.0f, 1.0f);
		OffsetMask->R = true;
		OffsetMask->G = true;
		OffsetMask->B = false;
		OffsetMask->A = false;
		G.Connect(Coordinates, TEXT(""), Scale, TEXT("A"));
		G.Connect(Window, TEXT("B"), Scale, TEXT("B"));
		G.Connect(Window, TEXT(""), OffsetMask, TEXT(""));
		G.Connect(Scale, TEXT(""), Uv, TEXT("A"));
		G.Connect(OffsetMask, TEXT(""), Uv, TEXT("B"));

		for (UMaterialExpressionTextureSampleParameter2D* Sampler : { RasterPixel, RasterVertex })
		{
			Sampler->ParameterName = TEXT("WaterRaster");
			Sampler->Texture = DefaultTexture;
			// DATI, non colori: Linear Color (la texture e' creata senza sRGB).
			Sampler->SamplerType = SAMPLERTYPE_LinearColor;
			G.Connect(Uv, TEXT(""), Sampler, TEXT("UVs"));
		}
		// Nel vertice non ci sono derivate per scegliere la mip: la 0, esplicita.
		RasterVertex->MipValueMode = TMVM_MipLevel;
		RasterVertex->ConstMipValue = 0;

		// L'uscita RGBA dei campionatori; se il motore non l'avesse, RGB + A.
		auto Rgba = [&G](UMaterialExpressionTextureSampleParameter2D* Sampler, int32 X, int32 Y) -> FPin
		{
			const int32 Full = FindOutputByChannels(Sampler, true, true, true, true);
			if (Full != INDEX_NONE) { return FPin{ Sampler, Full }; }
			const int32 Rgb = FindOutputByChannels(Sampler, true, true, true, false);
			const int32 Alpha = FindOutputByChannels(Sampler, false, false, false, true);
			UMaterialExpressionAppendVector* Append = G.Make<UMaterialExpressionAppendVector>(X, Y);
			if (!Append || Rgb == INDEX_NONE || Alpha == INDEX_NONE) { G.bOk = false; return FPin{}; }
			Append->A.Connect(Rgb, Sampler);
			Append->B.Connect(Alpha, Sampler);
			return FPin{ Append, 0 };
		};
		const FPin RasterPixelRgba = Rgba(RasterPixel, -1650, -300);
		const FPin RasterVertexRgba = Rgba(RasterVertex, -1650, -1000);

		// --- La posizione: SENZA il World Position Offset, cioe' il punto della
		// superficie a riposo. Nel pixel serve quello: la normale di Gerstner e'
		// funzione del punto a riposo, non di quello spostato.
		UMaterialExpressionWorldPosition* Position = G.Make<UMaterialExpressionWorldPosition>(-1900, -1300);
		UMaterialExpressionConstant* UuPerMetre = G.Make<UMaterialExpressionConstant>(-1900, -1450);
		UMaterialExpressionScalarParameter* MetresPerPixel = G.Make<UMaterialExpressionScalarParameter>(-1900, -1550);
		UMaterialExpressionScalarParameter* CellMetres = G.Make<UMaterialExpressionScalarParameter>(-1900, -1650);
		UMaterialExpressionPixelDepth* Depth = G.Make<UMaterialExpressionPixelDepth>(-1900, 100);
		UMaterialExpressionCameraVectorWS* ToCamera = G.Make<UMaterialExpressionCameraVectorWS>(-1900, 200);
		UMaterialExpressionTextureObjectParameter* NormalTexture = G.Make<UMaterialExpressionTextureObjectParameter>(-1900, 300);
		UMaterialExpressionTextureObjectParameter* FoamTexture = G.Make<UMaterialExpressionTextureObjectParameter>(-1900, 500);
		if (!G.bOk) { return false; }

		Position->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
		UuPerMetre->R = static_cast<float>(GeoWorld::Units::MetersToUu);
		MetresPerPixel->ParameterName = TEXT("WaterMetresPerPixel");
		MetresPerPixel->DefaultValue = 10.0f;
		CellMetres->ParameterName = TEXT("WaterCellMetres");
		CellMetres->DefaultValue = 20.0f;
		NormalTexture->ParameterName = TEXT("WaterNormal");
		FoamTexture->ParameterName = TEXT("WaterFoam");
		for (UMaterialExpressionTextureObjectParameter* Texture : { NormalTexture, FoamTexture })
		{
			Texture->Texture = DefaultTexture;
			Texture->SamplerType = SAMPLERTYPE_LinearColor;
		}

		// --- I parametri della collezione.
		auto CollectionParameter = [&G, Collection](const FString& Name, int32 X, int32 Y) -> FPin
		{
			UMaterialExpressionCollectionParameter* Node = G.Make<UMaterialExpressionCollectionParameter>(X, Y);
			if (!Node) { return FPin{}; }
			Node->Collection = Collection;
			Node->ParameterName = FName(*Name);
			Node->ParameterId = Collection->GetParameterId(Node->ParameterName);
			if (!Node->ParameterId.IsValid())
			{
				G.bOk = false;
				Report(FString::Printf(TEXT("M_GeoWater: MPC_GeoWater non ha il parametro %s."), *Name), FColor::Red);
			}
			return FPin{ Node, 0 };
		};
		FPin WaveA[WaterWaveCount], WaveB[WaterWaveCount];
		for (int32 Index = 0; Index < WaterWaveCount; ++Index)
		{
			WaveA[Index] = CollectionParameter(FString::Printf(TEXT("Wave%dA"), Index), -1400, -1700 + Index * 90);
			WaveB[Index] = CollectionParameter(FString::Printf(TEXT("Wave%dB"), Index), -1400, -1150 + Index * 90);
		}
		const FPin Wind = CollectionParameter(TEXT("WaterWind"), -1400, -500);
		const FPin Detail = CollectionParameter(TEXT("WaterDetail"), -1400, -400);
		const FPin Flow = CollectionParameter(TEXT("WaterFlow"), -1400, -300);
		if (!G.bOk) { return false; }

		// ------------------------------------------------------------------
		//  NOTA UE: la posizione nel mondo e' in "Large World Coordinates"
		//  (una coppia di float, per i mondi grandi). Il nodo Custom non la
		//  vuole: a seconda della versione del motore la riceverebbe come un
		//  tipo LWC, e "P.xy" non compilerebbe. TruncateLWC la porta a un float
		//  normale. Qui il float basta: il rebase tiene il mondo vicino
		//  all'origine (entro poche decine di km, cioe' millimetri di errore).
		//
		//  Il nodo si cerca PER NOME a runtime invece di includerne l'header:
		//  se in questa versione non ci fosse, il file compila lo stesso e la
		//  posizione va al Custom cosi' com'e' (e l'Output Log lo dice).
		// ------------------------------------------------------------------
		FPin PositionPin{ Position, 0 };
		if (UClass* TruncateClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpressionTruncateLWC")))
		{
			UMaterialExpression* Truncate = UMaterialEditingLibrary::CreateMaterialExpression(
				Material, TruncateClass, -1650, -1300);
			if (Truncate && UMaterialEditingLibrary::ConnectMaterialExpressions(Position, TEXT(""), Truncate, TEXT("")))
			{
				PositionPin = FPin{ Truncate, 0 };
			}
		}
		if (PositionPin.Node == Position)
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("M_GeoWater: nodo TruncateLWC non disponibile, la posizione va al Custom com'e'."));
		}
		const FPin UuPerMetrePin{ UuPerMetre, 0 };
		const FPin MetresPerPixelPin{ MetresPerPixel, 0 };

		TArray<FNamedPin> VertexInputs;
		TArray<FNamedPin> PixelInputs;
		VertexInputs.Add(FNamedPin{ TEXT("Pos"), PositionPin });
		VertexInputs.Add(FNamedPin{ TEXT("Raster"), RasterVertexRgba });
		PixelInputs.Add(FNamedPin{ TEXT("Pos"), PositionPin });
		PixelInputs.Add(FNamedPin{ TEXT("Raster"), RasterPixelRgba });
		static const TCHAR* WaveANames[WaterWaveCount] = { TEXT("A0"), TEXT("A1"), TEXT("A2"), TEXT("A3"), TEXT("A4"), TEXT("A5") };
		static const TCHAR* WaveBNames[WaterWaveCount] = { TEXT("B0"), TEXT("B1"), TEXT("B2"), TEXT("B3"), TEXT("B4"), TEXT("B5") };
		for (int32 Index = 0; Index < WaterWaveCount; ++Index)
		{
			VertexInputs.Add(FNamedPin{ WaveANames[Index], WaveA[Index] });
			PixelInputs.Add(FNamedPin{ WaveANames[Index], WaveA[Index] });
		}
		for (int32 Index = 0; Index < WaterWaveCount; ++Index)
		{
			VertexInputs.Add(FNamedPin{ WaveBNames[Index], WaveB[Index] });
			PixelInputs.Add(FNamedPin{ WaveBNames[Index], WaveB[Index] });
		}
		VertexInputs.Add(FNamedPin{ TEXT("MetresPerPixel"), MetresPerPixelPin });
		VertexInputs.Add(FNamedPin{ TEXT("CellMetres"), FPin{ CellMetres, 0 } });
		VertexInputs.Add(FNamedPin{ TEXT("UuPerMetre"), UuPerMetrePin });
		PixelInputs.Add(FNamedPin{ TEXT("Wind"), Wind });
		PixelInputs.Add(FNamedPin{ TEXT("Detail"), Detail });
		PixelInputs.Add(FNamedPin{ TEXT("Flow"), Flow });
		PixelInputs.Add(FNamedPin{ TEXT("MetresPerPixel"), MetresPerPixelPin });
		PixelInputs.Add(FNamedPin{ TEXT("UuPerMetre"), UuPerMetrePin });
		PixelInputs.Add(FNamedPin{ TEXT("Depth"), FPin{ Depth, 0 } });
		PixelInputs.Add(FNamedPin{ TEXT("ToCamera"), FPin{ ToCamera, 0 } });
		PixelInputs.Add(FNamedPin{ TEXT("NormalTex"), FPin{ NormalTexture, 0 } });
		PixelInputs.Add(FNamedPin{ TEXT("FoamTex"), FPin{ FoamTexture, 0 } });

		UMaterialExpressionCustom* Waves = MakeCustom(G, TEXT("Onde di Gerstner (WPO)"), WaterVertexCode,
			CMOT_Float3, VertexInputs, -900, -1200);
		UMaterialExpressionCustom* Surface = MakeCustom(G, TEXT("Superficie: normale e schiuma"), WaterPixelCode,
			CMOT_Float4, PixelInputs, -900, -200);
		if (!G.bOk) { return false; }

		G.Connect(Waves, TEXT(""), MP_WorldPositionOffset);

		// --- Normale e schiuma.
		UMaterialExpressionComponentMask* NormalMask = G.Make<UMaterialExpressionComponentMask>(-500, -300);
		UMaterialExpressionComponentMask* FoamMask = G.Make<UMaterialExpressionComponentMask>(-500, -100);
		UMaterialExpressionMultiply* FoamColour = G.Make<UMaterialExpressionMultiply>(-250, -150);
		UMaterialExpressionLinearInterpolate* Roughness = G.Make<UMaterialExpressionLinearInterpolate>(-250, 0);
		UMaterialExpressionOneMinus* BehindWater = G.Make<UMaterialExpressionOneMinus>(-250, 150);
		UMaterialExpressionComponentMask* OpacityMask = G.Make<UMaterialExpressionComponentMask>(-500, 300);
		if (!G.bOk) { return false; }

		NormalMask->R = true;
		NormalMask->G = true;
		NormalMask->B = true;
		NormalMask->A = false;
		FoamMask->R = false;
		FoamMask->G = false;
		FoamMask->B = false;
		FoamMask->A = true;
		OpacityMask->R = true;
		OpacityMask->G = false;
		OpacityMask->B = false;
		OpacityMask->A = false;
		// Il BaseColor di Single Layer Water e' quello della SUPERFICIE: nero
		// per l'acqua (il colore lo fa la luce dentro l'acqua), bianco per la
		// schiuma.
		FoamColour->ConstB = 0.85f;
		Roughness->ConstA = 0.03f;
		Roughness->ConstB = 0.55f;

		G.Connect(Surface, TEXT(""), NormalMask, TEXT(""));
		G.Connect(Surface, TEXT(""), FoamMask, TEXT(""));
		G.Connect(NormalMask, TEXT(""), MP_Normal);
		G.Connect(FoamMask, TEXT(""), FoamColour, TEXT("A"));
		G.Connect(FoamColour, TEXT(""), MP_BaseColor);
		G.Connect(FoamMask, TEXT(""), Roughness, TEXT("Alpha"));
		G.Connect(Roughness, TEXT(""), MP_Roughness);
		G.Connect(FoamMask, TEXT(""), BehindWater, TEXT(""));
		OpacityMask->Input.Connect(RasterPixelRgba.Output, RasterPixelRgba.Node);
		G.Connect(OpacityMask, TEXT(""), MP_OpacityMask);

		// --- L'acqua come mezzo: mare limpido, acque interne piu' verdi e torbide.
		// Coefficienti per metro (R, G, B). Il rosso si assorbe in pochi metri:
		// e' per questo che l'acqua profonda e' blu.
		UMaterialExpressionVectorParameter* AbsorptionSea = G.Make<UMaterialExpressionVectorParameter>(-700, 500);
		UMaterialExpressionVectorParameter* AbsorptionInland = G.Make<UMaterialExpressionVectorParameter>(-700, 650);
		UMaterialExpressionVectorParameter* ScatteringSea = G.Make<UMaterialExpressionVectorParameter>(-700, 800);
		UMaterialExpressionVectorParameter* ScatteringInland = G.Make<UMaterialExpressionVectorParameter>(-700, 950);
		UMaterialExpressionComponentMask* Exposure = G.Make<UMaterialExpressionComponentMask>(-700, 1100);
		UMaterialExpressionOneMinus* Inland = G.Make<UMaterialExpressionOneMinus>(-500, 1100);
		UMaterialExpressionLinearInterpolate* Absorption = G.Make<UMaterialExpressionLinearInterpolate>(-250, 550);
		UMaterialExpressionLinearInterpolate* Scattering = G.Make<UMaterialExpressionLinearInterpolate>(-250, 850);
		UMaterialExpressionConstant* PhaseG = G.Make<UMaterialExpressionConstant>(-250, 1000);
		UMaterialExpressionSingleLayerWaterMaterialOutput* WaterOutput =
			G.Make<UMaterialExpressionSingleLayerWaterMaterialOutput>(100, 700);
		if (!G.bOk) { return false; }

		AbsorptionSea->ParameterName = TEXT("WaterAbsorptionSea");
		AbsorptionSea->DefaultValue = FLinearColor(0.35f, 0.06f, 0.025f, 0.0f);
		AbsorptionInland->ParameterName = TEXT("WaterAbsorptionInland");
		AbsorptionInland->DefaultValue = FLinearColor(0.40f, 0.16f, 0.30f, 0.0f);
		ScatteringSea->ParameterName = TEXT("WaterScatteringSea");
		ScatteringSea->DefaultValue = FLinearColor(0.008f, 0.025f, 0.035f, 0.0f);
		ScatteringInland->ParameterName = TEXT("WaterScatteringInland");
		ScatteringInland->DefaultValue = FLinearColor(0.05f, 0.07f, 0.05f, 0.0f);
		Exposure->R = false;
		Exposure->G = true;
		Exposure->B = false;
		Exposure->A = false;
		PhaseG->R = 0.1f;

		// raster.G = esposizione alle onde: 1 il mare, 0,35 la laguna, da 0,12
		// a 0,8 i laghi (i grandi sono limpidi come il mare), quasi 0 fiumi e
		// canali. Lo stesso numero sceglie quanto l'acqua e' "interna".
		Exposure->Input.Connect(RasterPixelRgba.Output, RasterPixelRgba.Node);
		G.Connect(Exposure, TEXT(""), Inland, TEXT(""));
		G.Connect(AbsorptionSea, TEXT(""), Absorption, TEXT("A"));
		G.Connect(AbsorptionInland, TEXT(""), Absorption, TEXT("B"));
		G.Connect(Inland, TEXT(""), Absorption, TEXT("Alpha"));
		G.Connect(ScatteringSea, TEXT(""), Scattering, TEXT("A"));
		G.Connect(ScatteringInland, TEXT(""), Scattering, TEXT("B"));
		G.Connect(Inland, TEXT(""), Scattering, TEXT("Alpha"));

		// L'uscita speciale di Single Layer Water: i suoi ingressi si collegano
		// per membro, non per nome del pin.
		WaterOutput->AbsorptionCoefficients.Connect(0, Absorption);
		WaterOutput->ScatteringCoefficients.Connect(0, Scattering);
		WaterOutput->PhaseG.Connect(0, PhaseG);
		// Dove c'e' schiuma non si vede attraverso.
		WaterOutput->ColorScaleBehindWater.Connect(0, BehindWater);

		if (!G.bOk) { return false; }
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return true;
	}
}

static FAutoConsoleCommand GeoImageryCreateMaterialCommand(
	TEXT("geo.Imagery.CreateMaterial"),
	TEXT("Costruisce (o RIFA') in /GeoWorld/Materials: M_GeoTerrain (foto e strade dipinte), M_GeoRoad (strade 3D), M_GeoWater e MPC_GeoWater (acqua)."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		const bool bTerrain = CreateOrRebuildMaterial(MaterialPackagePath, MaterialAssetName, &BuildDrapeGraph);
		const bool bRoad = CreateOrRebuildMaterial(RoadMaterialPackagePath, RoadMaterialAssetName, &BuildRoadGraph);

		// L'acqua: prima la collezione e la texture di riposo, che il
		// materiale referenzia; poi il materiale.
		const bool bWaterAssets = CreateOrUpdateWaveCollection() != nullptr && CreateOrLoadWaterDefaultTexture() != nullptr;
		const bool bWater = bWaterAssets
			&& CreateOrRebuildMaterial(WaterMaterialPackagePath, WaterMaterialAssetName, &BuildWaterGraph);

		if (bTerrain && bRoad && bWater)
		{
			Report(TEXT("Materiali pronti. Riavvia il Play (o rilancia geo.Roads.Demo / geo.Water.Demo)."));
		}
		if (!bTerrain)
		{
			Report(TEXT("M_GeoTerrain non fatto: puoi costruirlo a mano, docs/fase6-verifica.md"), FColor::Red);
		}
		if (!bWater)
		{
			Report(TEXT("M_GeoWater non fatto: guarda l'Output Log (categoria LogGeoWorld) e docs/fase9-verifica.md"),
				FColor::Red);
		}
	}));

#endif  // WITH_EDITOR
