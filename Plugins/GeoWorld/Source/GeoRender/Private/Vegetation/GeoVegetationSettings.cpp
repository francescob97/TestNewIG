#include "Vegetation/GeoVegetationSettings.h"

#include "Engine/StaticMesh.h"
#include "Vegetation/GeoTreeModels.h"
#include "Vegetation/TreePlacement.h"

UGeoVegetationSettings::UGeoVegetationSettings()
{
	// Di default: i modelli Nanite di geo.Vegetation.CreateModels. Finche' non
	// ci sono, il runtime usa le forme fatte dal codice.
	TSoftObjectPtr<UStaticMesh>* Slots[] = { &Broadleaf0, &Broadleaf1, &Conifer0, &Conifer1,
	                                          &UmbrellaPine0, &UmbrellaPine1, &Shrub0, &Shrub1 };
	for (int32 Species = 0; Species < GeoWorld::Vegetation::SpeciesCount; ++Species)
	{
		for (int32 Variant = 0; Variant < GeoWorld::Vegetation::VariantCount; ++Variant)
		{
			*Slots[Species * GeoWorld::Vegetation::VariantCount + Variant] =
				TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(GeoVegetationModels::DefaultObjectPath(Species, Variant)));
		}
	}
}

TSoftObjectPtr<UStaticMesh> UGeoVegetationSettings::GetModel(int32 Species, int32 Variant) const
{
	const TSoftObjectPtr<UStaticMesh>* Slots[] = { &Broadleaf0, &Broadleaf1, &Conifer0, &Conifer1,
	                                                &UmbrellaPine0, &UmbrellaPine1, &Shrub0, &Shrub1 };
	const int32 Index = FMath::Clamp(Species, 0, GeoWorld::Vegetation::SpeciesCount - 1) * GeoWorld::Vegetation::VariantCount
		+ FMath::Clamp(Variant, 0, GeoWorld::Vegetation::VariantCount - 1);
	return *Slots[Index];
}
