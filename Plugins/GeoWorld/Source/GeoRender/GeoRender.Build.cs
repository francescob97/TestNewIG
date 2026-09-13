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
			// NOTA: GeometryFramework (UDynamicMeshComponent) verra' aggiunto in
			// Fase 5. Non lo mettiamo ora per non pagare tempo di compilazione
			// per un modulo che ancora non usiamo.
		});
	}
}
