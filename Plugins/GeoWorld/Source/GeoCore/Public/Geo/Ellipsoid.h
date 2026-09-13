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

} // namespace GeoWorld::Core
