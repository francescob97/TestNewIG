// Regole di build di GeoCore.
using UnrealBuildTool;

public class GeoCore : ModuleRules
{
	public GeoCore(ReadOnlyTargetRules Target) : base(Target)
	{
		// UseExplicitOrSharedPCHs: ogni .cpp include cio' che usa, e UBT forza
		// l'inclusione di un PCH condiviso fra i moduli. E' la modalita' moderna
		// e la piu' veloce; l'alternativa (PCH per modulo) esiste solo per
		// codebase legacy.
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// DeveloperSettings serve per UGeoWorldSettings, che si auto-espone
			// in Project Settings senza scrivere una riga di UI.
			"DeveloperSettings",
		});

		// -------------------------------------------------------------------
		//  DIPENDENZA CONDIZIONALE DALL'EDITOR
		//
		//  GeoCore e' un modulo RUNTIME, quindi normalmente non potrebbe toccare
		//  UnrealEd (che nelle build di gioco non esiste nemmeno). Ma ci serve
		//  per una cosa sola: leggere e spostare la camera del VIEWPORT
		//  dell'editor, che non e' un attore e non e' raggiungibile dalle API
		//  runtime.
		//
		//  Il pattern corretto e' questo: dipendenza aggiunta solo quando si sta
		//  costruendo un target che contiene l'editor, e codice corrispondente
		//  chiuso in #if WITH_EDITOR. Senza questa accortezza il progetto compila
		//  nell'editor e si rompe al packaging, che e' il momento peggiore per
		//  scoprirlo.
		// -------------------------------------------------------------------
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
