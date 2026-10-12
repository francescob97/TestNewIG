// =============================================================================
//  GeoFormat.h -- Coordinate e quote come le legge una persona.
//  STRATO: C++ PURO, header-only.
//
//  Serve alle stampe a schermo (la posizione in alto a destra): gradi decimali,
//  gradi-primi-secondi, metri con il separatore delle migliaia. All'italiana:
//  la virgola per i decimali, il punto per le migliaia.
//
//  PERCHE' QUI E NON NELL'HUD. Sono funzioni piccole ma con trappole note: i
//  secondi che arrotondano a 60 ("45°03'60,0"" invece di "45°04'00,0""), il
//  segno delle longitudini ovest, lo zero negativo ("-0 m"). Nello strato puro
//  si provano nei test, fuori da Unreal.
//
//  Le stringhe sono UTF-8 (il simbolo del grado e' "\xC2\xB0"): chi le mette a
//  schermo in Unreal le converte con UTF8_TO_TCHAR.
// =============================================================================
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace GeoWorld::Format
{
	/** Il simbolo del grado, in UTF-8. */
	inline constexpr const char* Degree = "\xC2\xB0";

	/**
	 * Un numero all'italiana: "1.234,5". `Decimals` cifre dopo la virgola,
	 * arrotondate. Mai "-0": un valore che arrotonda a zero si scrive "0".
	 */
	inline std::string Number(double Value, int Decimals)
	{
		if (!std::isfinite(Value)) { return "--"; }
		const double Scale = std::pow(10.0, Decimals);
		double Rounded = std::round(std::fabs(Value) * Scale);
		const bool bNegative = Value < 0.0 && Rounded > 0.0;

		const double IntegerPart = std::floor(Rounded / Scale);
		const double FractionPart = Rounded - IntegerPart * Scale;

		char Digits[64];
		std::snprintf(Digits, sizeof(Digits), "%.0f", IntegerPart);
		std::string Integer(Digits);
		std::string Grouped;
		const int Length = static_cast<int>(Integer.size());
		for (int Index = 0; Index < Length; ++Index)
		{
			if (Index > 0 && (Length - Index) % 3 == 0) { Grouped += '.'; }
			Grouped += Integer[static_cast<size_t>(Index)];
		}

		std::string Result = bNegative ? "-" + Grouped : Grouped;
		if (Decimals > 0)
		{
			char Fraction[32];
			std::snprintf(Fraction, sizeof(Fraction), "%0*.0f", Decimals, FractionPart);
			Result += ',';
			Result += Fraction;
		}
		return Result;
	}

	/** Gradi decimali con l'emisfero: "45,07031° N", "7,68620° E". */
	inline std::string DecimalDegrees(double Degrees, bool bLatitude, int Decimals = 5)
	{
		const char* Hemisphere = bLatitude ? (Degrees < 0.0 ? "S" : "N") : (Degrees < 0.0 ? "O" : "E");
		return Number(std::fabs(Degrees), Decimals) + Degree + " " + Hemisphere;
	}

	/**
	 * Gradi, primi e secondi: "45°04'13,1" N". Si arrotonda PRIMA di separare
	 * i campi, sui decimi di secondo: altrimenti 45°03'59,96" diventerebbe
	 * 45°03'60,0", un orario che non esiste.
	 */
	inline std::string DegreesMinutesSeconds(double Degrees, bool bLatitude)
	{
		const char* Hemisphere = bLatitude ? (Degrees < 0.0 ? "S" : "N") : (Degrees < 0.0 ? "O" : "E");
		const long long Tenths = static_cast<long long>(std::llround(std::fabs(Degrees) * 36000.0));
		const long long WholeDegrees = Tenths / 36000;
		const long long Minutes = (Tenths / 600) % 60;
		const long long SecondsTenths = Tenths % 600;

		char Buffer[64];
		std::snprintf(Buffer, sizeof(Buffer), "%lld%s%02lld'%02lld,%lld\" %s",
			WholeDegrees, Degree, Minutes, SecondsTenths / 10, SecondsTenths % 10, Hemisphere);
		return Buffer;
	}

	/** Metri, interi sopra i 100 m, con un decimale sotto: "1.234 m", "12,3 m". */
	inline std::string Metres(double Value)
	{
		return Number(Value, std::fabs(Value) < 100.0 ? 1 : 0) + " m";
	}

	/** Rotta della bussola, 0..359: "273°". */
	inline std::string Heading(double Degrees)
	{
		double Wrapped = std::fmod(Degrees, 360.0);
		if (Wrapped < 0.0) { Wrapped += 360.0; }
		long long Rounded = std::llround(Wrapped);
		if (Rounded == 360) { Rounded = 0; }
		char Buffer[16];
		std::snprintf(Buffer, sizeof(Buffer), "%03lld", Rounded);
		return std::string(Buffer) + Degree;
	}
}
