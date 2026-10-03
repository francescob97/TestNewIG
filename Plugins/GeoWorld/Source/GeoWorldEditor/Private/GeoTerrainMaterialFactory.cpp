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
// =============================================================================
#include "CoreMinimal.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "GeoCoreModule.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
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
