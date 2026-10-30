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
#include <utility>

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

	/**
	 * Vento: "7,2 m/s (14 kt) da 245°". La direzione e' quella DA CUI
	 * soffia, come nei bollettini; i nodi perche' li usa chi vola e chi va
	 * per mare.
	 */
	inline std::string Wind(double SpeedMs, double FromDegrees)
	{
		constexpr double KnotsPerMs = 3600.0 / 1852.0;
		return Number(SpeedMs, 1) + " m/s (" + Number(SpeedMs * KnotsPerMs, 0) + " kt) da " + Heading(FromDegrees);
	}

	/**
	 * Una distanza come la si dice: "350 m" (a decine), "1,2 km", "13 km";
	 * oltre 50 km (o infinita) "oltre 50 km": la visibilita' di un giorno
	 * limpido.
	 */
	inline std::string Distance(double Value)
	{
		if (!std::isfinite(Value) || Value > 50000.0) { return "oltre 50 km"; }
		if (Value < 995.0) { return Number(std::round(Value / 10.0) * 10.0, 0) + " m"; }
		return Number(Value / 1000.0, Value < 9950.0 ? 1 : 0) + " km";
	}

	/**
	 * Cosa cade e quanto: "pioggia 4,2 mm/h", "neve e pioggia 1,0 mm/h",
	 * "pioggia e grandine 35 mm/h", "niente". Si nomina ogni tipo che e'
	 * almeno il 10% del totale, il piu' abbondante per primo; i mm/h sono
	 * d'acqua (la neve fusa), come nei bollettini.
	 */
	inline std::string Precipitation(double RateMmH, double Rain, double Snow, double Hail)
	{
		if (!(RateMmH > 0.01)) { return "niente"; }
		struct FPart { const char* Name; double Fraction; };
		FPart Parts[3] = { { "pioggia", Rain }, { "neve", Snow }, { "grandine", Hail } };
		// Tre elementi: un ordinamento a mano, stabile (a pari frazione vince l'ordine sopra).
		for (int Pass = 0; Pass < 2; ++Pass)
		{
			for (int Index = 0; Index < 2 - Pass; ++Index)
			{
				if (Parts[Index + 1].Fraction > Parts[Index].Fraction) { std::swap(Parts[Index], Parts[Index + 1]); }
			}
		}
		std::string Names;
		for (const FPart& Part : Parts)
		{
			if (Part.Fraction < 0.1) { continue; }
			Names += Names.empty() ? Part.Name : std::string(" e ") + Part.Name;
		}
		if (Names.empty()) { Names = "precipitazione"; }
		return Names + " " + Number(RateMmH, RateMmH < 9.95 ? 1 : 0) + " mm/h";
	}
}
