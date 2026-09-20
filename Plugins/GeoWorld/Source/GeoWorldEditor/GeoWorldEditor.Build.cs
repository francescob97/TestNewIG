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

			// Fase 6: servono a costruire il materiale del drappeggio da codice.
			// MaterialEditor porta UMaterialEditingLibrary, AssetTools la
			// creazione dell'asset, UnrealEd il salvataggio su disco.
			"MaterialEditor", "AssetTools", "AssetRegistry",
		});
	}
}
