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
			// Json/JsonUtilities serviranno per il manifest dei tile (Fase 2).
			"Json", "JsonUtilities",
		});
	}
}
