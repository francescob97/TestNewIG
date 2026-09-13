// STRATO: C++ PURO (nessun include di Unreal Engine).
#include "Geo/Ellipsoid.h"

namespace GeoWorld::Core
{
	namespace
	{
		// Soglia sotto la quale consideriamo il punto "sull'asse polare".
		// Sotto questo raggio dall'asse Z la longitudine e' matematicamente
		// indefinita e le formule che dividono per p degenerano.
		constexpr double PolarAxisEpsilonM = 1e-9;
	}

	FEcef GeodeticSurfaceNormal(const FGeodetic& Geodetic)
	{
		const double CosLat = std::cos(Geodetic.LatRad);
		const double SinLat = std::sin(Geodetic.LatRad);
		const double CosLon = std::cos(Geodetic.LonRad);
		const double SinLon = std::sin(Geodetic.LonRad);

		// Gia' unitario per costruzione: non serve normalizzare.
		return FEcef{ CosLat * CosLon, CosLat * SinLon, SinLat };
	}

	FEcef GeodeticToEcef(const FGeodetic& G, const FEllipsoid& E)
	{
		const double SinLat = std::sin(G.LatRad);
		const double CosLat = std::cos(G.LatRad);
		const double SinLon = std::sin(G.LonRad);
		const double CosLon = std::cos(G.LonRad);

		// Raggio di curvatura in prima verticale.
		const double N = E.A / std::sqrt(1.0 - E.E2 * SinLat * SinLat);

		return FEcef{
			(N + G.HeightM) * CosLat * CosLon,
			(N + G.HeightM) * CosLat * SinLon,
			(N * (1.0 - E.E2) + G.HeightM) * SinLat };
	}

	namespace
	{
		// Quota ellissoidica data la latitudine gia' nota.
		//
		//   h = p*cos(lat) + Z*sin(lat) - a*sqrt(1 - e^2 sin^2(lat))
		//
		// PERCHE' QUESTA FORMA e non la piu' nota h = p/cos(lat) - N:
		// quest'ultima divide per cos(lat), che va a zero ai poli e fa esplodere
		// l'errore alle alte latitudini. La forma qui sopra non ha divisioni ed
		// e' stabile su tutto l'ellissoide, poli compresi.
		//
		// Verifica algebrica (utile per capire che non e' magia):
		//   p*cos + Z*sin = N + h - N*e^2*sin^2   e   a*sqrt(1-e^2 sin^2) = N - N*e^2*sin^2
		//   sottraendo resta esattamente h.
		inline double HeightFromLatitude(double P, double Z, double LatRad, const FEllipsoid& E)
		{
			const double SinLat = std::sin(LatRad);
			const double CosLat = std::cos(LatRad);
			return P * CosLat + Z * SinLat - E.A * std::sqrt(1.0 - E.E2 * SinLat * SinLat);
		}
	}

	FGeodetic EcefToGeodetic(const FEcef& Pos, const FEllipsoid& E)
	{
		// Distanza dall'asse polare.
		const double P = std::sqrt(Pos.X * Pos.X + Pos.Y * Pos.Y);

		// --- CASO LIMITE: punto sull'asse polare -----------------------------
		// La longitudine e' indefinita (per convenzione 0), la latitudine e'
		// esattamente +/-90 gradi e la quota si misura dal semiasse minore.
		if (P < PolarAxisEpsilonM)
		{
			const double Lat = (Pos.Z >= 0.0) ? HalfPi : -HalfPi;
			return FGeodetic::FromRadians(Lat, 0.0, std::abs(Pos.Z) - E.B);
		}

		// --- BOWRING 1976 ----------------------------------------------------
		// L'idea: si parte dalla latitudine PARAMETRICA (o "ridotta") theta
		// approssimata, che sull'ellissoide ha una forma chiusa semplice, e la si
		// usa dentro una relazione esatta che restituisce la latitudine geodetica
		// con un errore del terzo ordine nello schiacciamento: siccome f e' ~1/298,
		// l'errore residuo e' nell'ordine del decimo di millimetro.
		const double Theta    = std::atan2(Pos.Z * E.A, P * E.B);
		const double SinTheta = std::sin(Theta);
		const double CosTheta = std::cos(Theta);

		double Lat = std::atan2(
			Pos.Z + E.EP2 * E.B * SinTheta * SinTheta * SinTheta,
			P     - E.E2  * E.A * CosTheta * CosTheta * CosTheta);

		// --- UN PASSO DI RAFFINAZIONE A PUNTO FISSO --------------------------
		// Relazione esatta:  tan(lat) = Z / ( p * (1 - e^2 * N/(N+h)) ).
		// Inserendo la lat di Bowring (gia' buona a ~1e-10 rad) si ottiene una
		// latitudine accurata fino all'epsilon di macchina. Costa un sin/sqrt.
		{
			const double SinLat = std::sin(Lat);
			const double N      = E.A / std::sqrt(1.0 - E.E2 * SinLat * SinLat);
			const double H      = HeightFromLatitude(P, Pos.Z, Lat, E);

			// Guardia numerica: N + h si annulla solo per punti patologici vicini
			// al geocentro, dove le coordinate geodetiche non hanno comunque senso.
			const double Denom = N + H;
			if (std::abs(Denom) > 1e-12)
			{
				Lat = std::atan2(Pos.Z, P * (1.0 - E.E2 * N / Denom));
			}
		}

		const double Lon    = std::atan2(Pos.Y, Pos.X);
		const double Height = HeightFromLatitude(P, Pos.Z, Lat, E);

		return FGeodetic::FromRadians(Lat, Lon, Height);
	}

	FGeodetic EcefToGeodeticIterative(const FEcef& Pos, const FEllipsoid& E,
	                                  int MaxIterations, double ToleranceRad)
	{
		const double P = std::sqrt(Pos.X * Pos.X + Pos.Y * Pos.Y);

		if (P < PolarAxisEpsilonM)
		{
			const double PoleLat = (Pos.Z >= 0.0) ? HalfPi : -HalfPi;
			return FGeodetic::FromRadians(PoleLat, 0.0, std::abs(Pos.Z) - E.B);
		}

		// Inizializzazione con la latitudine geocentrica (approssimazione sferica):
		// volutamente "ingenua", per non ereditare nulla da Bowring. Se le due
		// implementazioni convergono allo stesso valore partendo da punti diversi,
		// il risultato e' credibile.
		double Lat = std::atan2(Pos.Z, P * (1.0 - E.E2));

		for (int Iter = 0; Iter < MaxIterations; ++Iter)
		{
			const double SinLat = std::sin(Lat);
			const double N      = E.A / std::sqrt(1.0 - E.E2 * SinLat * SinLat);
			const double H      = HeightFromLatitude(P, Pos.Z, Lat, E);

			const double Denom = N + H;
			if (std::abs(Denom) < 1e-12)
			{
				break;
			}

			const double NextLat = std::atan2(Pos.Z, P * (1.0 - E.E2 * N / Denom));
			const double Delta   = std::abs(NextLat - Lat);
			Lat = NextLat;

			if (Delta < ToleranceRad)
			{
				break;
			}
		}

		return FGeodetic::FromRadians(Lat, std::atan2(Pos.Y, Pos.X),
		                              HeightFromLatitude(P, Pos.Z, Lat, E));
	}

	FMat3 MakeEnuBasis(double LatRad, double LonRad)
	{
		const double SinLat = std::sin(LatRad), CosLat = std::cos(LatRad);
		const double SinLon = std::sin(LonRad), CosLon = std::cos(LonRad);

		// Up e' la normale geodetica; East e' tangente al parallelo (e non ha
		// componente Z, essendo orizzontale per definizione); North completa.
		const FEcef East { -SinLon,           CosLon,          0.0    };
		const FEcef North{ -SinLat * CosLon, -SinLat * SinLon, CosLat };
		const FEcef Up   {  CosLat * CosLon,  CosLat * SinLon, SinLat };

		return FMat3::FromRows(East, North, Up);
	}

	FMat3 MakeNeuBasis(double LatRad, double LonRad)
	{
		const double SinLat = std::sin(LatRad), CosLat = std::cos(LatRad);
		const double SinLon = std::sin(LonRad), CosLon = std::cos(LonRad);

		const FEcef North{ -SinLat * CosLon, -SinLat * SinLon, CosLat };
		const FEcef East { -SinLon,           CosLon,          0.0    };
		const FEcef Up   {  CosLat * CosLon,  CosLat * SinLon, SinLat };

		// Ordine North, East, Up: scambiando le prime due righe rispetto a ENU
		// il determinante passa da +1 a -1. E' la riflessione che converte il
		// mondo destrorso della geodesia in quello sinistrorso di Unreal.
		return FMat3::FromRows(North, East, Up);
	}

} // namespace GeoWorld::Core
