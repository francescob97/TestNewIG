// =============================================================================
//  Georeference.h -- La trasformazione pianeta <-> mondo di Unreal.
//  STRATO: C++ PURO (nessun include di Unreal Engine).
//
//  Definisce la mappa
//
//      P_unreal[cm] = M * (P_ecef[m] - O_ecef[m]) * 100
//
//  dove O e' l'origine corrente (un punto geodetico) e M e' la base locale con
//  righe (North, East, Up) espressa in ECEF.
//
//  ----------------------------------------------------------------------------
//  ASSI: North -> X, East -> Y, Up -> Z   (convenzione NEU, confermata)
//
//  Perche' non il banale East->X, North->Y, Up->Z: ENU e' DESTRORSO (E x N = U),
//  Unreal e' SINISTRORSO (X avanti, Y a DESTRA, Z su). Copiare le componenti 1:1
//  produce un mondo SPECCHIATO: i modelli hanno chiralita' sbagliata e le
//  rotazioni girano al contrario. Serve una riflessione, cioe' una matrice con
//  determinante -1; scambiare North ed East e' il modo piu' utile di ottenerla,
//  perche' in piu' fa coincidere lo YAW DI UNREAL CON L'AZIMUTH DELLA BUSSOLA:
//  yaw 0 = Nord (+X), yaw 90 = Est (+Y), orario visto dall'alto. Un attore con
//  rotazione identita' guarda a Nord.
//
//  La matrice resta ORTOGONALE anche con det -1, quindi inversa = trasposta
//  continua a valere: nessuna inversione numerica in tutto il motore.
//  ----------------------------------------------------------------------------
//
//  INVARIANTE FONDAMENTALE DEL REBASING:
//  la posizione in ECEF e' l'UNICA autorita'. La posizione in unita' Unreal e'
//  sempre un valore DERIVATO, mai stato primario. Percio' cambiare origine un
//  milione di volte non accumula deriva: non si compone mai una trasformazione
//  con la precedente, si ricalcola sempre da capo dall'ECEF.
// =============================================================================
#pragma once

#include "Geo/EnuFrame.h"
#include "Geo/GeoUnits.h"

namespace GeoWorld::Core
{
	class FGeoreference
	{
	public:
		FGeoreference() { SetOrigin(FGeodetic::FromDegrees(0.0, 0.0, 0.0)); }

		explicit FGeoreference(const FGeodetic& Origin, const FEllipsoid& InEllipsoid = WGS84)
			: Ellipsoid(InEllipsoid)
		{
			SetOrigin(Origin);
		}

		void SetOrigin(const FGeodetic& NewOrigin)
		{
			OriginGeodetic = NewOrigin;
			OriginEcef     = GeodeticToEcef(NewOrigin, Ellipsoid);
			EcefToUeBasis  = MakeNeuBasis(NewOrigin.LatRad, NewOrigin.LonRad);
			UeToEcefBasis  = EcefToUeBasis.Transposed();
		}

		const FEllipsoid& GetEllipsoid()      const { return Ellipsoid; }
		const FGeodetic&  GetOriginGeodetic() const { return OriginGeodetic; }
		const FEcef&      GetOriginEcef()     const { return OriginEcef; }
		const FMat3&      GetEcefToUeBasis()  const { return EcefToUeBasis; }

		// --- Le quattro conversioni che tutto il resto del motore usa ---------

		FUnrealPos EcefToUnreal(const FEcef& Position) const
		{
			// Traslazione in METRI e in double: e' qui che la sottrazione elimina
			// i sei zeri del raggio terrestre, prima che il numero incontri
			// qualunque cosa a precisione ridotta. L'ordine conta: sottrarre
			// PRIMA di scalare tiene i valori intermedi piccoli.
			const FEcef Local = EcefToUeBasis.Transform(Position - OriginEcef);
			return FUnrealPos{
				Local.X * Units::MetersToUu,
				Local.Y * Units::MetersToUu,
				Local.Z * Units::MetersToUu };
		}

		FEcef UnrealToEcef(const FUnrealPos& Position) const
		{
			const FEcef Local{
				Position.X * Units::UuToMeters,
				Position.Y * Units::UuToMeters,
				Position.Z * Units::UuToMeters };
			return UeToEcefBasis.Transform(Local) + OriginEcef;
		}

		FUnrealPos GeodeticToUnreal(const FGeodetic& Geodetic) const
		{
			return EcefToUnreal(GeodeticToEcef(Geodetic, Ellipsoid));
		}

		FGeodetic UnrealToGeodetic(const FUnrealPos& Position) const
		{
			return EcefToGeodetic(UnrealToEcef(Position), Ellipsoid);
		}

		// ---------------------------------------------------------------------
		//  Base di orientamento locale in un punto qualunque, espressa negli
		//  ASSI DI UNREAL. Le tre righe sono le direzioni (in spazio mondo Unreal)
		//  del Nord locale, dell'Est locale e dell'Alto locale in quel punto.
		//
		//  Serve per posare un oggetto "in piedi" sul terreno: la riga 0 diventa
		//  l'asse X dell'attore, la riga 1 l'asse Y, la riga 2 l'asse Z.
		//
		//  Perche' non basta la base dell'origine: a 500 km di distanza la
		//  verticale locale e' ruotata di ~4.5 gradi rispetto a quella
		//  dell'origine. Usare la base dell'origine ovunque significa oggetti
		//  visibilmente inclinati, ed e' l'errore che si nota subito guardando
		//  due cubi lontani fra loro.
		// ---------------------------------------------------------------------
		FMat3 GetLocalNeuBasisInUnreal(const FGeodetic& AtGeodetic) const
		{
			const FMat3 LocalNeuInEcef = MakeNeuBasis(AtGeodetic.LatRad, AtGeodetic.LonRad);

			// Ogni riga (un versore in ECEF) va ruotata negli assi di Unreal.
			// Non va traslata: e' una direzione, non un punto.
			return FMat3::FromRows(
				EcefToUeBasis.Transform(LocalNeuInEcef.GetRow(0)),   // Nord locale
				EcefToUeBasis.Transform(LocalNeuInEcef.GetRow(1)),   // Est locale
				EcefToUeBasis.Transform(LocalNeuInEcef.GetRow(2)));  // Alto locale
		}

		/**
		 * Ruota una DIREZIONE dallo spazio di Unreal a ECEF.
		 *
		 * Diversa da UnrealToEcef: una direzione non si trasla, si ruota
		 * soltanto. Applicare la traslazione a un vettore direzione darebbe un
		 * punto invece di una direzione, ed e' uno degli errori piu' facili da
		 * commettere e piu' difficili da vedere, perche' il risultato ha
		 * comunque l'aria di un vettore sensato.
		 *
		 * Nessuna conversione di unita': le direzioni sono adimensionali.
		 */
		FEcef UnrealDirectionToEcef(const FUnrealPos& Direction) const
		{
			return UeToEcefBasis.Transform(FEcef{ Direction.X, Direction.Y, Direction.Z });
		}

		// Distanza in METRI fra un punto in spazio Unreal e l'origine corrente.
		// E' il test del rebasing: non serve passare per l'ECEF, la norma si
		// conserva sotto rotazione.
		double DistanceFromOriginMeters(const FUnrealPos& Position) const
		{
			return Position.Length() * Units::UuToMeters;
		}

	private:
		FEllipsoid Ellipsoid = WGS84;
		FGeodetic  OriginGeodetic;
		FEcef      OriginEcef;
		FMat3      EcefToUeBasis;   // righe = North, East, Up   (det = -1)
		FMat3      UeToEcefBasis;   // trasposta della precedente
	};

} // namespace GeoWorld::Core
