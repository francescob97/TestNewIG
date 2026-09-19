// =============================================================================
//  Ellipsoid.h -- Ellissoide di riferimento e conversioni geodetiche <-> ECEF.
//  STRATO: C++ PURO (nessun include di Unreal Engine).
// =============================================================================
#pragma once

#include "Geo/GeoTypes.h"

namespace GeoWorld::Core
{
	// -------------------------------------------------------------------------
	//  FEllipsoid -- ellissoide di rotazione (sfera schiacciata ai poli).
	//
	//  Definito da due soli numeri indipendenti: il semiasse maggiore A e lo
	//  schiacciamento F. Tutto il resto e' derivato e precalcolato nel
	//  costruttore, perche' compare dentro loop caldi e non vogliamo ricalcolare
	//  eccentricita' milioni di volte.
	// -------------------------------------------------------------------------
	struct FEllipsoid
	{
		double A   = 0.0;   // semiasse maggiore (raggio equatoriale), metri
		double F   = 0.0;   // schiacciamento (a-b)/a
		double B   = 0.0;   // semiasse minore (raggio polare), metri
		double E2  = 0.0;   // prima eccentricita' al quadrato, e^2 = f(2-f)
		double EP2 = 0.0;   // seconda eccentricita' al quadrato, e'^2 = e^2/(1-e^2)

		// InInvF e' l'inverso dello schiacciamento (1/f), che e' il modo in cui
		// gli ellissoidi vengono sempre pubblicati (WGS84: 298.257223563).
		constexpr FEllipsoid(double InA, double InInvF)
			: A(InA)
			, F(1.0 / InInvF)
			, B(InA * (1.0 - 1.0 / InInvF))
			, E2((1.0 / InInvF) * (2.0 - (1.0 / InInvF)))
			, EP2(((1.0 / InInvF) * (2.0 - (1.0 / InInvF))) / (1.0 - ((1.0 / InInvF) * (2.0 - (1.0 / InInvF)))))
		{}

		// Raggio massimo: serve per il bounding volume globale e, in Fase 4, per
		// il horizon culling conservativo.
		constexpr double MaximumRadius() const { return A; }
		constexpr double MinimumRadius() const { return B; }
	};

	// WGS84: i due numeri che definiscono il datum usato da GPS, EPSG:4326 e
	// da tutto il nostro motore. Sono valori DEFINITI (non misurati), quindi
	// esatti per definizione.
	inline constexpr FEllipsoid WGS84{ 6378137.0, 298.257223563 };

	// -------------------------------------------------------------------------
	//  Normale geodetica alla superficie in un punto.
	//
	//  ATTENZIONE: e' la normale GEODETICA (perpendicolare al piano tangente
	//  all'ellissoide), NON quella geocentrica (direzione dal centro della Terra).
	//  Alle medie latitudini differiscono fino a ~11 primi d'arco: confonderle
	//  significa sbagliare la verticale locale, e in Fase 5 significa tile
	//  inclinati male e crepe fra livelli.
	//  Nota: dipende solo da lat/lon, non dalla quota.
	// -------------------------------------------------------------------------
	FEcef GeodeticSurfaceNormal(const FGeodetic& Geodetic);

	// -------------------------------------------------------------------------
	//  Geodetiche -> ECEF. Forma chiusa esatta, nessuna iterazione.
	//
	//      N(lat) = a / sqrt(1 - e^2 sin^2(lat))        (raggio di curvatura in
	//                                                    prima verticale)
	//      X = (N + h) cos(lat) cos(lon)
	//      Y = (N + h) cos(lat) sin(lon)
	//      Z = (N (1 - e^2) + h) sin(lat)
	//
	//  Il termine (1-e^2) sulla Z e' l'intero schiacciamento terrestre: e' il
	//  motivo per cui la verticale locale non punta al centro della Terra.
	// -------------------------------------------------------------------------
	FEcef GeodeticToEcef(const FGeodetic& Geodetic, const FEllipsoid& Ellipsoid = WGS84);

	// -------------------------------------------------------------------------
	//  ECEF -> Geodetiche. Implementazione di PRODUZIONE: Bowring (1976) con un
	//  passo di raffinazione a punto fisso.
	//
	//  PERCHE' BOWRING e non un metodo iterativo:
	//   - COSTO DETERMINISTICO. Questa funzione gira ogni frame (posizione
	//     geodetica della camera per HUD e rebasing) e, dalla Fase 4, su migliaia
	//     di tile. Un metodo iterativo ha un numero di iterazioni variabile:
	//     varianza nel frame time e, peggio, risultati non bit-identici fra
	//     thread diversi, che e' veleno quando il worker thread e il game thread
	//     devono concordare.
	//   - ACCURATEZZA SOVRABBONDANTE. Bowring sta sotto il decimo di millimetro
	//     per quote fra -10 km e +30 km. Il nostro dominio e' [-500 m, +9000 m]
	//     su un DTM con passo 10 m e accuratezza verticale metrica.
	//   - E' UNA FUNZIONE SOLA. Se un giorno servissero quote orbitali si
	//     sostituisce con Vermeille in un punto solo, e i test restano identici.
	//
	//  La raffinazione finale costa un sin/cos/sqrt in piu' e porta l'errore al
	//  livello dell'epsilon di macchina: la teniamo perche' il costo e' nulla
	//  rispetto al resto del frame.
	// -------------------------------------------------------------------------
	FGeodetic EcefToGeodetic(const FEcef& Position, const FEllipsoid& Ellipsoid = WGS84);

	// -------------------------------------------------------------------------
	//  ECEF -> Geodetiche, versione ITERATIVA a punto fisso (Heiskanen-Moritz).
	//
	//  NON usarla in produzione. Esiste per un motivo solo: essere un'implementazione
	//  indipendente contro cui confrontare Bowring nei test. Scrivere due volte
	//  la stessa cosa con algoritmi diversi e' il modo piu' economico di scoprire
	//  che una delle due ha un bug.
	// -------------------------------------------------------------------------
	FGeodetic EcefToGeodeticIterative(const FEcef& Position,
	                                  const FEllipsoid& Ellipsoid = WGS84,
	                                  int MaxIterations = 64,
	                                  double ToleranceRad = 1e-15);

	// -------------------------------------------------------------------------
	//  Basi locali, espresse in ECEF.
	//
	//  MakeEnuBasis  -> righe (East, North, Up).  DESTRORSA,  det = +1. Geodesia.
	//  MakeNeuBasis  -> righe (North, East, Up).  SINISTRORSA, det = -1. Unreal.
	//
	//  La differenza fra le due e' uno scambio di righe, cioe' una RIFLESSIONE.
	//  E' esattamente cio' che serve per passare dal mondo destrorso della
	//  geodesia a quello sinistrorso di Unreal senza specchiare la geometria.
	//  Entrambe restano ortonormali, quindi l'inversa resta la trasposta.
	// -------------------------------------------------------------------------
	FMat3 MakeEnuBasis(double LatRad, double LonRad);
	FMat3 MakeNeuBasis(double LatRad, double LonRad);


	// ======================================================================
	//  IMPLEMENTAZIONE
	//
	//  Lo strato puro e' HEADER-ONLY, e non per stile: in Unreal ogni modulo
	//  e' una DLL, e un simbolo definito in un .cpp non e' visibile agli altri
	//  moduli se non viene esportato con la macro API del modulo. Esportarlo
	//  significherebbe pero' mettere una macro del motore dentro lo strato che
	//  per definizione non deve sapere di stare dentro Unreal: proprio la
	//  perdita che la separazione in due strati esiste per evitare.
	//  Header-only risolve alla radice, e su funzioni matematiche di poche
	//  righe non costa niente.
	// ======================================================================

	namespace Detail
	{
		// Soglia sotto la quale consideriamo il punto "sull'asse polare".
		// Sotto questo raggio dall'asse Z la longitudine e' matematicamente
		// indefinita e le formule che dividono per p degenerano.
		constexpr double PolarAxisEpsilonM = 1e-9;
	}

	inline FEcef GeodeticSurfaceNormal(const FGeodetic& Geodetic)
	{
		const double CosLat = std::cos(Geodetic.LatRad);
		const double SinLat = std::sin(Geodetic.LatRad);
		const double CosLon = std::cos(Geodetic.LonRad);
		const double SinLon = std::sin(Geodetic.LonRad);

		// Gia' unitario per costruzione: non serve normalizzare.
		return FEcef{ CosLat * CosLon, CosLat * SinLon, SinLat };
	}

	inline FEcef GeodeticToEcef(const FGeodetic& G, const FEllipsoid& E)
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

	namespace Detail
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

	inline FGeodetic EcefToGeodetic(const FEcef& Pos, const FEllipsoid& E)
	{
		// Distanza dall'asse polare.
		const double P = std::sqrt(Pos.X * Pos.X + Pos.Y * Pos.Y);

		// --- CASO LIMITE: punto sull'asse polare -----------------------------
		// La longitudine e' indefinita (per convenzione 0), la latitudine e'
		// esattamente +/-90 gradi e la quota si misura dal semiasse minore.
		if (P < Detail::PolarAxisEpsilonM)
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
			const double H      = Detail::HeightFromLatitude(P, Pos.Z, Lat, E);

			// Guardia numerica: N + h si annulla solo per punti patologici vicini
			// al geocentro, dove le coordinate geodetiche non hanno comunque senso.
			const double Denom = N + H;
			if (std::abs(Denom) > 1e-12)
			{
				Lat = std::atan2(Pos.Z, P * (1.0 - E.E2 * N / Denom));
			}
		}

		const double Lon    = std::atan2(Pos.Y, Pos.X);
		const double Height = Detail::HeightFromLatitude(P, Pos.Z, Lat, E);

		return FGeodetic::FromRadians(Lat, Lon, Height);
	}

	inline FGeodetic EcefToGeodeticIterative(const FEcef& Pos, const FEllipsoid& E,
	                                  int MaxIterations, double ToleranceRad)
	{
		const double P = std::sqrt(Pos.X * Pos.X + Pos.Y * Pos.Y);

		if (P < Detail::PolarAxisEpsilonM)
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
			const double H      = Detail::HeightFromLatitude(P, Pos.Z, Lat, E);

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
		                              Detail::HeightFromLatitude(P, Pos.Z, Lat, E));
	}

	inline FMat3 MakeEnuBasis(double LatRad, double LonRad)
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

	inline FMat3 MakeNeuBasis(double LatRad, double LonRad)
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
