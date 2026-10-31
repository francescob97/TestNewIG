#include "Buildings/GeoBuildingSettings.h"

#include "Engine/StaticMesh.h"

bool UGeoBuildingSettings::ParseId(const FString& Id, uint64& OutOsmId, uint32& OutWikidata)
{
	OutOsmId = 0;
	OutWikidata = 0;
	const FString Text = Id.TrimStartAndEnd().ToLower();
	if (Text.IsEmpty()) { return false; }

	// Wikidata: "Q12345".
	if (Text.StartsWith(TEXT("q")) && Text.Len() > 1 && Text.Mid(1).IsNumeric())
	{
		const int64 Number = FCString::Atoi64(*Text.Mid(1));
		if (Number <= 0 || Number > MAX_uint32) { return false; }
		OutWikidata = static_cast<uint32>(Number);
		return true;
	}

	// OSM: "way/123", "relation/123", e le forme brevi "w123", "r123".
	uint64 Kind = 0;
	FString Number;
	if (Text.StartsWith(TEXT("way/"))) { Kind = 1; Number = Text.Mid(4); }
	else if (Text.StartsWith(TEXT("relation/"))) { Kind = 2; Number = Text.Mid(9); }
	else if (Text.StartsWith(TEXT("w"))) { Kind = 1; Number = Text.Mid(1); }
	else if (Text.StartsWith(TEXT("r"))) { Kind = 2; Number = Text.Mid(1); }
	if (Kind == 0 || Number.IsEmpty() || !Number.IsNumeric()) { return false; }
	const int64 Value = FCString::Atoi64(*Number);
	if (Value <= 0) { return false; }
	OutOsmId = (Kind << 62) | (static_cast<uint64>(Value) & ((1ull << 62) - 1));
	return true;
}
