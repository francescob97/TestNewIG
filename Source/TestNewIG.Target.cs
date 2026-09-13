// Target di GIOCO (build standalone / packaged).
//
// NOTA UE per chi arriva da altri motori: i file *.Target.cs non sono C++ ma C#,
// e vengono compilati ed ESEGUITI da UnrealBuildTool (UBT) prima di compilare il
// C++. Descrivono COSA costruire (quali moduli, quali flag), non come.
// Un progetto C++ deve avere almeno un Target di gioco e uno di editor.
using UnrealBuildTool;

public class TestNewIGTarget : TargetRules
{
	public TestNewIGTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;

		// DefaultBuildSettings fissa il "dialetto" di UBT (quali warning sono
		// errori, quali macro sono attive). "Latest" adotta le impostazioni piu'
		// recenti dell'engine installato: e' cio' che vogliamo su un progetto nuovo,
		// perche' non trasciniamo compatibilita' legacy.
		DefaultBuildSettings = BuildSettingsVersion.Latest;

		// UE ha cambiato piu' volte l'ordine con cui gli header engine si includono
		// a vicenda. "Latest" = ordine moderno, che richiede include espliciti
		// (IWYU). E' piu' rigoroso ma tiene i tempi di build bassi.
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("TestNewIG");
	}
}
