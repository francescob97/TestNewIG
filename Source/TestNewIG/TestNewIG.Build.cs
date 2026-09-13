// Regole di build del modulo primario di gioco.
//
// PERCHE' ESISTE, visto che il progetto doveva essere un guscio vuoto:
// Unreal considera "progetto C++" solo un progetto che ha almeno un modulo di
// gioco con IMPLEMENT_PRIMARY_GAME_MODULE. Senza, il .uproject e' Blueprint-only
// e UBT non compila NEMMENO il C++ dei plugin. Quindi questo modulo esiste solo
// per rendere il progetto compilabile: non contiene e non conterra' logica.
using UnrealBuildTool;

public class TestNewIG : ModuleRules
{
	public TestNewIG(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}
