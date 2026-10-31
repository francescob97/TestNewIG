// =============================================================================
//  VegetationCells.h -- Le celle degli alberi attorno alla camera. STRATO: C++
//  PURO, header-only.
//
//  Gli alberi non si mettono tutti insieme: si mettono a CELLE, le tile del
//  livello 15 (~610 x 430 m a 45°), solo quelle entro ~2,5 km dalla camera.
//  Una cella si costruisce su un worker (quale albero, dove, a che quota) e
//  diventa sul game thread qualche componente di istanze (uno per forma).
//
//  Qui c'e' tutto quello che non ha bisogno di Unreal, provato in
//  geovegetation_main.cpp (sezione 7):
//
//   - QUALI CELLE: quelle che hanno un punto entro il raggio, dalla piu'
//     vicina (CellsAround). Contando anche l'altezza sopra il suolo: da 5 km
//     di quota non se ne costruisce nessuna.
//   - LE BANDE: vicino (tutti gli alberi, con l'ombra), medio (la meta'),
//     lontano (un quarto). Per cambiare banda si deve superare il confine di
//     qualche decina di metri (l'ISTERESI): senza, una cella proprio sul
//     confine cambierebbe a ogni passo della camera, e gli alberi
//     comparirebbero e sparirebbero a scatti.
//   - IL SUOLO: gli alberi stanno sul terreno DISEGNATO (FSurfaceSampler),
//     cioe' sulle tile visibili adesso. La FIRMA di una cella dice su quali
//     tile e' stata costruita: quando il terreno sotto si affina la firma
//     cambia, e la cella si rifa' sul terreno nuovo. Senza, gli alberi
//     resterebbero sospesi (o sepolti) sulla forma vecchia.
//   - LE ISTANZE: l'albero di TreePlacement.h nel frame della cella, in
//     metri, con quello che serve al materiale (il verde, l'altezza, il caso).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "Mesh/GroundCells.h"
#include "Mesh/TileMesh.h"
#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"
#include "Tiles/VegetationTileFormat.h"
#include "Vegetation/TreePlacement.h"

namespace GeoWorld::Vegetation
{
	/** Il livello delle celle: ~610 m nord-sud, ~430 m est-ovest a 45°. */
	inline constexpr uint32_t CellLevel = 15;

	// Le celle, le bande e il suolo disegnato stanno in Mesh/GroundCells.h,
	// insieme agli edifici; qui i nomi restano quelli di sempre.
	using Cells::BandOutside;
	using Cells::FBandSettings;
	using Cells::DistanceToCellM;
	using Cells::BandFor;
	using Cells::FCellRef;
	using Cells::CellsAround;
	using Cells::SurfaceSignature;
	using Cells::BoundsOverlap;
	using Cells::FGroundSurfaces;

	/**
	 * Un albero pronto per il componente di istanze: nel frame della cella
	 * (nord, est, alto, m) con quello che serve al materiale.
	 */
	struct FTreeInstance
	{
		float Local[3] = { 0.0f, 0.0f, 0.0f };
		float YawRad = 0.0f, WidthM = 0.0f, HeightM = 0.0f;
		float Colour[3] = { 0.0f, 0.0f, 0.0f };
		float Random = 0.0f;
		float Thin = 0.0f;
		uint8_t Species = 0;
		uint8_t Variant = 0;
	};

	/** I dati per istanza del materiale (Per Instance Custom Data): il verde, l'altezza, il caso. */
	inline constexpr int32_t InstanceDataFloats = 5;

	inline void InstanceData(const FTreeInstance& Tree, float Out[InstanceDataFloats])
	{
		Out[0] = Tree.Colour[0];
		Out[1] = Tree.Colour[1];
		Out[2] = Tree.Colour[2];
		Out[3] = Tree.HeightM;
		Out[4] = Tree.Random;
	}

	inline void MakeInstances(const std::vector<FTree>& Trees, const FCellFrame& Frame, std::vector<FTreeInstance>& Out)
	{
		Out.clear();
		Out.reserve(Trees.size());
		for (const FTree& Tree : Trees)
		{
			double Local[3];
			Frame.Local(Tree.LatDeg, Tree.LonDeg, Tree.GroundM, Local);
			FTreeInstance Instance;
			for (int32_t Axis = 0; Axis < 3; ++Axis) { Instance.Local[Axis] = static_cast<float>(Local[Axis]); }
			Instance.YawRad = Tree.YawRad;
			Instance.WidthM = Tree.WidthM;
			Instance.HeightM = Tree.HeightM;
			for (int32_t Channel = 0; Channel < 3; ++Channel) { Instance.Colour[Channel] = Tree.Colour[Channel]; }
			Instance.Random = Tree.Random;
			Instance.Thin = Tree.Thin;
			Instance.Species = static_cast<uint8_t>(Tree.Species);
			Instance.Variant = Tree.Variant;
			Out.push_back(Instance);
		}
	}

	/**
	 * Tutto il lavoro di una cella, quello del worker: chi cresce dove
	 * (PlaceTrees, sulla copertura del suolo e sul terreno disegnato) e le
	 * istanze nel frame della cella. Il frame e' quello di FCellFrame::At sul
	 * centro della cella: il componente ha la sua trasformazione.
	 */
	inline void BuildCellInstances(const Tiles::FTileBounds& Cell, const FPlacementSettings& Settings,
	                               const Tiles::FVegetationTile& Cover, const FGroundSurfaces& Ground, double GeoidM,
	                               std::vector<FTreeInstance>& Out)
	{
		std::vector<FTree> Trees;
		PlaceTrees(Cell.South, Cell.West, Cell.North, Cell.East, Settings,
			[&Cover](double LatDeg, double LonDeg) { return Cover.ClassAt(LatDeg, LonDeg); },
			[&Ground](double LatDeg, double LonDeg, double& OutHeightM, double& OutSlopeDeg)
			{
				return Ground.GroundAt(LatDeg, LonDeg, OutHeightM, OutSlopeDeg);
			},
			GeoidM, Trees);
		MakeInstances(Trees, FCellFrame::At(Cell.CentreLat(), Cell.CentreLon()), Out);
	}

	/** La forma (specie, variante) di un albero: l'indice del suo componente nella cella. */
	inline int32_t ShapeIndex(uint8_t Species, uint8_t Variant)
	{
		return static_cast<int32_t>(Species) * VariantCount + std::min<int32_t>(Variant, VariantCount - 1);
	}
	inline constexpr int32_t ShapeCount = SpeciesCount * VariantCount;
}
