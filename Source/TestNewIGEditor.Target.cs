// Target di EDITOR. E' quello che viene costruito quando premi "Build" in Visual
// Studio e poi apri il .uproject: include i moduli di tipo Editor (nel nostro
// caso GeoWorldEditor) che nel target di gioco non esistono nemmeno.
using UnrealBuildTool;

public class TestNewIGEditorTarget : TargetRules
{
	public TestNewIGEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("TestNewIG");
	}
}
