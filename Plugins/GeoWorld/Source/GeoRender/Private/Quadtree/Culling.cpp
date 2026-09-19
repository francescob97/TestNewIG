#include "Quadtree/Culling.h"

#include <algorithm>
#include <cmath>

namespace GeoWorld::Quadtree
{
	using namespace GeoWorld::Core;

	FTileBoundingVolume MakeTileBoundingVolume(
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

	FFrustumPlanes MakeFrustumPlanes(const FViewParameters& View)
	{
		FFrustumPlanes Planes;

		const double HalfVertical = View.VerticalFovRad * 0.5;
		const double HalfHorizontal = std::atan(std::tan(HalfVertical) * View.AspectRatio);

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

	bool IsSphereInFrustum(const FFrustumPlanes& Planes, const FEcef& Centre, double Radius)
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

	FEcef ToScaledSpace(const FEcef& Point, const FEllipsoid& Ellipsoid)
	{
		return FEcef{ Point.X / Ellipsoid.A, Point.Y / Ellipsoid.A, Point.Z / Ellipsoid.B };
	}

	double ComputeCameraHorizonSquared(const FEcef& ScaledCamera)
	{
		// |cv|^2 - 1: quadrato della distanza dalla camera al cerchio d'orizzonte
		// sulla sfera unitaria. Se la camera e' dentro la sfera esce negativo.
		return ScaledCamera.LengthSquared() - 1.0;
	}

	bool IsPointBelowHorizon(const FEcef& ScaledCamera, double CameraHorizonSquared,
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

	bool IsSphereBelowHorizon(const FEcef& Camera, const FEcef& Centre, double Radius,
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

	bool IsTileBelowHorizon(const FTileBoundingVolume& Volume, const FEcef& Camera,
	                        const FEllipsoid& Ellipsoid)
	{
		return IsSphereBelowHorizon(Camera, Volume.Centre, Volume.Radius, Ellipsoid);
	}
}
