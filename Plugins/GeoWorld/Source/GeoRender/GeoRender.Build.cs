// Quadtree, selezione LOD, generazione mesh. Sostanza dalla Fase 4/5.
// In Fase 1 contiene solo gli attori di verifica visiva.
using UnrealBuildTool;

public class GeoRender : ModuleRules
{
	public GeoRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "GeoCore", "GeoTiles",

			// Fase 5: GeometryFramework porta UDynamicMeshComponent,
			// GeometryCore la FDynamicMesh3 che il componente contiene.
			// Sono il provider di PARTENZA: l'interfaccia in
			// Terrain/GeoTerrainMeshProvider.h esiste per poterlo sostituire
			// con un FPrimitiveSceneProxy custom senza toccare il resto.
			"GeometryFramework", "GeometryCore",
		});
	}
}
