#include "Vegetation/GeoTreeModels.h"

#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "StaticMeshAttributes.h"

namespace GeoVegetationModels
{
	FName SlotName()
	{
		static const FName Name(TEXT("Vegetation"));
		return Name;
	}

	const TCHAR* SpeciesAssetName(int32 Species)
	{
		switch (static_cast<GeoWorld::Vegetation::ESpecies>(Species))
		{
		case GeoWorld::Vegetation::ESpecies::Broadleaf: return TEXT("Broadleaf");
		case GeoWorld::Vegetation::ESpecies::Conifer: return TEXT("Conifer");
		case GeoWorld::Vegetation::ESpecies::UmbrellaPine: return TEXT("UmbrellaPine");
		default: return TEXT("Shrub");
		}
	}

	FString DefaultPackagePath(int32 Species, int32 Variant)
	{
		return FString::Printf(TEXT("/GeoWorld/Vegetation/SM_GeoTree_%s_%d"), SpeciesAssetName(Species), Variant);
	}

	FString DefaultObjectPath(int32 Species, int32 Variant)
	{
		const FString Package = DefaultPackagePath(Species, Variant);
		return Package + TEXT(".") + FPaths::GetBaseFilename(Package);
	}

	void FillMeshDescription(const GeoWorld::Vegetation::FTreeMesh& Shape, bool bFlipWinding, FMeshDescription& Out)
	{
		FStaticMeshAttributes Attributes(Out);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colours = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
		Uvs.SetNumChannels(1);

		const FPolygonGroupID Group = Out.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = SlotName();

		const int32 VertexCount = static_cast<int32>(Shape.VertexCount());
		const int32 TriangleCount = static_cast<int32>(Shape.TriangleCount());
		Out.ReserveNewVertices(VertexCount);
		Out.ReserveNewVertexInstances(VertexCount);
		Out.ReserveNewTriangles(TriangleCount);

		TArray<FVertexInstanceID> Corners;
		Corners.Reserve(VertexCount);
		for (int32 Index = 0; Index < VertexCount; ++Index)
		{
			const FVertexID Vertex = Out.CreateVertex();
			Positions[Vertex] = FVector3f(Shape.Positions[3 * Index], Shape.Positions[3 * Index + 1], Shape.Positions[3 * Index + 2]);
			const FVertexInstanceID VertexInstance = Out.CreateVertexInstance(Vertex);
			const FVector3f Normal(Shape.Normals[3 * Index], Shape.Normals[3 * Index + 1], Shape.Normals[3 * Index + 2]);
			Normals[VertexInstance] = Normal;
			const FVector3f Helper = FMath::Abs(Normal.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
			Tangents[VertexInstance] = FVector3f::CrossProduct(Helper, Normal).GetSafeNormal();
			Signs[VertexInstance] = 1.0f;
			const FLinearColor Paint = FLinearColor::FromSRGBColor(FColor(Shape.Colours[4 * Index], Shape.Colours[4 * Index + 1],
				Shape.Colours[4 * Index + 2], Shape.Colours[4 * Index + 3]));
			Colours[VertexInstance] = FVector4f(Paint.R, Paint.G, Paint.B, Paint.A);
			Uvs.Set(VertexInstance, 0, FVector2f(0.0f, 0.0f));
			Corners.Add(VertexInstance);
		}
		for (int32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
		{
			FVertexInstanceID Corner[3] = {
				Corners[static_cast<int32>(Shape.Indices[3 * Triangle])],
				Corners[static_cast<int32>(Shape.Indices[3 * Triangle + 1])],
				Corners[static_cast<int32>(Shape.Indices[3 * Triangle + 2])] };
			if (bFlipWinding) { Swap(Corner[1], Corner[2]); }
			Out.CreateTriangle(Group, MakeArrayView(Corner, 3));
		}
	}
}
