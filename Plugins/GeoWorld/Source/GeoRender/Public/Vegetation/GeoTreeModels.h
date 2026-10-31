// =============================================================================
//  GeoTreeModels.h -- Le forme degli alberi come mesh di Unreal, e dove stanno
//  i modelli. STRATO: UNREAL.
//
//  Lo usano in due:
//    - UGeoVegetationSubsystem, a runtime, quando un modello manca: costruisce
//      la forma al volo (due LOD, non Nanite);
//    - geo.Vegetation.CreateModels, nell'editor: salva le otto forme come
//      asset NANITE in /GeoWorld/Vegetation, dove UGeoVegetationSettings le
//      cerca di default.
//  Cosi' la traduzione FTreeMesh -> FMeshDescription e' scritta una volta.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Vegetation/TreeMesh.h"

struct FMeshDescription;

namespace GeoVegetationModels
{
	/** Il nome dello slot del materiale delle forme (e del gruppo di poligoni). */
	GEORENDER_API FName SlotName();

	/** "Broadleaf", "Conifer", "UmbrellaPine", "Shrub": i nomi degli asset. */
	GEORENDER_API const TCHAR* SpeciesAssetName(int32 Species);

	/** "/GeoWorld/Vegetation/SM_GeoTree_Conifer_1": il package del modello di default. */
	GEORENDER_API FString DefaultPackagePath(int32 Species, int32 Variant);

	/** "/GeoWorld/Vegetation/SM_GeoTree_Conifer_1.SM_GeoTree_Conifer_1": l'oggetto. */
	GEORENDER_API FString DefaultObjectPath(int32 Species, int32 Variant);

	/**
	 * Una forma di TreeMesh.h in una FMeshDescription, il formato con cui
	 * Unreal costruisce le static mesh (a runtime e nell'editor).
	 *
	 * NOTA UE sul colore dei vertici: la costruzione lo converte in FColor
	 * CON la codifica sRGB, e il nodo VertexColor del materiale restituisce poi
	 * quei byte divisi per 255. Per far arrivare al materiale esattamente i byte
	 * di TreeMesh.h (R chioma, G luce, B altezza) qui si passa il colore lineare
	 * che, ricodificato sRGB, torna quei byte: FromSRGBColor.
	 *
	 * La tangente: il materiale non ha normal map, ma il motore vuole una base
	 * completa; una qualunque perpendicolare alla normale va bene.
	 */
	GEORENDER_API void FillMeshDescription(const GeoWorld::Vegetation::FTreeMesh& Shape, bool bFlipWinding, FMeshDescription& Out);
}
