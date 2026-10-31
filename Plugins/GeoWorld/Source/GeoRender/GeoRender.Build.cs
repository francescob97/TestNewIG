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

			// La libreria dei modelli degli alberi sta in Project Settings:
			// UDeveloperSettings vive in questo modulo.
			"DeveloperSettings",
		});

		// La grandine al suolo aggiorna una texture sul posto
		// (UpdateTextureRegions): la regione da copiare e' un tipo dell'RHI.
		PrivateDependencyModuleNames.AddRange(new string[] { "RHI" });

		// La vegetazione costruisce le forme degli alberi come UStaticMesh a
		// runtime: FMeshDescription (MeshDescription) con gli attributi delle
		// static mesh (StaticMeshDescription), poi BuildFromMeshDescriptions.
		PrivateDependencyModuleNames.AddRange(new string[] { "MeshDescription", "StaticMeshDescription" });
	}
}
