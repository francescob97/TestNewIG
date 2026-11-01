// =============================================================================
//  Stars.h -- Le stelle vere e i pianeti. STRATO: C++ PURO, header-only.
//
//  Fin qui le stelle erano finte (Sky/NightSkyShader.inl): una griglia sul
//  cielo con una stella a caso in qualche cella, con la statistica giusta ma
//  senza le costellazioni. Qui ci sono quelle vere e i pianeti, provati in
//  geosky_main.cpp (sezioni "stelle" e "pianeti"):
//
//   1. IL CATALOGO: il file .gst che scrive `geoworld build-stars` dal
//      catalogo di Yale (9.110 stelle fino alla magnitudine 6,5, tutte quelle
//      a occhio nudo): posizione J2000, magnitudine, indice di colore B-V.
//   2. LA PRECESSIONE: l'asse della Terra gira come una trottola, un giro in
//      26.000 anni. Le posizioni del catalogo sono quelle del 2000; nel 2026
//      il cielo si e' spostato di un terzo di grado (Meeus cap. 21).
//   3. IL CIELO DA GIRARE: le stelle stanno ferme in un componente di Unreal,
//      e si gira il componente (SkyFieldRotation). Una trappola: il cielo
//      equatoriale e' destrorso, il mondo di Unreal (nord, est, alto) no; il
//      passaggio e' una riflessione, che un quaternione non sa fare. Le
//      stelle si mettono in un cielo SPECCHIATO (y cambiata di segno): da li'
//      al mondo e' una rotazione vera.
//   4. IL COLORE: dal B-V la temperatura (Ballesteros), dalla temperatura il
//      colore di un corpo nero, smorzato: a occhio le stelle sono quasi bianche.
//   5. QUANTE SE NE VEDONO: la magnitudine limite, dal crepuscolo e dalla luna.
//   6. I PIANETI: Mercurio, Venere, Marte, Giove, Saturno, dagli elementi
//      orbitali approssimati del JPL (Standish, validi dal 1800 al 2050, pochi
//      primi d'arco); la magnitudine dalle formule dell'Astronomical Almanac
//      (Meeus cap. 41), con gli anelli di Saturno (cap. 45).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Sky/Astronomy.h"

namespace GeoWorld::Sky
{
	// =========================================================================
	//  Il catalogo (.gst)
	// =========================================================================

	struct FCatalogStar
	{
		/** Ascensione retta e declinazione all'equinozio J2000, gradi. */
		float RaDeg = 0.0f;
		float DecDeg = 0.0f;
		/** La magnitudine visuale: piu' piccola = piu' luminosa (Sirio -1,46, la Polare 2,02). */
		float Magnitude = 6.0f;
		/** L'indice di colore B-V: 0 bianco-azzurro (Vega), 0,65 il sole, 1,85 arancio (Betelgeuse). */
		float ColourIndex = 0.65f;
	};

	inline constexpr uint32_t StarCatalogMagic = 0x54535747u;   // "GWST" little-endian
	inline constexpr uint16_t StarCatalogVersion = 1;
	inline constexpr size_t StarCatalogHeaderBytes = 16;
	inline constexpr size_t StarCatalogRecordBytes = 12;

	/** Legge un file .gst (Pipeline/geoworld/starbuild.py). Falso con OutError se non va. */
	inline bool ParseStarCatalog(const uint8_t* Bytes, size_t Count, std::vector<FCatalogStar>& Out, const char*& OutError)
	{
		OutError = nullptr;
		Out.clear();
		auto U16 = [Bytes](size_t At) { return static_cast<uint32_t>(Bytes[At] | (Bytes[At + 1] << 8)); };
		auto U32 = [Bytes](size_t At)
		{
			return static_cast<uint32_t>(Bytes[At]) | (static_cast<uint32_t>(Bytes[At + 1]) << 8)
			     | (static_cast<uint32_t>(Bytes[At + 2]) << 16) | (static_cast<uint32_t>(Bytes[At + 3]) << 24);
		};
		if (Count < StarCatalogHeaderBytes) { OutError = "file piu' corto dell'header"; return false; }
		if (U32(0) != StarCatalogMagic) { OutError = "non e' un catalogo di stelle (magic diverso da GWST)"; return false; }
		if (U16(4) != StarCatalogVersion) { OutError = "versione del catalogo non gestita"; return false; }
		const uint32_t Stars = U32(8);
		if (U32(12) != 2000u) { OutError = "equinozio diverso da J2000"; return false; }
		if (static_cast<uint64_t>(Count) != StarCatalogHeaderBytes + static_cast<uint64_t>(Stars) * StarCatalogRecordBytes)
		{
			OutError = "dimensione del file incoerente con il numero di stelle";
			return false;
		}
		Out.reserve(Stars);
		for (uint32_t Index = 0; Index < Stars; ++Index)
		{
			const size_t At = StarCatalogHeaderBytes + static_cast<size_t>(Index) * StarCatalogRecordBytes;
			FCatalogStar Star;
			uint32_t Ra = U32(At), Dec = U32(At + 4);
			std::memcpy(&Star.RaDeg, &Ra, 4);
			std::memcpy(&Star.DecDeg, &Dec, 4);
			Star.Magnitude = static_cast<float>(static_cast<int16_t>(U16(At + 8))) * 0.01f;
			Star.ColourIndex = static_cast<float>(static_cast<int16_t>(U16(At + 10))) * 0.001f;
			if (!std::isfinite(Star.RaDeg) || !std::isfinite(Star.DecDeg) || Star.DecDeg < -90.0f || Star.DecDeg > 90.0f)
			{
				OutError = "una stella ha una posizione impossibile: file rovinato";
				Out.clear();
				return false;
			}
			Out.push_back(Star);
		}
		return true;
	}

	// =========================================================================
	//  Vettori e precessione
	// =========================================================================

	/** Il versore dell'ascensione retta e declinazione (x verso il punto vernale, z verso il polo nord celeste). */
	inline void EquatorialUnit(double RaDeg, double DecDeg, double Out[3])
	{
		const double Ra = RaDeg * DegToRad, Dec = DecDeg * DegToRad;
		Out[0] = std::cos(Dec) * std::cos(Ra);
		Out[1] = std::cos(Dec) * std::sin(Ra);
		Out[2] = std::sin(Dec);
	}

	inline void UnitToEquatorial(const double V[3], double& OutRaDeg, double& OutDecDeg)
	{
		const double Length = std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
		OutRaDeg = AstronomyDetail::Wrap360(std::atan2(V[1], V[0]) * RadToDeg);
		OutDecDeg = std::asin(std::max(-1.0, std::min(1.0, V[2] / Length))) * RadToDeg;
	}

	/**
	 * La precessione dall'equinozio J2000 a quello della data (TT), Meeus 21.3
	 * (la formula rigorosa, con zeta, z e theta). Come matrice: Out per un
	 * versore J2000 da' il versore della data. Le colonne sono le immagini dei
	 * tre assi, calcolate con la formula del libro.
	 */
	inline void PrecessionMatrix(double JdTt, double Out[3][3])
	{
		const double T = (JdTt - J2000) / 36525.0;
		const double Zeta = (2306.2181 * T + 0.30188 * T * T + 0.017998 * T * T * T) / 3600.0 * DegToRad;
		const double Z = (2306.2181 * T + 1.09468 * T * T + 0.018203 * T * T * T) / 3600.0 * DegToRad;
		const double Theta = (2004.3109 * T - 0.42665 * T * T - 0.041833 * T * T * T) / 3600.0 * DegToRad;
		const double Axes[3][2] = { { 0.0, 0.0 }, { 90.0, 0.0 }, { 0.0, 90.0 } };
		for (int32_t Column = 0; Column < 3; ++Column)
		{
			const double Ra0 = Axes[Column][0] * DegToRad, Dec0 = Axes[Column][1] * DegToRad;
			const double A = std::cos(Dec0) * std::sin(Ra0 + Zeta);
			const double B = std::cos(Theta) * std::cos(Dec0) * std::cos(Ra0 + Zeta) - std::sin(Theta) * std::sin(Dec0);
			const double C = std::sin(Theta) * std::cos(Dec0) * std::cos(Ra0 + Zeta) + std::cos(Theta) * std::sin(Dec0);
			const double Ra = std::atan2(A, B) + Z;
			const double Dec = std::asin(std::max(-1.0, std::min(1.0, C)));
			Out[0][Column] = std::cos(Dec) * std::cos(Ra);
			Out[1][Column] = std::cos(Dec) * std::sin(Ra);
			Out[2][Column] = std::sin(Dec);
		}
	}

	/** Una posizione J2000 portata all'equinozio della data. */
	inline void Precess(double RaDeg, double DecDeg, double JdTt, double& OutRaDeg, double& OutDecDeg)
	{
		double P[3][3], V[3], W[3];
		PrecessionMatrix(JdTt, P);
		EquatorialUnit(RaDeg, DecDeg, V);
		for (int32_t Row = 0; Row < 3; ++Row) { W[Row] = P[Row][0] * V[0] + P[Row][1] * V[1] + P[Row][2] * V[2]; }
		UnitToEquatorial(W, OutRaDeg, OutDecDeg);
	}

	// =========================================================================
	//  Il cielo da girare
	// =========================================================================

	/** Il versore J2000 nel cielo SPECCHIATO (y cambiata di segno): quello delle istanze in Unreal. */
	inline void MirroredUnit(double RaDeg, double DecDeg, double Out[3])
	{
		EquatorialUnit(RaDeg, DecDeg, Out);
		Out[1] = -Out[1];
	}

	/**
	 * La rotazione dal cielo specchiato J2000 al cielo locale (nord, est, alto)
	 * di un osservatore: specchio, precessione, poi dall'equatoriale all'orizzonte
	 * (la trasposta di HorizonToEquatorial). Due riflessioni fanno una rotazione
	 * vera (determinante +1): Unreal la fa con un quaternione.
	 * LocalSiderealDeg: il tempo siderale locale (FSkyBodies::LocalSiderealDeg).
	 */
	inline void SkyFieldRotation(double LatDeg, double LocalSiderealDeg, double JdTt, double Out[3][3])
	{
		double ToEquator[3][3], P[3][3];
		HorizonToEquatorial(LatDeg, LocalSiderealDeg, ToEquator);
		PrecessionMatrix(JdTt, P);
		for (int32_t Row = 0; Row < 3; ++Row)
		{
			for (int32_t Column = 0; Column < 3; ++Column)
			{
				// (ToEquator^T * P)[Row][Column], poi la colonna y cambia segno (lo specchio).
				double Sum = 0.0;
				for (int32_t K = 0; K < 3; ++K) { Sum += ToEquator[K][Row] * P[K][Column]; }
				Out[Row][Column] = (Column == 1) ? -Sum : Sum;
			}
		}
	}

	/** Altezza e azimut di un versore del cielo locale (nord, est, alto). */
	inline FHorizontal HorizontalOf(const double Local[3])
	{
		FHorizontal Out;
		Out.AltitudeDeg = std::asin(std::max(-1.0, std::min(1.0, Local[2]))) * RadToDeg;
		Out.AzimuthDeg = AstronomyDetail::Wrap360(std::atan2(Local[1], Local[0]) * RadToDeg);
		return Out;
	}

	// =========================================================================
	//  Il colore e quante se ne vedono
	// =========================================================================

	/** La temperatura della superficie dall'indice di colore (Ballesteros 2012), kelvin. */
	inline double ColourTemperature(double ColourIndex)
	{
		const double Bv = std::max(-0.4, std::min(2.0, ColourIndex));
		return 4600.0 * (1.0 / (0.92 * Bv + 1.7) + 1.0 / (0.92 * Bv + 0.62));
	}

	namespace StarDetail
	{
		inline double SrgbToLinear(double Value)
		{
			const double C = std::max(0.0, std::min(1.0, Value));
			return C <= 0.04045 ? C / 12.92 : std::pow((C + 0.055) / 1.055, 2.4);
		}
	}

	/**
	 * Il colore di una stella, in luce LINEARE, con la luminanza uguale a 1:
	 * il corpo nero alla sua temperatura (l'approssimazione di Helland, in
	 * sRGB), smorzato a meta' verso il bianco (a occhio, di notte, i colori si
	 * vedono appena).
	 */
	inline void StarColour(double ColourIndex, float Out[3])
	{
		const double T = ColourTemperature(ColourIndex) * 0.01;      // l'approssimazione lavora in centinaia di kelvin (non-unita)
		double R = 255.0, G = 255.0, B = 255.0;
		if (T > 66.0)
		{
			R = 329.698727446 * std::pow(T - 60.0, -0.1332047592);
			G = 288.1221695283 * std::pow(T - 60.0, -0.0755148492);
		}
		else
		{
			G = 99.4708025861 * std::log(T) - 161.1195681661;
			B = T <= 19.0 ? 0.0 : 138.5177312231 * std::log(T - 10.0) - 305.0447927307;
		}
		double Rgb[3] = { StarDetail::SrgbToLinear(R / 255.0), StarDetail::SrgbToLinear(G / 255.0), StarDetail::SrgbToLinear(B / 255.0) };
		for (double& Channel : Rgb) { Channel = 0.5 * Channel + 0.5 * 1.0; }
		const double Luminance = 0.2126 * Rgb[0] + 0.7152 * Rgb[1] + 0.0722 * Rgb[2];
		for (int32_t Channel = 0; Channel < 3; ++Channel) { Out[Channel] = static_cast<float>(Rgb[Channel] / Luminance); }
	}

	/**
	 * La magnitudine delle stelle piu' deboli che si vedono: a occhio nudo, in
	 * un cielo buio, 6. Nel crepuscolo meno (a -6 gradi, la fine del crepuscolo
	 * civile, solo le stelle di prima grandezza); con il sole sopra
	 * l'orizzonte quasi nessuna (Venere, a volte). La luna piena alta toglie
	 * piu' di una magnitudine. MoonLight: 0 senza luna, 1 la luna piena alta.
	 */
	inline double LimitingMagnitude(double SunAltitudeDeg, double MoonLight)
	{
		static const double Points[6][2] = { { 3.0, -6.0 }, { 0.0, -4.5 }, { -3.0, -1.5 }, { -6.0, 1.5 }, { -12.0, 4.5 }, { -18.0, 6.0 } };
		double Twilight = 6.0;
		if (SunAltitudeDeg >= Points[0][0]) { Twilight = Points[0][1]; }
		else
		{
			for (int32_t Index = 1; Index < 6; ++Index)
			{
				if (SunAltitudeDeg >= Points[Index][0])
				{
					const double F = (SunAltitudeDeg - Points[Index][0]) / (Points[Index - 1][0] - Points[Index][0]);
					Twilight = Points[Index][1] + (Points[Index - 1][1] - Points[Index][1]) * F;
					break;
				}
			}
		}
		const double Moon = 6.0 - 1.2 * std::max(0.0, std::min(1.0, MoonLight));
		return std::min(Twilight, Moon);
	}

	// =========================================================================
	//  I pianeti
	// =========================================================================

	enum class EPlanet : uint8_t { Mercury = 0, Venus, Mars, Jupiter, Saturn };
	inline constexpr int32_t PlanetCount = 5;

	inline const char* PlanetName(EPlanet Planet)
	{
		switch (Planet)
		{
		case EPlanet::Mercury: return "Mercurio";
		case EPlanet::Venus:   return "Venere";
		case EPlanet::Mars:    return "Marte";
		case EPlanet::Jupiter: return "Giove";
		default:               return "Saturno";
		}
	}

	/** Un pianeta visto dalla Terra. */
	struct FPlanet
	{
		/** Ascensione retta e declinazione geocentriche, equinozio J2000, gradi (con il tempo della luce). */
		double RaDeg = 0.0, DecDeg = 0.0;
		/** Distanza dalla Terra e dal sole, unita' astronomiche. */
		double DistanceAu = 1.0, SunDistanceAu = 1.0;
		/** L'angolo sole-pianeta-Terra (la fase) e quello sole-Terra-pianeta (l'elongazione), gradi. */
		double PhaseAngleDeg = 0.0, ElongationDeg = 0.0;
		double Magnitude = 0.0;
		/** L'inclinazione degli anelli verso la Terra (solo Saturno), gradi. */
		double RingTiltDeg = 0.0;
	};

	namespace PlanetDetail
	{
		/**
		 * Gli elementi orbitali approssimati del JPL (E. M. Standish, "Keplerian
		 * Elements for Approximate Positions of the Major Planets", tabella 1,
		 * dal 1800 al 2050), sull'eclittica e l'equinozio J2000: semiasse (UA),
		 * eccentricita', inclinazione, longitudine media, longitudine del
		 * perielio, longitudine del nodo (gradi); e quanto cambiano in un secolo.
		 */
		struct FElements { double A, E, I, L, Perihelion, Node; };
		inline constexpr FElements Elements[6][2] = {
			{ { 0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593 },
			  { 0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081 } },
			{ { 0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255 },
			  { 0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418 } },
			{ { 1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891 },
			  { 0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343 } },
			{ { 5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909 },
			  { -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106 } },
			{ { 9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448 },
			  { -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794 } },
			// La Terra (il baricentro Terra-Luna: a qualche migliaio di km dalla Terra, trascurabile).
			{ { 1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0 },
			  { 0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0 } },
		};
		inline constexpr int32_t EarthIndex = 5;
		/** L'obliquita' dell'eclittica J2000, gradi. */
		inline constexpr double ObliquityJ2000 = 23.43928;
		/** La luce percorre un'unita' astronomica in tanti giorni. */
		inline constexpr double LightDaysPerAu = 0.0057755183;

		/** La posizione eliocentrica sull'eclittica J2000 (UA) all'istante TT. */
		inline void Heliocentric(int32_t Body, double JdTt, double Out[3])
		{
			const double T = (JdTt - J2000) / 36525.0;
			const FElements& Base = Elements[Body][0];
			const FElements& Rate = Elements[Body][1];
			const double A = Base.A + Rate.A * T;
			const double E = Base.E + Rate.E * T;
			const double I = (Base.I + Rate.I * T) * DegToRad;
			const double L = Base.L + Rate.L * T;
			const double Perihelion = Base.Perihelion + Rate.Perihelion * T;
			const double Node = (Base.Node + Rate.Node * T) * DegToRad;
			const double Omega = Perihelion * DegToRad - Node;
			const double M = AstronomyDetail::Wrap180(L - Perihelion) * DegToRad;

			// L'equazione di Keplero, con Newton: E - e sin E = M.
			double Eccentric = M + E * std::sin(M);
			for (int32_t Iteration = 0; Iteration < 12; ++Iteration)
			{
				const double Step = (M - (Eccentric - E * std::sin(Eccentric))) / (1.0 - E * std::cos(Eccentric));
				Eccentric += Step;
				if (std::fabs(Step) < 1.0e-12) { break; }
			}
			const double X = A * (std::cos(Eccentric) - E);
			const double Y = A * std::sqrt(1.0 - E * E) * std::sin(Eccentric);
			const double CosW = std::cos(Omega), SinW = std::sin(Omega);
			const double CosN = std::cos(Node), SinN = std::sin(Node);
			const double CosI = std::cos(I), SinI = std::sin(I);
			Out[0] = (CosW * CosN - SinW * SinN * CosI) * X + (-SinW * CosN - CosW * SinN * CosI) * Y;
			Out[1] = (CosW * SinN + SinW * CosN * CosI) * X + (-SinW * SinN + CosW * CosN * CosI) * Y;
			Out[2] = (SinW * SinI) * X + (CosW * SinI) * Y;
		}

		inline double Length(const double V[3]) { return std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]); }

		inline double AngleDeg(double CosValue) { return std::acos(std::max(-1.0, std::min(1.0, CosValue))) * RadToDeg; }
	}

	/**
	 * Un pianeta visto dal centro della Terra all'istante JdTt (TT): dove, a
	 * che distanza, con che fase e che magnitudine. La posizione tiene conto
	 * del tempo della luce (Giove si vede dove era 35-50 minuti prima).
	 */
	inline FPlanet ObservePlanet(EPlanet Planet, double JdTt)
	{
		using namespace PlanetDetail;
		const int32_t Body = static_cast<int32_t>(Planet);
		double Earth[3], Helio[3], Geo[3];
		Heliocentric(EarthIndex, JdTt, Earth);
		Heliocentric(Body, JdTt, Helio);
		for (int32_t Axis = 0; Axis < 3; ++Axis) { Geo[Axis] = Helio[Axis] - Earth[Axis]; }
		// Il tempo della luce: il pianeta dove era quando la luce e' partita.
		Heliocentric(Body, JdTt - Length(Geo) * LightDaysPerAu, Helio);
		for (int32_t Axis = 0; Axis < 3; ++Axis) { Geo[Axis] = Helio[Axis] - Earth[Axis]; }

		FPlanet Out;
		const double R = Length(Helio), Delta = Length(Geo), Sun = Length(Earth);
		Out.DistanceAu = Delta;
		Out.SunDistanceAu = R;
		Out.PhaseAngleDeg = AngleDeg((R * R + Delta * Delta - Sun * Sun) / (2.0 * R * Delta));
		Out.ElongationDeg = AngleDeg((Sun * Sun + Delta * Delta - R * R) / (2.0 * Sun * Delta));

		// Dall'eclittica all'equatore J2000.
		const double Epsilon = ObliquityJ2000 * DegToRad;
		const double Equatorial[3] = { Geo[0], Geo[1] * std::cos(Epsilon) - Geo[2] * std::sin(Epsilon),
		                               Geo[1] * std::sin(Epsilon) + Geo[2] * std::cos(Epsilon) };
		UnitToEquatorial(Equatorial, Out.RaDeg, Out.DecDeg);

		// La magnitudine (Meeus 41, coefficienti dell'Astronomical Almanac 1984).
		const double I = Out.PhaseAngleDeg;
		const double Distance = 5.0 * std::log10(R * Delta);
		switch (Planet)
		{
		case EPlanet::Mercury: Out.Magnitude = -0.42 + Distance + 0.0380 * I - 0.000273 * I * I + 0.000002 * I * I * I; break;
		case EPlanet::Venus:   Out.Magnitude = -4.40 + Distance + 0.0009 * I + 0.000239 * I * I - 0.00000065 * I * I * I; break;
		case EPlanet::Mars:    Out.Magnitude = -1.52 + Distance + 0.016 * I; break;
		case EPlanet::Jupiter: Out.Magnitude = -9.40 + Distance + 0.005 * I; break;
		case EPlanet::Saturn:
		{
			// Gli anelli (Meeus 45): quanto sono aperti verso la Terra.
			const double T = (JdTt - J2000) / 36525.0;
			const double RingInclination = (28.075216 - 0.012998 * T + 0.000004 * T * T) * DegToRad;
			const double RingNode = (169.508470 + 1.394681 * T + 0.000412 * T * T) * DegToRad;
			const double Lambda = std::atan2(Geo[1], Geo[0]);
			const double Beta = std::asin(Geo[2] / Delta);
			const double SinB = std::sin(RingInclination) * std::cos(Beta) * std::sin(Lambda - RingNode)
			                  - std::cos(RingInclination) * std::sin(Beta);
			Out.RingTiltDeg = std::asin(std::max(-1.0, std::min(1.0, SinB))) * RadToDeg;
			Out.Magnitude = -8.88 + Distance - 2.60 * std::fabs(SinB) + 1.25 * SinB * SinB;
			break;
		}
		}
		return Out;
	}

	/** Il colore di un pianeta (lineare, luminanza 1): Marte rosso, Saturno giallino. */
	inline void PlanetColour(EPlanet Planet, float Out[3])
	{
		double R = 1.0, G = 0.98, B = 0.92;     // Venere
		switch (Planet)
		{
		case EPlanet::Mercury: G = 0.92; B = 0.82; break;
		case EPlanet::Mars:    G = 0.62; B = 0.42; break;
		case EPlanet::Jupiter: G = 0.95; B = 0.86; break;
		case EPlanet::Saturn:  G = 0.92; B = 0.74; break;
		default: break;
		}
		const double Luminance = 0.2126 * R + 0.7152 * G + 0.0722 * B;
		Out[0] = static_cast<float>(R / Luminance);
		Out[1] = static_cast<float>(G / Luminance);
		Out[2] = static_cast<float>(B / Luminance);
	}

	// =========================================================================
	//  Le istanze per Unreal
	// =========================================================================

	/** Un punto del cielo pronto per il componente: nel cielo specchiato J2000, a Radius metri. */
	struct FStarInstance
	{
		float Position[3] = { 0.0f, 0.0f, 0.0f };
		float Colour[3] = { 1.0f, 1.0f, 1.0f };
		float Magnitude = 6.0f;
		/** Un numero 0..1 per stella: la fase della scintillazione. */
		float Seed = 0.0f;
	};

	/** I dati per istanza del materiale (M_GeoStars): il colore, la magnitudine, il seme. */
	inline constexpr int32_t StarDataFloats = 5;

	inline void StarData(const FStarInstance& Star, float Out[StarDataFloats])
	{
		Out[0] = Star.Colour[0];
		Out[1] = Star.Colour[1];
		Out[2] = Star.Colour[2];
		Out[3] = Star.Magnitude;
		Out[4] = Star.Seed;
	}

	inline FStarInstance MakeStarInstance(double RaDeg, double DecDeg, float Magnitude, const float Colour[3], double RadiusM, uint32_t Salt)
	{
		FStarInstance Out;
		double Unit[3];
		MirroredUnit(RaDeg, DecDeg, Unit);
		for (int32_t Axis = 0; Axis < 3; ++Axis)
		{
			Out.Position[Axis] = static_cast<float>(Unit[Axis] * RadiusM);
			Out.Colour[Axis] = Colour[Axis];
		}
		Out.Magnitude = Magnitude;
		uint32_t Hash = Salt * 0x9E3779B9u + 0x632BE5ABu;
		Hash = (Hash ^ (Hash >> 15)) * 0x846CA68Bu;
		Hash ^= Hash >> 13;
		Out.Seed = static_cast<float>(Hash & 0xFFFFu) / 65535.0f;
		return Out;
	}

	/** Le stelle del catalogo come istanze, fino alla magnitudine MaxMagnitude. */
	inline void BuildStarInstances(const std::vector<FCatalogStar>& Catalog, double RadiusM, double MaxMagnitude, std::vector<FStarInstance>& Out)
	{
		Out.clear();
		Out.reserve(Catalog.size());
		uint32_t Salt = 1;
		for (const FCatalogStar& Star : Catalog)
		{
			if (Star.Magnitude > MaxMagnitude) { continue; }
			float Colour[3];
			StarColour(Star.ColourIndex, Colour);
			Out.push_back(MakeStarInstance(Star.RaDeg, Star.DecDeg, Star.Magnitude, Colour, RadiusM, Salt++));
		}
	}

	/** Un pianeta come istanza (si aggiorna ogni tanto: si muove fra le stelle). */
	inline FStarInstance PlanetInstance(EPlanet Planet, const FPlanet& Seen, double RadiusM)
	{
		float Colour[3];
		PlanetColour(Planet, Colour);
		FStarInstance Out = MakeStarInstance(Seen.RaDeg, Seen.DecDeg, static_cast<float>(Seen.Magnitude), Colour, RadiusM,
			0xA5A5u + static_cast<uint32_t>(Planet));
		// I pianeti non scintillano: sono dischetti, non punti.
		Out.Seed = -1.0f;
		return Out;
	}
}
