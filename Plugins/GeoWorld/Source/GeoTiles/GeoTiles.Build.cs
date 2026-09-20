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

			// Fase 6: ImageWrapper porta il decoder JPEG del motore, che gira
			// sui thread di caricamento. Scrivere o importare un decoder JPEG
			// per non dipendere da un modulo del motore sarebbe purismo che
			// costa e non rende.
			"ImageWrapper",
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
