// =============================================================================
//  Culling.h -- Scarto per frustum e per orizzonte. STRATO: C++ PURO.
// =============================================================================
#pragma once

#include "Quadtree/QuadtreeTypes.h"
#include <algorithm>
#include <cmath>

namespace GeoWorld::Quadtree
{
	/**
	 * Costruisce il volume di contenimento di una tile dai suoi estremi
	 * geografici e dall'intervallo di quote, che l'indice del dataset fornisce
	 * SENZA leggere la tile da disco. E' questo che rende possibile scartare un
	 * nodo prima ancora di chiederlo.
	 */
	FTileBoundingVolume MakeTileBoundingVolume(
		double West, double South, double East, double North,
		double MinHeight, double MaxHeight,
		const Core::FEllipsoid& Ellipsoid = Core::WGS84);

	/** I cinque piani del frustum (quattro laterali + near), normali verso l'interno. */
	struct FFrustumPlanes
	{
		FEcef Normals[5];
		double Offsets[5] = { 0, 0, 0, 0, 0 };   // piano: dot(N, P) >= Offset
	};

	FFrustumPlanes MakeFrustumPlanes(const FViewParameters& View);

	/** false se la sfera e' interamente fuori da almeno un piano. */
	bool IsSphereInFrustum(const FFrustumPlanes& Planes, const FEcef& Centre, double Radius);

	/**
	 * Test di occlusione contro l'ellissoide: il punto sta oltre l'orizzonte?
	 *
	 * Si lavora nello "spazio scalato", cioe' dividendo le coordinate ECEF per i
	 * semiassi: li' l'ellissoide diventa una sfera di raggio 1 e il test
	 * dell'orizzonte torna a essere quello elementare della sfera. E' il
	 * classico test di Cesium.
	 */
	bool IsPointBelowHorizon(const FEcef& ScaledCamera, double CameraHorizonSquared,
	                         const FEcef& Point, const Core::FEllipsoid& Ellipsoid);

	/** Porta un punto ECEF nello spazio scalato dove l'ellissoide e' una sfera unitaria. */
	FEcef ToScaledSpace(const FEcef& Point, const Core::FEllipsoid& Ellipsoid);

	/**
	 * Quadrato della distanza dalla camera all'orizzonte, in spazio scalato.
	 * Negativo se la camera e' DENTRO l'ellissoide: in quel caso non si scarta
	 * niente, perche' l'orizzonte non e' definito.
	 */
	double ComputeCameraHorizonSquared(const FEcef& ScaledCamera);

	/**
	 * L'intera SFERA di contenimento sta oltre l'orizzonte?
	 *
	 * =========================================================================
	 *  PERCHE' NON SI TESTANO I PUNTI CAMPIONE
	 * =========================================================================
	 *  La prima versione testava i 18 campioni del volume e scartava se erano
	 *  occlusi tutti. Sembra conservativo e non lo e': i campioni sono punti, e
	 *  una tile grande puo' contenere zone visibili che non cadono su nessuno di
	 *  essi. Il caso reale, trovato dai test: una tile di livello 0 copre mezzo
	 *  pianeta e i suoi campioni stanno a longitudine 0, 90 e 180 gradi. Con la
	 *  camera sopra Roma erano tutti oltre l'orizzonte, e la tile che conteneva
	 *  la camera veniva scartata.
	 *
	 *  Qui si ragiona invece sulla sfera intera, in angoli visti dal centro del
	 *  pianeta:
	 *
	 *      theta  angolo fra la direzione della camera e quella del centro sfera
	 *      beta   semiangolo sotteso dalla sfera
	 *      alphaC angolo di visibilita' della camera:  acos(R / distanza_camera)
	 *      alphaT angolo di visibilita' del punto piu' lontano della sfera
	 *
	 *  La sfera e' interamente occlusa se  theta - beta > alphaC + alphaT.
	 *
	 *  Il pianeta e' approssimato con la sfera INSCRITTA (raggio polare). E'
	 *  deliberato: una sfera piu' piccola occlude meno, quindi al piu' si tiene
	 *  qualche tile che si sarebbe potuta scartare. L'errore va tenuto in questo
	 *  verso — tenere una tile di troppo costa lavoro, scartarne una visibile
	 *  apre un buco nel terreno.
	 */
	bool IsSphereBelowHorizon(const FEcef& Camera, const FEcef& Centre, double Radius,
	                          const Core::FEllipsoid& Ellipsoid);

	/** Comodita': applica IsSphereBelowHorizon al volume di una tile. */
	bool IsTileBelowHorizon(const FTileBoundingVolume& Volume, const FEcef& Camera,
	                        const Core::FEllipsoid& Ellipsoid);

	/**
	 * La tile, come RETTANGOLO geografico, sta tutta oltre l'orizzonte?
	 *
	 * =========================================================================
	 *  PERCHE' UN SECONDO TEST, E PERCHE' E' QUELLO CHE USA IL SELETTORE
	 * =========================================================================
	 *  Il test sulla sfera qui sopra rinuncia ("non so, tengo") appena la sfera
	 *  di contenimento scende sotto la superficie del pianeta. Ma una sfera che
	 *  contiene un pezzo di superficie curva scende SEMPRE sotto la superficie,
	 *  di una quantita' che cresce con la tile: in pratica rinuncia per tutte
	 *  le tile dal livello 0 fin verso il 10. Finche' il frustum scartava
	 *  l'altra faccia del pianeta non se ne accorgeva nessuno. Con la selezione
	 *  indipendente dalla vista, su un dataset mondiale, si caricavano tile
	 *  sull'America guardando Roma: trovato dal test "l'orizzonte continua a
	 *  scartare l'altra faccia del pianeta".
	 *
	 *  Qui si ragiona sul rettangolo vero, nello spazio scalato dove il
	 *  pianeta e' esattamente la sfera unitaria:
	 *
	 *    gamma   angolo minimo, visto dal centro, fra la camera e un punto
	 *            qualunque del rettangolo
	 *    alphaC  acos(1 / distanza scalata della camera)
	 *    alphaT  acos(1 / distanza scalata massima di un punto della tile)
	 *
	 *    la tile e' occlusa se  gamma > alphaC + alphaT
	 *
	 *  gamma si calcola in forma chiusa: in spazio scalato la latitudine di
	 *  un punto in superficie e' quella PARAMETRICA, monotona rispetto alla
	 *  geodetica, quindi il rettangolo resta un rettangolo, e la distanza di
	 *  un punto da un rettangolo lat/lon su una sfera ha una formula chiusa.
	 */
	bool IsTileRectBeyondHorizon(double WestDeg, double SouthDeg, double EastDeg, double NorthDeg,
	                             double MaxHeight, const FEcef& Camera,
	                             const Core::FEllipsoid& Ellipsoid);

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


	inline FTileBoundingVolume MakeTileBoundingVolume(
		double West, double South, double East, double North,
		double MinHeight, double MaxHeight, const FEllipsoid& Ellipsoid)
	{
		FTileBoundingVolume Volume;

		// 3x3 punti sulla superficie (spigoli, meta' dei lati, centro) alle due
		// quote estreme. Il punto centrale e' quello che conta sulle tile grandi:
		// li' la superficie si allontana di piu' dal piano degli spigoli.
		const double Lons[3] = { West, (West + East) * 0.5, East };
		const double Lats[3] = { North, (South + North) * 0.5, South };
		const double Heights[2] = { MinHeight, MaxHeight };

		for (double Height : Heights)
		{
			for (double Lat : Lats)
			{
				for (double Lon : Lons)
				{
					Volume.Samples[Volume.SampleCount++] =
						GeodeticToEcef(FGeodetic::FromDegrees(Lat, Lon, Height), Ellipsoid);
				}
			}
		}

		// Centro = centro dell'AABB dei campioni, raggio = distanza massima.
		// Non e' la sfera minima assoluta, ma e' vicina e si calcola in una
		// passata: con decine di migliaia di nodi per frame conta.
		FEcef Minimum = Volume.Samples[0];
		FEcef Maximum = Volume.Samples[0];
		for (int32_t Index = 1; Index < Volume.SampleCount; ++Index)
		{
			const FEcef& Sample = Volume.Samples[Index];
			Minimum.X = std::min(Minimum.X, Sample.X); Maximum.X = std::max(Maximum.X, Sample.X);
			Minimum.Y = std::min(Minimum.Y, Sample.Y); Maximum.Y = std::max(Maximum.Y, Sample.Y);
			Minimum.Z = std::min(Minimum.Z, Sample.Z); Maximum.Z = std::max(Maximum.Z, Sample.Z);
		}

		Volume.Centre = FEcef{ (Minimum.X + Maximum.X) * 0.5,
		                       (Minimum.Y + Maximum.Y) * 0.5,
		                       (Minimum.Z + Maximum.Z) * 0.5 };

		double RadiusSquared = 0.0;
		for (int32_t Index = 0; Index < Volume.SampleCount; ++Index)
		{
			RadiusSquared = std::max(RadiusSquared,
				(Volume.Samples[Index] - Volume.Centre).LengthSquared());
		}
		Volume.Radius = std::sqrt(RadiusSquared);

		return Volume;
	}

	inline FFrustumPlanes MakeFrustumPlanes(const FViewParameters& View)
	{
		FFrustumPlanes Planes;

		// Il margine si applica alla TANGENTE del semiangolo, non all'angolo:
		// e' la tangente a misurare quanto e' largo il frustum sul piano dello
		// schermo, quindi "il 20% piu' largo" significa questo. Moltiplicare
		// l'angolo darebbe un allargamento che cresce in modo strano verso i
		// 90 gradi.
		const double Margin = (View.FrustumMarginFactor > 1.0) ? View.FrustumMarginFactor : 1.0;

		// Limite a 85 gradi per semiangolo: oltre, la tangente esplode e i
		// piani degenerano. Nessun campo visivo reale ci arriva, ma un valore
		// di margine assurdo da console non deve produrre un frustum rotto.
		constexpr double MaxHalfAngle = 1.4835;     // 85 gradi
		const double HalfVertical =
			std::min(std::atan(std::tan(View.VerticalFovRad * 0.5) * Margin), MaxHalfAngle);
		const double HalfHorizontal =
			std::min(std::atan(std::tan(HalfVertical) * View.AspectRatio), MaxHalfAngle);

		const double SinH = std::sin(HalfHorizontal), CosH = std::cos(HalfHorizontal);
		const double SinV = std::sin(HalfVertical), CosV = std::cos(HalfVertical);

		// Normale del piano sinistro: F*sin(h) + R*cos(h).
		// Verifica: il bordo sinistro del frustum punta lungo F*cos(h) - R*sin(h),
		// e il prodotto scalare fra i due e' sin*cos - cos*sin = 0, quindi la
		// normale e' davvero perpendicolare al bordo. Punta verso +R, cioe'
		// verso l'interno.
		const FEcef& F = View.Forward;
		const FEcef& R = View.Right;
		const FEcef& U = View.Up;

		Planes.Normals[0] = F * SinH + R * CosH;    // sinistro
		Planes.Normals[1] = F * SinH - R * CosH;    // destro
		Planes.Normals[2] = F * SinV + U * CosV;    // inferiore
		Planes.Normals[3] = F * SinV - U * CosV;    // superiore
		Planes.Normals[4] = F;                      // near

		// I primi quattro passano per la camera; il near e' spostato in avanti.
		for (int Index = 0; Index < 4; ++Index)
		{
			Planes.Offsets[Index] = Planes.Normals[Index].Dot(View.CameraEcef);
		}
		Planes.Offsets[4] = F.Dot(View.CameraEcef) + View.NearClipMetres;

		// NOTA: il piano FAR non c'e'. Su scala planetaria il terreno lontano si
		// scarta da solo per errore su schermo e per orizzonte, molto prima che
		// un far plane abbia senso; metterne uno arbitrario taglierebbe montagne
		// visibili all'orizzonte.
		return Planes;
	}

	inline bool IsSphereInFrustum(const FFrustumPlanes& Planes, const FEcef& Centre, double Radius)
	{
		for (int Index = 0; Index < 5; ++Index)
		{
			const double Distance = Planes.Normals[Index].Dot(Centre) - Planes.Offsets[Index];
			if (Distance < -Radius)
			{
				return false;   // interamente dietro questo piano
			}
		}
		return true;
	}

	inline FEcef ToScaledSpace(const FEcef& Point, const FEllipsoid& Ellipsoid)
	{
		return FEcef{ Point.X / Ellipsoid.A, Point.Y / Ellipsoid.A, Point.Z / Ellipsoid.B };
	}

	inline double ComputeCameraHorizonSquared(const FEcef& ScaledCamera)
	{
		// |cv|^2 - 1: quadrato della distanza dalla camera al cerchio d'orizzonte
		// sulla sfera unitaria. Se la camera e' dentro la sfera esce negativo.
		return ScaledCamera.LengthSquared() - 1.0;
	}

	inline bool IsPointBelowHorizon(const FEcef& ScaledCamera, double CameraHorizonSquared,
	                         const FEcef& Point, const FEllipsoid& Ellipsoid)
	{
		if (CameraHorizonSquared <= 0.0)
		{
			return false;   // camera dentro l'ellissoide: orizzonte indefinito
		}

		const FEcef ScaledPoint = ToScaledSpace(Point, Ellipsoid);
		const FEcef ToPoint = ScaledPoint - ScaledCamera;

		// Proiezione del vettore camera->punto sulla direzione camera->centro.
		const double AlongViewToCentre = -ToPoint.Dot(ScaledCamera);

		// Prima condizione: il punto sta oltre il piano dell'orizzonte.
		if (AlongViewToCentre <= CameraHorizonSquared)
		{
			return false;
		}

		// Seconda condizione: sta anche dentro il cono d'ombra del pianeta.
		// Senza, un punto molto alto oltre il piano dell'orizzonte verrebbe
		// scartato pur essendo visibile: e' il caso di una montagna lontana che
		// spunta oltre la curvatura.
		return AlongViewToCentre * AlongViewToCentre / ToPoint.LengthSquared()
		     > CameraHorizonSquared;
	}

	inline bool IsSphereBelowHorizon(const FEcef& Camera, const FEcef& Centre, double Radius,
	                          const FEllipsoid& Ellipsoid)
	{
		// Sfera inscritta nell'ellissoide: occlude meno del vero, quindi si
		// sbaglia sempre tenendo qualche tile in piu'.
		const double PlanetRadius = Ellipsoid.B;

		const double CameraDistance = Camera.Length();
		if (CameraDistance <= PlanetRadius)
		{
			return false;   // camera dentro il pianeta: orizzonte indefinito
		}

		const double CentreDistance = Centre.Length();
		const double NearestDistance = CentreDistance - Radius;
		if (NearestDistance <= PlanetRadius)
		{
			// La sfera tocca o attraversa la superficie: e' il caso delle tile
			// grandi, il cui volume ingloba parte del pianeta. Non si puo'
			// concludere niente, quindi non si scarta.
			return false;
		}

		const double CosTheta = std::max(-1.0, std::min(1.0,
			Camera.Dot(Centre) / (CameraDistance * CentreDistance)));
		const double Theta = std::acos(CosTheta);

		// Semiangolo sotteso dalla sfera vista dal centro del pianeta.
		const double Beta = std::asin(std::min(1.0, Radius / CentreDistance));

		// Quanto lontano, in angolo, arriva lo sguardo dalla camera e dal punto
		// piu' esterno della sfera.
		const double AlphaCamera = std::acos(PlanetRadius / CameraDistance);
		const double AlphaTile = std::acos(PlanetRadius / NearestDistance);

		return (Theta - Beta) > (AlphaCamera + AlphaTile);
	}

	inline bool IsTileBelowHorizon(const FTileBoundingVolume& Volume, const FEcef& Camera,
	                        const FEllipsoid& Ellipsoid)
	{
		return IsSphereBelowHorizon(Camera, Volume.Centre, Volume.Radius, Ellipsoid);
	}

	inline bool IsTileRectBeyondHorizon(double WestDeg, double SouthDeg, double EastDeg, double NorthDeg,
	                                    double MaxHeight, const FEcef& Camera,
	                                    const FEllipsoid& Ellipsoid)
	{
		// Tutto nello SPAZIO SCALATO (x/a, y/a, z/b), come IsPointBelowHorizon:
		// li' l'ellissoide e' la sfera unitaria ESATTA, e una trasformazione
		// affine conserva rette e tangenze, quindi il cono d'orizzonte calcolato
		// sulla sfera unitaria e' quello vero del pianeta. La prima versione
		// usava la sfera inscritta di raggio B: corretta ma cosi' prudente da
		// tenere tile a livello del mare 400 km oltre l'orizzonte.
		const FEcef Scaled = ToScaledSpace(Camera, Ellipsoid);
		const double CameraDistance = Scaled.Length();
		if (CameraDistance <= 1.0) { return false; }

		// La direzione scalata di un punto in superficie ha come latitudine la
		// latitudine PARAMETRICA: tan(beta) = (b/a) tan(geodetica). E' monotona,
		// quindi il rettangolo geografico resta un rettangolo.
		const double Ratio = Ellipsoid.B / Ellipsoid.A;
		auto Parametric = [Ratio](double LatDeg)
		{
			return std::atan(Ratio * std::tan(LatDeg * Core::DegToRad));
		};
		const double South = Parametric(std::max(-89.999999, SouthDeg));
		const double North = Parametric(std::min(89.999999, NorthDeg));

		const double CameraLat = std::atan2(Scaled.Z, std::sqrt(Scaled.X * Scaled.X + Scaled.Y * Scaled.Y));
		const double CameraLon = std::atan2(Scaled.Y, Scaled.X);

		// Differenza di longitudine ridotta a [0, pi].
		auto LonGap = [CameraLon](double LonDeg)
		{
			const double Delta = std::fmod(std::fabs(LonDeg * Core::DegToRad - CameraLon), 2.0 * Core::Pi);
			return Delta > Core::Pi ? 2.0 * Core::Pi - Delta : Delta;
		};

		const double West = WestDeg * Core::DegToRad;
		const double East = EastDeg * Core::DegToRad;
		double Wrapped = CameraLon;
		while (Wrapped < West) { Wrapped += 2.0 * Core::Pi; }
		const bool bLonInside = (Wrapped <= East);

		// gamma: angolo minimo fra la direzione della camera e il rettangolo.
		double Gamma = 0.0;
		if (bLonInside)
		{
			// Per ogni latitudine la distanza cresce con la differenza di
			// longitudine: il punto piu' vicino sta sul meridiano della camera.
			Gamma = std::fabs(CameraLat - std::max(South, std::min(North, CameraLat)));
		}
		else
		{
			// Il punto piu' vicino sta sul lato meridiano piu' vicino. Lungo
			// quel meridiano
			//   cos(d) = sin(lat) sin(lc) + cos(lat) cos(lc) cos(dlon)
			// e' una sinusoide in lat: il suo massimo (la distanza minima) sta
			// nel picco se cade nell'intervallo, altrimenti a un estremo.
			const double Gap = std::min(LonGap(WestDeg), LonGap(EastDeg));
			const double SinPart = std::sin(CameraLat);
			const double CosPart = std::cos(CameraLat) * std::cos(Gap);
			auto CosDistance = [SinPart, CosPart](double Lat)
			{
				return SinPart * std::sin(Lat) + CosPart * std::cos(Lat);
			};

			double Best = std::max(CosDistance(South), CosDistance(North));
			const double Peak = std::atan2(SinPart, CosPart);
			if (Peak >= South && Peak <= North) { Best = std::max(Best, CosDistance(Peak)); }
			Gamma = std::acos(std::max(-1.0, std::min(1.0, Best)));
		}

		// Una quota h sposta un punto di al piu' h/B nello spazio scalato
		// (si divide per semiassi >= B), e ne ruota la direzione di un angolo
		// dell'ordine di (h/A) * 0.0034 rad: 5e-6 rad per 10 km. Il margine
		// lo copre, dal lato che tiene le tile.
		constexpr double DirectionSlack = 1e-5;
		const double TileDistance = 1.0 + std::max(0.0, MaxHeight) / Ellipsoid.B;
		const double AlphaCamera = std::acos(1.0 / CameraDistance);
		const double AlphaTile = std::acos(1.0 / TileDistance);

		return Gamma - DirectionSlack > AlphaCamera + AlphaTile;
	}

}
