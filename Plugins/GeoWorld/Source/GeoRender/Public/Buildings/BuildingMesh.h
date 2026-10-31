// =============================================================================
//  BuildingMesh.h -- Dalle sagome degli edifici ai triangoli. STRATO: C++
//  PURO, header-only.
//
//  Per ogni edificio di una tile .gbd (BuildingTileFormat.h):
//
//   1. LA BASE: la quota piu' bassa del terreno DISEGNATO sotto il contorno
//      (GroundCells.h). I muri partono un metro e mezzo piu' giu': su un
//      pendio il lato a monte resta sepolto, quello a valle non galleggia.
//   2. I MURI: un rettangolo per lato, del contorno e dei cortili, girato
//      verso fuori (verso il cortile, per i cortili).
//   3. IL TETTO, secondo la forma (dai tag, o decisa qui dalla classe e
//      dalla sagoma):
//        piano       il contorno triangolato, cortili compresi;
//        a due falde il colmo lungo il lato lungo, i timpani di muro;
//        a padiglione il colmo accorciato, quattro falde;
//        a piramide  la punta sul baricentro;
//        a cupola    anelli che si stringono, come una sfera.
//      Le falde si fanno solo dove la geometria e' semplice: due falde e
//      padiglione su un contorno di 4 lati, piramide e cupola su un contorno
//      convesso senza cortili. Altrimenti il tetto e' piano (per un tetto a
//      falde su una L servirebbe lo "scheletro" del poligono: un'altra volta).
//   4. I COLORI: quelli dei tag, o una tavolozza per classe (intonaci italiani:
//      ocra, crema, salmone; terracotta per i tetti a falde; grigio per quelli
//      piani), scelta con il caso dell'edificio.
//
//  Cosa porta ogni vertice al materiale (BuildingShader.inl):
//    colore  RGB il colore (sRGB), A 255 muro / 0 tetto
//    UV0     facciata: metri lungo il muro, metri sopra la base
//    UV1     stile: altezza di un piano (0 = niente finestre), un caso 0..1
//  Le finestre non sono triangoli: le disegna il materiale, piano per piano.
//
//  I triangoli girano come quelli del terreno e degli alberi: la normale
//  geometrica (B - A) x (C - A) punta fuori. I test lo verificano.
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Mesh/GroundCells.h"
#include "Tiles/BuildingTileFormat.h"

namespace GeoWorld::Buildings
{
	using Tiles::EBuildingClass;
	using Tiles::ERoofShape;
	using Tiles::FBuilding;
	using Tiles::FBuildingTile;

	struct FBuildingMesh
	{
		std::vector<float> Positions;   // 3 per vertice, metri nel frame della cella (nord, est, alto)
		std::vector<float> Normals;     // 3 per vertice
		std::vector<uint8_t> Colours;   // 4 per vertice: RGB sRGB, A 255 muro / 0 tetto
		std::vector<float> Facade;      // 2 per vertice: lungo il muro, sopra la base (m)
		std::vector<float> Style;       // 2 per vertice: altezza di un piano (0 = niente finestre), caso
		std::vector<uint32_t> Indices;  // 3 per triangolo

		size_t VertexCount() const { return Positions.size() / 3; }
		size_t TriangleCount() const { return Indices.size() / 3; }
	};

	/** Quanto dettaglio: la banda vicina tutto, quelle lontane meno. */
	struct FBuildingDetail
	{
		/** Tolleranza della semplificazione dei contorni, m (0: nessuna). */
		double SimplifyM = 0.0;
		/** Gli edifici piu' piccoli di cosi' (m^2 di pianta) non si disegnano. */
		double MinAreaM2 = 0.0;
		/** Le finestre nel materiale (UV1.x = 0 le spegne). */
		bool bWindows = true;
		/** I tetti a falde (spento: tutti piani). */
		bool bSlopedRoofs = true;
		/** Quanto scendono i muri sotto la quota piu' bassa del suolo, m. */
		double FoundationM = 1.5;
	};

	/** Un edificio che non si estrude: al suo posto un modello della libreria (un monumento). */
	struct FLandmarkSpot
	{
		uint64_t OsmId = 0;
		uint32_t Wikidata = 0;
		/** Il baricentro del contorno alla quota della base, nel frame della cella (m). */
		double Local[3] = { 0.0, 0.0, 0.0 };
		/** La direzione del lato piu' lungo, radianti dal nord verso est. */
		double YawRad = 0.0;
		/** Lato lungo e lato corto del contorno, m. */
		double LengthM = 0.0, WidthM = 0.0;
	};

	struct FBuildingCellStats
	{
		int32_t Built = 0;
		int32_t Skipped = 0;        // troppo piccoli per la banda, o senza suolo sotto
		int32_t SlopedRoofs = 0;
		int32_t Landmarks = 0;
		/** Parti tolte perche' il loro contorno e' diventato un monumento. */
		int32_t HiddenParts = 0;
	};

	namespace BuildingMeshDetail
	{
		struct FPoint
		{
			double X = 0.0, Y = 0.0;   // X est, Y nord, metri
		};

		inline double Cross(const FPoint& O, const FPoint& A, const FPoint& B)
		{
			return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X);
		}

		inline double SignedArea(const std::vector<FPoint>& Ring)
		{
			double Sum = 0.0;
			for (size_t I = 0, N = Ring.size(); I < N; ++I)
			{
				const FPoint& A = Ring[I];
				const FPoint& B = Ring[(I + 1) % N];
				Sum += A.X * B.Y - B.X * A.Y;
			}
			return 0.5 * Sum;
		}

		inline double Distance(const FPoint& A, const FPoint& B) { return std::hypot(B.X - A.X, B.Y - A.Y); }

		inline bool IsConvex(const std::vector<FPoint>& Ring)
		{
			const size_t N = Ring.size();
			if (N < 3) { return false; }
			for (size_t I = 0; I < N; ++I)
			{
				if (Cross(Ring[I], Ring[(I + 1) % N], Ring[(I + 2) % N]) < -1e-6) { return false; }
			}
			return true;
		}

		/** Toglie i vertici quasi allineati (deviazione sotto ~6°) e i lati sotto MinEdge: le "spezzature" dei rilievi. */
		inline std::vector<FPoint> Clean(const std::vector<FPoint>& Ring, double MinEdge)
		{
			std::vector<FPoint> Out = Ring;
			bool bChanged = true;
			while (bChanged && Out.size() > 3)
			{
				bChanged = false;
				for (size_t I = 0; I < Out.size() && Out.size() > 3; ++I)
				{
					const FPoint& Prev = Out[(I + Out.size() - 1) % Out.size()];
					const FPoint& Here = Out[I];
					const FPoint& Next = Out[(I + 1) % Out.size()];
					const double A = Distance(Prev, Here), B = Distance(Here, Next);
					const double Sine = std::fabs(Cross(Prev, Here, Next)) / std::max(1e-9, A * B);
					const double Dot = (Here.X - Prev.X) * (Next.X - Here.X) + (Here.Y - Prev.Y) * (Next.Y - Here.Y);
					if (A < MinEdge || (Sine < 0.1 && Dot > 0.0))
					{
						Out.erase(Out.begin() + static_cast<std::ptrdiff_t>(I));
						bChanged = true;
						break;
					}
				}
			}
			return Out;
		}

		/** Douglas-Peucker su un anello chiuso: si tengono i due punti piu' lontani e si semplifica fra loro. */
		inline void SimplifySpan(const std::vector<FPoint>& Ring, size_t First, size_t Last, double Tolerance, std::vector<bool>& Keep)
		{
			const size_t N = Ring.size();
			if ((Last + N - First) % N < 2) { return; }
			double Worst = -1.0;
			size_t WorstIndex = First;
			const FPoint& A = Ring[First];
			const FPoint& B = Ring[Last];
			const double Length = std::max(1e-9, Distance(A, B));
			for (size_t I = (First + 1) % N; I != Last; I = (I + 1) % N)
			{
				const double D = std::fabs(Cross(A, B, Ring[I])) / Length;
				if (D > Worst) { Worst = D; WorstIndex = I; }
			}
			if (Worst > Tolerance)
			{
				Keep[WorstIndex] = true;
				SimplifySpan(Ring, First, WorstIndex, Tolerance, Keep);
				SimplifySpan(Ring, WorstIndex, Last, Tolerance, Keep);
			}
		}

		inline std::vector<FPoint> Simplify(const std::vector<FPoint>& Ring, double Tolerance)
		{
			const size_t N = Ring.size();
			if (Tolerance <= 0.0 || N <= 4) { return Ring; }
			size_t Far = 0;
			double FarDistance = -1.0;
			for (size_t I = 1; I < N; ++I)
			{
				const double D = Distance(Ring[0], Ring[I]);
				if (D > FarDistance) { FarDistance = D; Far = I; }
			}
			std::vector<bool> Keep(N, false);
			Keep[0] = Keep[Far] = true;
			SimplifySpan(Ring, 0, Far, Tolerance, Keep);
			SimplifySpan(Ring, Far, 0, Tolerance, Keep);
			std::vector<FPoint> Out;
			for (size_t I = 0; I < N; ++I) { if (Keep[I]) { Out.push_back(Ring[I]); } }
			return Out.size() >= 3 ? Out : Ring;
		}

		inline bool SegmentsCross(const FPoint& A, const FPoint& B, const FPoint& C, const FPoint& D)
		{
			const double D1 = Cross(C, D, A), D2 = Cross(C, D, B), D3 = Cross(A, B, C), D4 = Cross(A, B, D);
			return ((D1 > 0.0) != (D2 > 0.0)) && ((D3 > 0.0) != (D4 > 0.0))
				&& std::fabs(D1) > 1e-12 && std::fabs(D2) > 1e-12 && std::fabs(D3) > 1e-12 && std::fabs(D4) > 1e-12;
		}

		inline bool InTriangle(const FPoint& P, const FPoint& A, const FPoint& B, const FPoint& C)
		{
			return Cross(A, B, P) > 1e-12 && Cross(B, C, P) > 1e-12 && Cross(C, A, P) > 1e-12;
		}

		/**
		 * Triangola un poligono con i buchi (contorno antiorario, buchi orari).
		 * Points: tutti i vertici, il contorno e poi i buchi; Rings: dove
		 * comincia e quanti sono quelli di ogni anello. Out: terne di indici in
		 * Points, antiorarie.
		 *
		 * COME. Prima ogni buco si "cuce" al contorno con un ponte: dal suo
		 * vertice piu' a est al vertice piu' vicino che si vede senza tagliare
		 * nessun lato (il poligono diventa uno solo, che passa due volte per il
		 * ponte). Poi si tagliano le "orecchie": tre vertici consecutivi che
		 * fanno un triangolo convesso senza altri vertici dentro.
		 */
		inline bool Triangulate(const std::vector<FPoint>& Points, const std::vector<std::array<size_t, 2>>& Rings,
		                        std::vector<std::array<uint32_t, 3>>& Out)
		{
			Out.clear();
			if (Rings.empty() || Rings[0][1] < 3) { return false; }
			std::vector<uint32_t> Polygon;
			for (size_t I = 0; I < Rings[0][1]; ++I) { Polygon.push_back(static_cast<uint32_t>(Rings[0][0] + I)); }

			// I buchi, dal piu' a est.
			std::vector<size_t> Order;
			for (size_t Ring = 1; Ring < Rings.size(); ++Ring) { if (Rings[Ring][1] >= 3) { Order.push_back(Ring); } }
			auto MaxX = [&](size_t Ring)
			{
				double Best = -1e300;
				for (size_t I = 0; I < Rings[Ring][1]; ++I) { Best = std::max(Best, Points[Rings[Ring][0] + I].X); }
				return Best;
			};
			std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return MaxX(A) > MaxX(B); });
			std::vector<bool> Merged(Rings.size(), false);
			Merged[0] = true;

			auto Blocked = [&](const FPoint& A, const FPoint& B, uint32_t SkipA, uint32_t SkipB)
			{
				// Taglia un lato del poligono cucito fin qui, o di un buco non ancora cucito?
				for (size_t I = 0, N = Polygon.size(); I < N; ++I)
				{
					const uint32_t P = Polygon[I], Q = Polygon[(I + 1) % N];
					if (P == SkipA || Q == SkipA || P == SkipB || Q == SkipB) { continue; }
					if (SegmentsCross(A, B, Points[P], Points[Q])) { return true; }
				}
				for (size_t Ring = 1; Ring < Rings.size(); ++Ring)
				{
					if (Merged[Ring]) { continue; }
					for (size_t I = 0, N = Rings[Ring][1]; I < N; ++I)
					{
						const uint32_t P = static_cast<uint32_t>(Rings[Ring][0] + I);
						const uint32_t Q = static_cast<uint32_t>(Rings[Ring][0] + (I + 1) % N);
						if (P == SkipA || Q == SkipA || P == SkipB || Q == SkipB) { continue; }
						if (SegmentsCross(A, B, Points[P], Points[Q])) { return true; }
					}
				}
				return false;
			};

			for (const size_t Ring : Order)
			{
				const size_t Start = Rings[Ring][0], Count = Rings[Ring][1];
				size_t East = 0;
				for (size_t I = 1; I < Count; ++I) { if (Points[Start + I].X > Points[Start + East].X) { East = I; } }
				const uint32_t HoleVertex = static_cast<uint32_t>(Start + East);
				// I vertici del poligono dal piu' vicino: il primo che si vede.
				std::vector<size_t> Candidates(Polygon.size());
				for (size_t I = 0; I < Candidates.size(); ++I) { Candidates[I] = I; }
				std::sort(Candidates.begin(), Candidates.end(), [&](size_t A, size_t B)
				{
					return Distance(Points[HoleVertex], Points[Polygon[A]]) < Distance(Points[HoleVertex], Points[Polygon[B]]);
				});
				size_t Bridge = Polygon.size();
				for (const size_t Candidate : Candidates)
				{
					if (!Blocked(Points[HoleVertex], Points[Polygon[Candidate]], HoleVertex, Polygon[Candidate]))
					{
						Bridge = Candidate;
						break;
					}
				}
				Merged[Ring] = true;
				if (Bridge == Polygon.size()) { continue; }    // nessun ponte: il buco si perde, il tetto c'e' lo stesso
				// ... ponte, buco (dal vertice piu' a est, tutto il giro), di nuovo il vertice del buco, ponte ...
				std::vector<uint32_t> Insert;
				for (size_t I = 0; I <= Count; ++I) { Insert.push_back(static_cast<uint32_t>(Start + (East + I) % Count)); }
				Insert.push_back(Polygon[Bridge]);
				Polygon.insert(Polygon.begin() + static_cast<std::ptrdiff_t>(Bridge) + 1, Insert.begin(), Insert.end());
			}

			// Le orecchie.
			std::vector<uint32_t> Left = Polygon;
			size_t Guard = 0;
			while (Left.size() > 3 && Guard < 4 * Polygon.size() * Polygon.size() + 64)
			{
				++Guard;
				const size_t N = Left.size();
				size_t Ear = N, Fallback = N;
				double FallbackTurn = -1e300;
				for (size_t I = 0; I < N; ++I)
				{
					const uint32_t A = Left[(I + N - 1) % N], B = Left[I], C = Left[(I + 1) % N];
					const double Turn = Cross(Points[A], Points[B], Points[C]);
					if (Turn > FallbackTurn) { FallbackTurn = Turn; Fallback = I; }
					if (Turn <= 1e-12) { continue; }
					bool bEmpty = true;
					for (size_t J = 0; J < N && bEmpty; ++J)
					{
						const uint32_t P = Left[J];
						if (P == A || P == B || P == C) { continue; }
						const FPoint& Q = Points[P];
						// I vertici doppi del ponte stanno sui vertici del triangolo: non contano.
						if ((Q.X == Points[A].X && Q.Y == Points[A].Y) || (Q.X == Points[B].X && Q.Y == Points[B].Y)
							|| (Q.X == Points[C].X && Q.Y == Points[C].Y)) { continue; }
						if (InTriangle(Q, Points[A], Points[B], Points[C])) { bEmpty = false; }
					}
					if (bEmpty) { Ear = I; break; }
				}
				// Nessuna orecchia pulita (vertici quasi allineati, sagome storte):
				// si taglia la piu' convessa, per andare avanti comunque.
				const size_t Cut = (Ear < N) ? Ear : Fallback;
				const uint32_t A = Left[(Cut + N - 1) % N], B = Left[Cut], C = Left[(Cut + 1) % N];
				if (Cross(Points[A], Points[B], Points[C]) > 1e-12) { Out.push_back({ A, B, C }); }
				Left.erase(Left.begin() + static_cast<std::ptrdiff_t>(Cut));
			}
			if (Left.size() == 3 && Cross(Points[Left[0]], Points[Left[1]], Points[Left[2]]) > 1e-12)
			{
				Out.push_back({ Left[0], Left[1], Left[2] });
			}
			return !Out.empty();
		}

		/** Un numero 0..1 da un id e un indice: i colori, le varianti del tetto. */
		inline double Random(uint64_t Id, uint32_t Salt)
		{
			uint64_t Value = Id * 0x9E3779B97F4A7C15ull + (static_cast<uint64_t>(Salt) + 1) * 0xBF58476D1CE4E5B9ull;
			Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ull;
			Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBull;
			Value ^= Value >> 31;
			return static_cast<double>(Value >> 11) * (1.0 / 9007199254740992.0);
		}

		/** La tavolozza: (R, G, B) sRGB. */
		struct FColour { uint8_t R, G, B; };

		inline FColour WallPalette(uint8_t Class, double Pick)
		{
			// Intonaci italiani: ocra, crema, salmone, giallo, rosso mattone, bianco.
			static const FColour Plaster[] = { { 225, 200, 150 }, { 232, 214, 170 }, { 214, 170, 120 }, { 222, 165, 135 },
				{ 200, 125, 95 }, { 236, 228, 205 }, { 196, 186, 168 }, { 228, 196, 130 } };
			static const FColour Modern[] = { { 205, 205, 200 }, { 226, 223, 216 }, { 182, 178, 172 }, { 214, 206, 190 } };
			static const FColour Concrete[] = { { 178, 178, 172 }, { 152, 152, 150 }, { 196, 190, 180 }, { 165, 160, 150 } };
			static const FColour Stone[] = { { 216, 202, 172 }, { 190, 122, 92 }, { 236, 226, 206 }, { 200, 185, 160 } };
			auto Pick4 = [Pick](const FColour* Table) { return Table[std::min(3, static_cast<int>(Pick * 4.0))]; };
			switch (static_cast<EBuildingClass>(Class))
			{
			case EBuildingClass::Commercial:
			case EBuildingClass::Civic: return Pick4(Modern);
			case EBuildingClass::Industrial:
			case EBuildingClass::Agricultural:
			case EBuildingClass::Small:
			case EBuildingClass::Roof: return Pick4(Concrete);
			case EBuildingClass::Religious:
			case EBuildingClass::Tower:
			case EBuildingClass::Historic: return Pick4(Stone);
			default: return Plaster[std::min(7, static_cast<int>(Pick * 8.0))];
			}
		}

		inline FColour RoofPalette(bool bSloped, uint8_t Class, double Pick)
		{
			static const FColour Tiles[] = { { 170, 85, 60 }, { 185, 102, 72 }, { 150, 76, 56 }, { 162, 96, 76 } };
			static const FColour Flat[] = { { 122, 120, 116 }, { 142, 139, 132 }, { 100, 99, 97 }, { 160, 155, 145 } };
			if (bSloped && static_cast<EBuildingClass>(Class) != EBuildingClass::Industrial)
			{
				return Tiles[std::min(3, static_cast<int>(Pick * 4.0))];
			}
			return Flat[std::min(3, static_cast<int>(Pick * 4.0))];
		}

		/** Le classi con le finestre disegnate dal materiale. */
		inline bool HasWindows(uint8_t Class)
		{
			switch (static_cast<EBuildingClass>(Class))
			{
			case EBuildingClass::Other:
			case EBuildingClass::House:
			case EBuildingClass::Apartments:
			case EBuildingClass::Commercial:
			case EBuildingClass::Civic:
			case EBuildingClass::Historic: return true;
			default: return false;
			}
		}

		/** Chi riceve i vertici: tiene i vettori di FBuildingMesh e gira i triangoli verso fuori. */
		class FWriter
		{
		public:
			explicit FWriter(FBuildingMesh& InOut) : Out(InOut) {}

			uint32_t Vertex(double North, double East, double Up, double NNorth, double NEast, double NUp,
			                const FColour& Colour, bool bWall, double Along, double Height, float FloorM, float Random)
			{
				const double Length = std::sqrt(NNorth * NNorth + NEast * NEast + NUp * NUp);
				const double Scale = Length > 1e-12 ? 1.0 / Length : 0.0;
				for (const double Value : { North, East, Up }) { Out.Positions.push_back(static_cast<float>(Value)); }
				for (const double Value : { NNorth * Scale, NEast * Scale, NUp * Scale }) { Out.Normals.push_back(static_cast<float>(Value)); }
				Out.Colours.push_back(Colour.R);
				Out.Colours.push_back(Colour.G);
				Out.Colours.push_back(Colour.B);
				Out.Colours.push_back(bWall ? 255 : 0);
				Out.Facade.push_back(static_cast<float>(Along));
				Out.Facade.push_back(static_cast<float>(Height));
				Out.Style.push_back(FloorM);
				Out.Style.push_back(Random);
				return static_cast<uint32_t>(Out.Positions.size() / 3 - 1);
			}

			/** Un triangolo girato in modo che la sua normale geometrica stia dalla parte di Outward (nord, est, alto). */
			void Triangle(uint32_t A, uint32_t B, uint32_t C, double ONorth, double OEast, double OUp)
			{
				const float* PA = &Out.Positions[A * 3];
				const float* PB = &Out.Positions[B * 3];
				const float* PC = &Out.Positions[C * 3];
				const double U[3] = { PB[0] - PA[0], PB[1] - PA[1], PB[2] - PA[2] };
				const double V[3] = { PC[0] - PA[0], PC[1] - PA[1], PC[2] - PA[2] };
				const double F[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
				if (F[0] * ONorth + F[1] * OEast + F[2] * OUp < 0.0) { std::swap(B, C); }
				Out.Indices.push_back(A);
				Out.Indices.push_back(B);
				Out.Indices.push_back(C);
			}

		private:
			FBuildingMesh& Out;
		};
	}

	/** La forma del tetto che si costruisce davvero, data la sagoma (e la scelta se e' Auto). */
	inline ERoofShape ResolveRoof(const FBuilding& Building, size_t OuterVertices, bool bConvex, bool bHoles,
	                              double AreaM2, bool bSlopedAllowed)
	{
		using namespace BuildingMeshDetail;
		ERoofShape Shape = static_cast<ERoofShape>(Building.Roof);
		if (Shape == ERoofShape::Auto)
		{
			const double Pick = Random(Building.OsmId, 7);
			switch (static_cast<EBuildingClass>(Building.Class))
			{
			case EBuildingClass::House:
				Shape = Pick < 0.6 ? ERoofShape::Hipped : ERoofShape::Gabled;
				break;
			case EBuildingClass::Agricultural:
			case EBuildingClass::Religious:
				Shape = ERoofShape::Gabled;
				break;
			case EBuildingClass::Small:
				Shape = Pick < 0.5 ? ERoofShape::Gabled : ERoofShape::Flat;
				break;
			case EBuildingClass::Apartments:
			case EBuildingClass::Historic:
				Shape = ERoofShape::Hipped;
				break;
			case EBuildingClass::Tower:
				Shape = ERoofShape::Pyramidal;
				break;
			case EBuildingClass::Other:
				// Un edificio piccolo e basso e' quasi sempre una casa.
				Shape = (AreaM2 < 400.0 && Building.Levels <= 3) ? (Pick < 0.6 ? ERoofShape::Hipped : ERoofShape::Gabled) : ERoofShape::Flat;
				break;
			default:
				Shape = ERoofShape::Flat;
				break;
			}
		}
		if (!bSlopedAllowed) { return ERoofShape::Flat; }
		switch (Shape)
		{
		case ERoofShape::Gabled:
		case ERoofShape::Hipped:
			return (OuterVertices == 4 && bConvex && !bHoles) ? Shape : ERoofShape::Flat;
		case ERoofShape::Pyramidal:
		case ERoofShape::Dome:
			return (bConvex && !bHoles && OuterVertices <= 64) ? Shape : ERoofShape::Flat;
		default:
			return ERoofShape::Flat;
		}
	}

	/**
	 * Un edificio nel frame della cella. Ground da' la quota del terreno
	 * disegnato; GeoidM non serve (le altezze sono dal suolo). Falso se non
	 * si costruisce (troppo piccolo, o niente suolo sotto).
	 */
	inline bool BuildBuilding(const FBuilding& Building, const Cells::FCellFrame& Frame, const Cells::FGroundSurfaces& Ground,
	                          const FBuildingDetail& Detail, FBuildingMesh& Out, bool* OutSloped = nullptr,
	                          FLandmarkSpot* OutSpot = nullptr)
	{
		using namespace BuildingMeshDetail;
		if (Building.Rings.empty() || Building.Rings[0].Size() < 3) { return false; }

		// 1. Gli anelli in metri (X est, Y nord) nel frame della cella, e la base.
		std::vector<std::vector<FPoint>> Rings;
		double Base = 1e300;
		bool bGround = false;
		for (size_t RingIndex = 0; RingIndex < Building.Rings.size(); ++RingIndex)
		{
			const Tiles::FBuildingRing& Source = Building.Rings[RingIndex];
			std::vector<FPoint> Ring;
			Ring.reserve(Source.Size());
			for (size_t I = 0; I < Source.Size(); ++I)
			{
				double Local[3];
				Frame.Local(Source.Lat[I], Source.Lon[I], 0.0, Local);
				Ring.push_back(FPoint{ Local[1], Local[0] });
				if (RingIndex == 0)
				{
					double Height = 0.0, Slope = 0.0;
					if (Ground.GroundAt(Source.Lat[I], Source.Lon[I], Height, Slope))
					{
						Base = std::min(Base, Height);
						bGround = true;
					}
				}
			}
			Ring = Clean(Ring, 0.3);
			if (Detail.SimplifyM > 0.0) { Ring = Simplify(Ring, Detail.SimplifyM); }
			if (Ring.size() < 3)
			{
				if (RingIndex == 0) { return false; }
				continue;
			}
			// L'orientamento lo garantisce la pipeline; per sicurezza si rifa'.
			const double Area = SignedArea(Ring);
			if ((RingIndex == 0) != (Area > 0.0)) { std::reverse(Ring.begin(), Ring.end()); }
			Rings.push_back(std::move(Ring));
		}
		if (!bGround || Rings.empty()) { return false; }
		const std::vector<FPoint>& Outer = Rings[0];
		const double AreaM2 = SignedArea(Outer);
		if (AreaM2 < std::max(1.0, Detail.MinAreaM2)) { return false; }

		// La curvatura: il piano del frame si stacca dalla Terra di d^2 / 2R
		// (1 cm a 350 m). Per ogni vertice la quota "alto" del frame.
		auto Drop = [](double East, double North)
		{
			return -(East * East + North * North) / (2.0 * 6371000.0);
		};

		// 2. Le altezze.
		const bool bConvex = IsConvex(Outer);
		const bool bHoles = Rings.size() > 1;
		const ERoofShape Roof = ResolveRoof(Building, Outer.size(), bConvex, bHoles, AreaM2, Detail.bSlopedRoofs);
		const bool bSloped = Roof != ERoofShape::Flat;
		double ShortSide = 1e300, LongSide = 0.0;
		for (size_t I = 0; I < Outer.size(); ++I)
		{
			const double Length = Distance(Outer[I], Outer[(I + 1) % Outer.size()]);
			ShortSide = std::min(ShortSide, Length);
			LongSide = std::max(LongSide, Length);
		}
		double MinX = 1e300, MaxX = -1e300, MinY = 1e300, MaxY = -1e300;
		for (const FPoint& Point : Outer)
		{
			MinX = std::min(MinX, Point.X); MaxX = std::max(MaxX, Point.X);
			MinY = std::min(MinY, Point.Y); MaxY = std::max(MaxY, Point.Y);
		}
		const double Extent = std::min(MaxX - MinX, MaxY - MinY);
		double RoofHeight = Building.RoofHeightM;
		if (bSloped && RoofHeight <= 0.0)
		{
			const bool bSteep = static_cast<EBuildingClass>(Building.Class) == EBuildingClass::Tower
			                 || static_cast<EBuildingClass>(Building.Class) == EBuildingClass::Religious;
			const double Pitch = bSteep ? 0.9 : (Building.Class == static_cast<uint8_t>(EBuildingClass::Apartments) ? 0.3 : 0.45);
			switch (Roof)
			{
			case ERoofShape::Gabled:
			case ERoofShape::Hipped: RoofHeight = Pitch * 0.5 * (Outer.size() == 4 ? std::min(Distance(Outer[0], Outer[1]), Distance(Outer[1], Outer[2])) : ShortSide); break;
			case ERoofShape::Pyramidal: RoofHeight = (bSteep ? 1.6 : Pitch) * 0.5 * Extent; break;
			case ERoofShape::Dome: RoofHeight = 0.5 * Extent; break;
			default: break;
			}
			RoofHeight = std::min(RoofHeight, bSteep ? 30.0 : 8.0);
		}
		if (!bSloped) { RoofHeight = 0.0; }
		const double MinHeight = Building.MinHeightM;
		double WallTop = Building.HeightM;
		// Un'altezza misurata (o con il tetto nei tag) e' la cima: il tetto sta dentro.
		const bool bRoofInside = (Building.Flags & Tiles::BuildingHeightMeasured) != 0 || Building.RoofHeightM > 0.0f;
		if (bRoofInside) { WallTop -= RoofHeight; }
		if (WallTop < MinHeight + 0.5)
		{
			RoofHeight = std::max(0.0, RoofHeight - (MinHeight + 0.5 - WallTop));
			WallTop = MinHeight + 0.5;
		}
		const bool bRoofOnly = static_cast<EBuildingClass>(Building.Class) == EBuildingClass::Roof;

		// 3. I colori e lo stile.
		const double PickWall = Random(Building.OsmId, 1), PickRoof = Random(Building.OsmId, 2);
		FColour WallColour = WallPalette(Building.Class, PickWall);
		FColour RoofColour = RoofPalette(bSloped, Building.Class, PickRoof);
		if (Building.Flags & Tiles::BuildingWallColour) { WallColour = FColour{ Building.WallColour[0], Building.WallColour[1], Building.WallColour[2] }; }
		if (Building.Flags & Tiles::BuildingRoofColour) { RoofColour = FColour{ Building.RoofColour[0], Building.RoofColour[1], Building.RoofColour[2] }; }
		const int32_t Levels = std::max<int32_t>(1, Building.Levels);
		const float FloorM = (Detail.bWindows && HasWindows(Building.Class) && !bRoofOnly)
			? static_cast<float>(std::max(2.6, (WallTop - MinHeight) / Levels)) : 0.0f;
		const float Chance = static_cast<float>(Random(Building.OsmId, 3));

		// Il punto per un monumento: base, baricentro, lato lungo.
		if (OutSpot)
		{
			double Cx = 0.0, Cy = 0.0;
			for (const FPoint& Point : Outer) { Cx += Point.X; Cy += Point.Y; }
			Cx /= static_cast<double>(Outer.size());
			Cy /= static_cast<double>(Outer.size());
			size_t Longest = 0;
			for (size_t I = 1; I < Outer.size(); ++I)
			{
				if (Distance(Outer[I], Outer[(I + 1) % Outer.size()]) > Distance(Outer[Longest], Outer[(Longest + 1) % Outer.size()])) { Longest = I; }
			}
			const FPoint& A = Outer[Longest];
			const FPoint& B = Outer[(Longest + 1) % Outer.size()];
			OutSpot->OsmId = Building.OsmId;
			OutSpot->Wikidata = Building.Wikidata;
			OutSpot->Local[0] = Cy;
			OutSpot->Local[1] = Cx;
			OutSpot->Local[2] = Base + Drop(Cx, Cy);
			OutSpot->YawRad = std::atan2(B.X - A.X, B.Y - A.Y);
			OutSpot->LengthM = LongSide;
			OutSpot->WidthM = Extent;
			return true;    // al suo posto un modello: niente muri ne' tetto
		}

		FWriter Mesh(Out);
		const double Bottom = (MinHeight > 0.0) ? Base + MinHeight : Base - Detail.FoundationM;
		const double Top = Base + WallTop;

		// 4. I muri: un rettangolo per lato, verso fuori (a destra del verso dell'anello).
		// (Una pensilina ha "muri" alti mezzo metro: lo spessore del tetto.)
		{
			for (const std::vector<FPoint>& Ring : Rings)
			{
				for (size_t I = 0, N = Ring.size(); I < N; ++I)
				{
					const FPoint& A = Ring[I];
					const FPoint& B = Ring[(I + 1) % N];
					const double Length = Distance(A, B);
					if (Length < 1e-3) { continue; }
					// Fuori: (dy, -dx) nel piano est-nord, cioe' nord = -dx, est = dy.
					const double OutEast = (B.Y - A.Y) / Length, OutNorth = -(B.X - A.X) / Length;
					const double DropA = Drop(A.X, A.Y), DropB = Drop(B.X, B.Y);
					const float WallFloor = bRoofOnly ? 0.0f : FloorM;
					const uint32_t V0 = Mesh.Vertex(A.Y, A.X, Bottom + DropA, OutNorth, OutEast, 0.0, WallColour, true, 0.0, Bottom - Base, WallFloor, Chance);
					const uint32_t V1 = Mesh.Vertex(B.Y, B.X, Bottom + DropB, OutNorth, OutEast, 0.0, WallColour, true, Length, Bottom - Base, WallFloor, Chance);
					const uint32_t V2 = Mesh.Vertex(B.Y, B.X, Top + DropB, OutNorth, OutEast, 0.0, WallColour, true, Length, WallTop, WallFloor, Chance);
					const uint32_t V3 = Mesh.Vertex(A.Y, A.X, Top + DropA, OutNorth, OutEast, 0.0, WallColour, true, 0.0, WallTop, WallFloor, Chance);
					Mesh.Triangle(V0, V1, V2, OutNorth, OutEast, 0.0);
					Mesh.Triangle(V0, V2, V3, OutNorth, OutEast, 0.0);
				}
			}
		}

		// Sotto un corpo sospeso (min_height, pensiline) si vede il fondo.
		std::vector<FPoint> Points;
		std::vector<std::array<size_t, 2>> Spans;
		for (const std::vector<FPoint>& Ring : Rings)
		{
			Spans.push_back({ Points.size(), Ring.size() });
			Points.insert(Points.end(), Ring.begin(), Ring.end());
		}
		auto FlatFace = [&](double Height, bool bUp, const FColour& Colour, bool bWall)
		{
			std::vector<std::array<uint32_t, 3>> Triangles;
			if (!Triangulate(Points, Spans, Triangles)) { return; }
			std::vector<uint32_t> Map(Points.size(), UINT32_MAX);
			for (const std::array<uint32_t, 3>& Triangle : Triangles)
			{
				for (const uint32_t Corner : Triangle)
				{
					if (Map[Corner] == UINT32_MAX)
					{
						const FPoint& P = Points[Corner];
						Map[Corner] = Mesh.Vertex(P.Y, P.X, Height + Drop(P.X, P.Y), 0.0, 0.0, bUp ? 1.0 : -1.0, Colour, bWall,
							P.X, P.Y, 0.0f, Chance);
					}
				}
				Mesh.Triangle(Map[Triangle[0]], Map[Triangle[1]], Map[Triangle[2]], 0.0, 0.0, bUp ? 1.0 : -1.0);
			}
		};
		if (MinHeight > 0.0) { FlatFace(Bottom, false, WallColour, true); }

		// 5. Il tetto.
		const double Ridge = Top + RoofHeight;
		auto RoofVertex = [&](const FPoint& P, double Height, double NEast, double NNorth, double NUp)
		{
			return Mesh.Vertex(P.Y, P.X, Height + Drop(P.X, P.Y), NNorth, NEast, NUp, RoofColour, false, P.X, P.Y, 0.0f, Chance);
		};
		auto Plane = [&](const std::vector<FPoint>& Corners, const std::vector<double>& Heights)
		{
			// Una falda piana (3 o 4 vertici): la normale dal prodotto vettoriale, verso l'alto.
			const double Ax = Corners[1].X - Corners[0].X, Ay = Corners[1].Y - Corners[0].Y, Az = Heights[1] - Heights[0];
			const size_t Last = Corners.size() - 1;
			const double Bx = Corners[Last].X - Corners[0].X, By = Corners[Last].Y - Corners[0].Y, Bz = Heights[Last] - Heights[0];
			double Nx = Ay * Bz - Az * By, Ny = Az * Bx - Ax * Bz, Nz = Ax * By - Ay * Bx;   // (est, nord, alto)
			if (Nz < 0.0) { Nx = -Nx; Ny = -Ny; Nz = -Nz; }
			std::vector<uint32_t> Ids;
			for (size_t I = 0; I < Corners.size(); ++I) { Ids.push_back(RoofVertex(Corners[I], Heights[I], Nx, Ny, Nz)); }
			Mesh.Triangle(Ids[0], Ids[1], Ids[2], Ny, Nx, Nz);
			if (Ids.size() == 4) { Mesh.Triangle(Ids[0], Ids[2], Ids[3], Ny, Nx, Nz); }
		};
		auto Gable = [&](const FPoint& A, const FPoint& B, const FPoint& Apex)
		{
			// Il timpano: un triangolo di muro, verticale, verso fuori.
			const double Length = std::max(1e-6, Distance(A, B));
			const double OutEast = (B.Y - A.Y) / Length, OutNorth = -(B.X - A.X) / Length;
			const double Mid = 0.5 * Length;
			const uint32_t V0 = Mesh.Vertex(A.Y, A.X, Top + Drop(A.X, A.Y), OutNorth, OutEast, 0.0, WallColour, true, 0.0, WallTop, 0.0f, Chance);
			const uint32_t V1 = Mesh.Vertex(B.Y, B.X, Top + Drop(B.X, B.Y), OutNorth, OutEast, 0.0, WallColour, true, Length, WallTop, 0.0f, Chance);
			const uint32_t V2 = Mesh.Vertex(Apex.Y, Apex.X, Ridge + Drop(Apex.X, Apex.Y), OutNorth, OutEast, 0.0, WallColour, true, Mid, WallTop + RoofHeight, 0.0f, Chance);
			Mesh.Triangle(V0, V1, V2, OutNorth, OutEast, 0.0);
		};

		bool bRoofDone = false;
		if ((Roof == ERoofShape::Gabled || Roof == ERoofShape::Hipped) && Outer.size() == 4)
		{
			// Il colmo corre lungo i lati lunghi: 0-1 e 2-3, oppure 1-2 e 3-0.
			const bool bAlong01 = Distance(Outer[0], Outer[1]) + Distance(Outer[2], Outer[3])
			                   >= Distance(Outer[1], Outer[2]) + Distance(Outer[3], Outer[0]);
			const FPoint& P0 = Outer[bAlong01 ? 0 : 1];
			const FPoint& P1 = Outer[bAlong01 ? 1 : 2];
			const FPoint& P2 = Outer[bAlong01 ? 2 : 3];
			const FPoint& P3 = Outer[bAlong01 ? 3 : 0];
			// Le estremita' del colmo: a meta' dei lati corti (P1-P2 e P3-P0).
			FPoint R1{ 0.5 * (P1.X + P2.X), 0.5 * (P1.Y + P2.Y) };
			FPoint R3{ 0.5 * (P3.X + P0.X), 0.5 * (P3.Y + P0.Y) };
			if (Roof == ERoofShape::Hipped)
			{
				// Il padiglione: il colmo si accorcia di mezzo lato corto per parte (falde a 45° in pianta).
				const double Span = Distance(R1, R3);
				const double Pull = std::min(0.5 * 0.5 * (Distance(P1, P2) + Distance(P3, P0)), 0.45 * Span);
				const double Ux = (R1.X - R3.X) / std::max(1e-6, Span), Uy = (R1.Y - R3.Y) / std::max(1e-6, Span);
				R1 = FPoint{ R1.X - Ux * Pull, R1.Y - Uy * Pull };
				R3 = FPoint{ R3.X + Ux * Pull, R3.Y + Uy * Pull };
				Plane({ P0, P1, R1, R3 }, { Top, Top, Ridge, Ridge });
				Plane({ P2, P3, R3, R1 }, { Top, Top, Ridge, Ridge });
				Plane({ P1, P2, R1 }, { Top, Top, Ridge });
				Plane({ P3, P0, R3 }, { Top, Top, Ridge });
			}
			else
			{
				Plane({ P0, P1, R1, R3 }, { Top, Top, Ridge, Ridge });
				Plane({ P2, P3, R3, R1 }, { Top, Top, Ridge, Ridge });
				Gable(P1, P2, R1);
				Gable(P3, P0, R3);
			}
			bRoofDone = true;
		}
		else if (Roof == ERoofShape::Pyramidal || Roof == ERoofShape::Dome)
		{
			double Cx = 0.0, Cy = 0.0;
			for (const FPoint& Point : Outer) { Cx += Point.X; Cy += Point.Y; }
			const FPoint Centre{ Cx / static_cast<double>(Outer.size()), Cy / static_cast<double>(Outer.size()) };
			if (Roof == ERoofShape::Pyramidal)
			{
				for (size_t I = 0, N = Outer.size(); I < N; ++I)
				{
					Plane({ Outer[I], Outer[(I + 1) % N], Centre }, { Top, Top, Ridge });
				}
			}
			else
			{
				// La cupola: anelli sempre piu' stretti e alti, con le normali di una sfera schiacciata.
				constexpr int32_t Steps = 5;
				const double Radius = std::max(0.5, 0.5 * Extent);
				std::vector<std::vector<uint32_t>> Shells;
				for (int32_t Step = 0; Step < Steps; ++Step)
				{
					const double Theta = 0.5 * 3.14159265358979323846 * Step / Steps;
					const double Shrink = std::cos(Theta), Lift = std::sin(Theta);
					std::vector<uint32_t> Ring;
					for (const FPoint& Point : Outer)
					{
						const FPoint P{ Centre.X + (Point.X - Centre.X) * Shrink, Centre.Y + (Point.Y - Centre.Y) * Shrink };
						const double Dx = (Point.X - Centre.X) / Radius, Dy = (Point.Y - Centre.Y) / Radius;
						Ring.push_back(RoofVertex(P, Top + RoofHeight * Lift, Dx * Shrink, Dy * Shrink, Lift * Radius / std::max(0.5, RoofHeight)));
					}
					Shells.push_back(Ring);
				}
				const uint32_t Apex = RoofVertex(Centre, Ridge, 0.0, 0.0, 1.0);
				for (int32_t Step = 0; Step + 1 < Steps; ++Step)
				{
					for (size_t I = 0, N = Outer.size(); I < N; ++I)
					{
						const size_t J = (I + 1) % N;
						const FPoint& A = Outer[I];
						const FPoint& B = Outer[J];
						const double OutEast = (B.Y - A.Y), OutNorth = -(B.X - A.X);
						Mesh.Triangle(Shells[Step][I], Shells[Step][J], Shells[Step + 1][J], OutNorth, OutEast, 0.5);
						Mesh.Triangle(Shells[Step][I], Shells[Step + 1][J], Shells[Step + 1][I], OutNorth, OutEast, 0.5);
					}
				}
				for (size_t I = 0, N = Outer.size(); I < N; ++I)
				{
					Mesh.Triangle(Shells[Steps - 1][I], Shells[Steps - 1][(I + 1) % N], Apex, 0.0, 0.0, 1.0);
				}
			}
			bRoofDone = true;
		}
		if (!bRoofDone) { FlatFace(Top, true, RoofColour, false); }
		if (OutSloped) { *OutSloped = bSloped && bRoofDone; }
		return true;
	}

	namespace BuildingMeshDetail
	{
		/** Il baricentro (media dei vertici) del contorno, gradi. */
		inline void RingCentre(const Tiles::FBuildingRing& Ring, double& OutLon, double& OutLat)
		{
			OutLon = 0.0;
			OutLat = 0.0;
			if (Ring.Size() == 0) { return; }
			for (size_t I = 0; I < Ring.Size(); ++I) { OutLon += Ring.Lon[I]; OutLat += Ring.Lat[I]; }
			OutLon /= static_cast<double>(Ring.Size());
			OutLat /= static_cast<double>(Ring.Size());
		}

		/** Il punto sta nell'anello? Raggio verso est, pari/dispari (in gradi: per una sagoma va bene). */
		inline bool InsideRing(const Tiles::FBuildingRing& Ring, double Lon, double Lat)
		{
			if (Ring.Size() < 3) { return false; }
			bool bInside = false;
			for (size_t I = 0, J = Ring.Size() - 1; I < Ring.Size(); J = I++)
			{
				if ((Ring.Lat[I] > Lat) != (Ring.Lat[J] > Lat)
					&& Lon < Ring.Lon[I] + (Lat - Ring.Lat[I]) * (Ring.Lon[J] - Ring.Lon[I]) / (Ring.Lat[J] - Ring.Lat[I]))
				{
					bInside = !bInside;
				}
			}
			return bInside;
		}
	}

	/**
	 * Tutti gli edifici di una tile in una mesh, nel frame della cella (il
	 * centro della tile). Quelli in Landmarks (id OSM impacchettati, o
	 * Wikidata) non si estrudono: tornano in OutSpots, per i modelli.
	 *
	 * I CONTORNI CON LE PARTI (BuildingOutline) non si estrudono mai: si
	 * estrudono le loro parti. Servono ai monumenti: il tag wikidata di una
	 * chiesa fatta a parti sta sul contorno. Se il contorno e' un monumento,
	 * il modello prende il suo posto e le parti che gli stanno dentro (la
	 * pipeline le mette nella stessa tile) spariscono.
	 */
	inline void BuildBuildingCell(const FBuildingTile& Tile, const Cells::FCellFrame& Frame, const Cells::FGroundSurfaces& Ground,
	                              const FBuildingDetail& Detail, const std::vector<uint64_t>& LandmarkOsmIds,
	                              const std::vector<uint32_t>& LandmarkWikidata, FBuildingMesh& Out,
	                              std::vector<FLandmarkSpot>& OutSpots, FBuildingCellStats& Stats)
	{
		using namespace BuildingMeshDetail;
		Out = FBuildingMesh();
		OutSpots.clear();
		Stats = FBuildingCellStats();
		auto IsLandmark = [&](const FBuilding& Building)
		{
			return std::find(LandmarkOsmIds.begin(), LandmarkOsmIds.end(), Building.OsmId) != LandmarkOsmIds.end()
				|| (Building.Wikidata != 0
				    && std::find(LandmarkWikidata.begin(), LandmarkWikidata.end(), Building.Wikidata) != LandmarkWikidata.end());
		};

		// I contorni con le parti che sono diventati monumenti: le parti dentro si tolgono.
		std::vector<const Tiles::FBuildingRing*> Replaced;
		for (const FBuilding& Building : Tile.Buildings)
		{
			if ((Building.Flags & Tiles::BuildingOutline) && IsLandmark(Building)) { Replaced.push_back(&Building.Rings[0]); }
		}

		for (const FBuilding& Building : Tile.Buildings)
		{
			if (IsLandmark(Building))
			{
				FLandmarkSpot Spot;
				FBuildingDetail Full = Detail;
				Full.MinAreaM2 = 0.0;
				if (BuildBuilding(Building, Frame, Ground, Full, Out, nullptr, &Spot))
				{
					OutSpots.push_back(Spot);
					++Stats.Landmarks;
				}
				continue;
			}
			if (Building.Flags & Tiles::BuildingOutline) { continue; }   // si disegnano le sue parti
			if ((Building.Flags & Tiles::BuildingPart) && !Replaced.empty())
			{
				double Lon = 0.0, Lat = 0.0;
				RingCentre(Building.Rings[0], Lon, Lat);
				bool bHidden = false;
				for (const Tiles::FBuildingRing* Outline : Replaced) { bHidden = bHidden || InsideRing(*Outline, Lon, Lat); }
				if (bHidden)
				{
					++Stats.HiddenParts;
					continue;
				}
			}
			bool bSloped = false;
			if (BuildBuilding(Building, Frame, Ground, Detail, Out, &bSloped))
			{
				++Stats.Built;
				Stats.SlopedRoofs += bSloped ? 1 : 0;
			}
			else
			{
				++Stats.Skipped;
			}
		}
	}
}
