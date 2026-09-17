// Formato tile, loader asincrono, cache LRU. Sostanza dalla Fase 2/3.
using UnrealBuildTool;

public class GeoTiles : ModuleRules
{
	public GeoTiles(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "GeoCore",
		});
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Json serve per manifest.json. L'indice per livello e gli heightmap
			// sono invece binari, letti a mano: un formato binario si parsa in
			// microsecondi, un JSON da 18 MB no.
			"Json", "JsonUtilities",
		});
	}
}
