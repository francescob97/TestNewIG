// Fase 7: le entita' (aerei, veicoli, navi) che arrivano da fuori.
//
// Due strati, come gli altri moduli:
//   - Public/Entities: C++ PURO, header-only (lo stato canonico, le
//     conversioni di frame, l'estrapolazione, il registro, i modelli, il
//     formato della memoria condivisa), testato da
//     Tools/StandaloneTests/geoentities_main.cpp senza Unreal;
//   - il resto: il subsystem, gli attori, le sorgenti (i thread che leggono).
//
// Gli adattatori dei protocolli pesanti (HLA, che vuole un RTI di terze
// parti) staranno in moduli SEPARATI e opzionali (docs/fase7-design.md,
// sezione 9): questo modulo non deve mai richiedere niente che non sia nel
// motore.
using UnrealBuildTool;

public class GeoEntities : ModuleRules
{
	public GeoEntities(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "GeoCore", "GeoTiles", "GeoRender",
			// Project Settings -> GeoWorld - Entita'.
			"DeveloperSettings",
		});

		// I modelli da file (docs/modelli-design.md): le mesh costruite a
		// runtime (FMeshDescription con gli attributi delle static mesh) e le
		// texture decodificate da Unreal (PNG, JPEG, BMP, TGA...).
		PrivateDependencyModuleNames.AddRange(new string[] { "MeshDescription", "StaticMeshDescription", "ImageWrapper" });
	}
}
