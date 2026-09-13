// Modulo di tipo Editor: strumenti, visualizzatori, importazione.
// Non viene compilato nei target di gioco.
using UnrealBuildTool;

public class GeoWorldEditor : ModuleRules
{
	public GeoWorldEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine",
		});
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd", "Slate", "SlateCore", "GeoCore", "GeoRender",
		});
	}
}
