// =============================================================================
//  GeoConsoleArgs.h -- Argomenti dei comandi console con le virgolette.
//  STRATO: UNREAL, header-only.
//
//  NOTA UE: la console divide gli argomenti a OGNI spazio, anche dentro le
//  virgolette (FString::ParseIntoArrayWS). Un percorso come
//      geo.Terrain.Demo "C:\Unreal Projects\TestNewIG\Pipeline\dataset\dted"
//  arriverebbe in due pezzi, '"C:\Unreal' e 'Projects\...\dted"', e il
//  dataset non si aprirebbe. Qui si rimette insieme la riga e la si ridivide
//  rispettando le virgolette (FParse::Token), senza toccare le barre
//  rovesciate dei percorsi di Windows.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Misc/Parse.h"

namespace GeoWorld::Console
{
	inline TArray<FString> QuotedArgs(const TArray<FString>& RawArgs)
	{
		const FString Line = FString::Join(RawArgs, TEXT(" "));
		TArray<FString> Result;
		const TCHAR* Cursor = *Line;
		FString Token;
		// UseEscape = false: in "D:\dati\nuovi" la \n e' una cartella, non un a capo.
		while (FParse::Token(Cursor, Token, false))
		{
			Result.Add(Token);
		}
		return Result;
	}
}
