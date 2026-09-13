// =============================================================================
//  EnuFrame.h -- Frame locale East-North-Up ancorato a un punto geodetico.
//  STRATO: C++ PURO (nessun include di Unreal Engine).
//
//  A cosa serve, concretamente: e' il ponte fra "coordinate del pianeta" e
//  "coordinate di un pezzo di terreno". In Fase 5 ogni tile genera i propri
//  vertici in un FEnuFrame centrato sul tile stesso, cosi' i valori restano
//  di pochi chilometri e stanno in un float con precisione millimetrica.
//  E' questa scelta che rende il rebasing quasi gratuito: un cambio d'origine
//  non tocca un solo vertice, cambia solo la trasformazione del componente.
// =============================================================================
#pragma once

#include "Geo/Ellipsoid.h"

namespace GeoWorld::Core
{
	class FEnuFrame
	{
	public:
		FEnuFrame() = default;

		explicit FEnuFrame(const FGeodetic& Origin, const FEllipsoid& InEllipsoid = WGS84)
			: Ellipsoid(InEllipsoid)
			, OriginGeodetic(Origin)
			, OriginEcef(GeodeticToEcef(Origin, InEllipsoid))
			, EcefToEnuBasis(MakeEnuBasis(Origin.LatRad, Origin.LonRad))
			// Ortonormale => l'inversa E' la trasposta. Nessuna inversione
			// numerica, nessuna perdita di precisione, round-trip esatta.
			, EnuToEcefBasis(EcefToEnuBasis.Transposed())
		{}

		const FGeodetic& GetOriginGeodetic() const { return OriginGeodetic; }
		const FEcef&     GetOriginEcef()     const { return OriginEcef; }
		const FMat3&     GetEcefToEnuBasis() const { return EcefToEnuBasis; }

		FEnu EcefToEnu(const FEcef& Position) const
		{
			// Prima si trasla (portando l'origine del frame nell'origine), poi si
			// ruota. Invertire l'ordine darebbe un risultato diverso e sbagliato.
			const FEcef Local = EcefToEnuBasis.Transform(Position - OriginEcef);
			return FEnu{ Local.X, Local.Y, Local.Z };
		}

		FEcef EnuToEcef(const FEnu& Local) const
		{
			return EnuToEcefBasis.Transform(FEcef{ Local.E, Local.N, Local.U }) + OriginEcef;
		}

		FEnu GeodeticToEnu(const FGeodetic& Geodetic) const
		{
			return EcefToEnu(GeodeticToEcef(Geodetic, Ellipsoid));
		}

		FGeodetic EnuToGeodetic(const FEnu& Local) const
		{
			return EcefToGeodetic(EnuToEcef(Local), Ellipsoid);
		}

	private:
		FEllipsoid Ellipsoid      = WGS84;
		FGeodetic  OriginGeodetic;
		FEcef      OriginEcef;
		FMat3      EcefToEnuBasis;
		FMat3      EnuToEcefBasis;
	};

} // namespace GeoWorld::Core
