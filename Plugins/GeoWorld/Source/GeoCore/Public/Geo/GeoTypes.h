// =============================================================================
//  GeoTypes.h -- Tipi fondamentali della geodesia.
//
//  STRATO: C++ PURO.
//  Questo file, e tutto cio' che sta sotto Public/Geo e Private/Geo, NON deve
//  includere NIENTE di Unreal Engine: niente CoreMinimal.h, niente FVector,
//  niente check()/UE_LOG, niente macro UCLASS/USTRUCT.
//
//  Perche' questa regola e' importante e non e' purismo:
//   1. Questo codice viene compilato ANCHE da Tools/StandaloneTests con un
//      normale g++/clang, senza Unreal. Cosi' la matematica si verifica con
//      numeri veri in un secondo, invece che aprendo l'editor.
//   2. In Fase 2 gli stessi identici valori si confrontano con pyproj/PROJ.
//   3. Se domani serve portare il core altrove, non e' incastrato nel motore.
//
//  CONVENZIONI FISSATE UNA VOLTA PER TUTTE:
//   - Tutto in double. Mai float in questo strato. Un float a distanza del
//     raggio terrestre espresso in centimetri ha un ULP di ~76 cm: inutilizzabile.
//   - Angoli in RADIANTI all'interno, gradi SOLO al confine (I/O, UI, config).
//   - Lunghezze in METRI. I centimetri (unita' Unreal) esistono solo dentro
//     FGeoreference, in un unico punto del codice.
// =============================================================================
#pragma once

#include <cmath>

namespace GeoWorld::Core
{
	// Costanti locali: non usiamo PI di Unreal (che e' float e vive in una macro)
	// ne' M_PI (che non e' standard e su MSVC richiede _USE_MATH_DEFINES).
	inline constexpr double Pi        = 3.14159265358979323846;
	inline constexpr double HalfPi    = Pi * 0.5;
	inline constexpr double TwoPi     = Pi * 2.0;
	inline constexpr double DegToRad  = Pi / 180.0;
	inline constexpr double RadToDeg  = 180.0 / Pi;

	// -------------------------------------------------------------------------
	//  FGeodetic -- coordinate geodetiche su un ellissoide (lat, lon, quota).
	//
	//  La quota e' ELLISSOIDICA (altezza sopra l'ellissoide WGS84), non
	//  ortometrica (sopra il geoide / livello del mare). E' una distinzione che
	//  in Italia vale tra -50 e +55 metri circa, quindi non e' un dettaglio: la
	//  pipeline di Fase 2 dovra' convertire le quote TINITALY con EGM2008.
	// -------------------------------------------------------------------------
	struct FGeodetic
	{
		double LatRad  = 0.0;   // latitudine  [-pi/2, +pi/2]
		double LonRad  = 0.0;   // longitudine [-pi, +pi]
		double HeightM = 0.0;   // quota ellissoidica in metri

		FGeodetic() = default;

		// NOTA DI DESIGN: il costruttore posizionale e' PRIVATO di proposito.
		// FGeodetic(41.9, 12.5, 40) non dice se il primo argomento e' lat o lon,
		// e scambiarli e' l'errore piu' comune del settore (GeoJSON usa lon,lat;
		// quasi tutto il resto usa lat,lon). Obbligando a passare per le factory,
		// il nome della funzione porta l'informazione e il compilatore la impone.
		static FGeodetic FromRadians(double InLatRad, double InLonRad, double InHeightM)
		{
			return FGeodetic(InLatRad, InLonRad, InHeightM);
		}

		static FGeodetic FromDegrees(double InLatDeg, double InLonDeg, double InHeightM)
		{
			return FGeodetic(InLatDeg * DegToRad, InLonDeg * DegToRad, InHeightM);
		}

		double LatDeg() const { return LatRad * RadToDeg; }
		double LonDeg() const { return LonRad * RadToDeg; }

	private:
		FGeodetic(double InLatRad, double InLonRad, double InHeightM)
			: LatRad(InLatRad), LonRad(InLonRad), HeightM(InHeightM) {}
	};

	// -------------------------------------------------------------------------
	//  FEcef -- Earth-Centered, Earth-Fixed. Cartesiane in METRI, destrorse.
	//  Origine nel centro di massa terrestre, Z verso il polo Nord, X verso
	//  l'intersezione fra equatore e meridiano di Greenwich, Y a completare.
	//
	//  E' un tipo DISTINTO e non un vettore generico: e' cosi' che impediamo al
	//  compilatore di accettare metri ECEF dove servono centimetri Unreal.
	// -------------------------------------------------------------------------
	struct FEcef
	{
		double X = 0.0, Y = 0.0, Z = 0.0;

		FEcef operator-(const FEcef& O) const { return FEcef{ X - O.X, Y - O.Y, Z - O.Z }; }
		FEcef operator+(const FEcef& O) const { return FEcef{ X + O.X, Y + O.Y, Z + O.Z }; }
		FEcef operator*(double S)       const { return FEcef{ X * S,   Y * S,   Z * S   }; }

		double Dot(const FEcef& O)      const { return X * O.X + Y * O.Y + Z * O.Z; }
		double LengthSquared()          const { return Dot(*this); }
		double Length()                 const { return std::sqrt(LengthSquared()); }

		FEcef Normalized() const
		{
			const double L = Length();
			return (L > 0.0) ? FEcef{ X / L, Y / L, Z / L } : FEcef{};
		}
	};

	// -------------------------------------------------------------------------
	//  FEnu -- frame cartesiano locale East-North-Up, in METRI, DESTRORSO.
	//  E' il frame "del cannocchiale": piano tangente all'ellissoide in un punto.
	// -------------------------------------------------------------------------
	struct FEnu
	{
		double E = 0.0, N = 0.0, U = 0.0;
	};

	// -------------------------------------------------------------------------
	//  FUnrealPos -- posizione nello spazio mondo di Unreal, in CENTIMETRI.
	//
	//  Perche' un tipo dedicato e non direttamente FVector: cosi' tutta la catena
	//  di trasformazione (incluso il rebasing) resta C++ puro e testabile senza
	//  Unreal. Lo strato Public/Unreal fa l'ultimo passo FUnrealPos <-> FVector,
	//  che e' una copia di tre double e basta.
	// -------------------------------------------------------------------------
	struct FUnrealPos
	{
		double X = 0.0, Y = 0.0, Z = 0.0;

		FUnrealPos operator-(const FUnrealPos& O) const { return FUnrealPos{ X - O.X, Y - O.Y, Z - O.Z }; }
		FUnrealPos operator+(const FUnrealPos& O) const { return FUnrealPos{ X + O.X, Y + O.Y, Z + O.Z }; }

		double LengthSquared() const { return X * X + Y * Y + Z * Z; }
		double Length()        const { return std::sqrt(LengthSquared()); }
	};

	// -------------------------------------------------------------------------
	//  FMat3 -- matrice 3x3, memorizzata PER RIGHE (row-major).
	//
	//  Ci serve una matrice nostra invece di FMatrix di Unreal per la regola di
	//  purezza di questo strato. E' minuscola: moltiplicazione per vettore,
	//  trasposta, prodotto. Non serve altro, perche' tutte le nostre matrici sono
	//  ORTONORMALI e quindi non invertiamo mai nulla: l'inversa e' la trasposta.
	// -------------------------------------------------------------------------
	struct FMat3
	{
		// M[riga][colonna]
		double M[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };

		// Costruisce la matrice che ha per righe i tre vettori dati.
		// Applicandola a un vettore v si ottiene (Row0.v, Row1.v, Row2.v), cioe'
		// le componenti di v nella base {Row0, Row1, Row2}: e' esattamente il
		// cambio di base da ECEF a un frame locale.
		static FMat3 FromRows(const FEcef& Row0, const FEcef& Row1, const FEcef& Row2)
		{
			FMat3 R;
			R.M[0][0] = Row0.X; R.M[0][1] = Row0.Y; R.M[0][2] = Row0.Z;
			R.M[1][0] = Row1.X; R.M[1][1] = Row1.Y; R.M[1][2] = Row1.Z;
			R.M[2][0] = Row2.X; R.M[2][1] = Row2.Y; R.M[2][2] = Row2.Z;
			return R;
		}

		FEcef GetRow(int Index) const
		{
			return FEcef{ M[Index][0], M[Index][1], M[Index][2] };
		}

		FMat3 Transposed() const
		{
			FMat3 R;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					R.M[i][j] = M[j][i];
			return R;
		}

		// Prodotto matrice-vettore, con il vettore trattato come colonna.
		FEcef Transform(const FEcef& V) const
		{
			return FEcef{
				M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z,
				M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
				M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z };
		}

		FMat3 operator*(const FMat3& O) const
		{
			FMat3 R;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					R.M[i][j] = M[i][0] * O.M[0][j] + M[i][1] * O.M[1][j] + M[i][2] * O.M[2][j];
			return R;
		}

		// Serve ai test: una base destrorsa ha det +1, una sinistrorsa -1.
		// La nostra matrice ECEF->Unreal DEVE avere determinante -1, perche'
		// converte un frame destrorso (ECEF/ENU) in uno sinistrorso (Unreal).
		double Determinant() const
		{
			return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
			     - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
			     + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
		}
	};

} // namespace GeoWorld::Core
