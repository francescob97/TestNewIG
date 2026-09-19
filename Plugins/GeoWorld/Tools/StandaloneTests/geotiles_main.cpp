// =============================================================================
//  Test dello strato puro di GeoTiles (Fase 3), senza Unreal.
//
//  Verifica tre cose che non si possono verificare a parole:
//   1. lo schema di tiling C++ concorda con quello Python, confrontandosi con i
//      vettori generati dalla pipeline;
//   2. il lettore del formato .ght legge DAVVERO le tile prodotte dalla
//      pipeline, comprese le giunzioni condivise;
//   3. la cache LRU sfratta quello che deve e non sfratta quello che e' pinnato.
//
//  Uso:  geotiles_tests [percorso_di_un_dataset]
//  Senza argomento, i test che richiedono un dataset vengono saltati.
// =============================================================================
#include "Tiles/TileCache.h"
#include "Tiles/TileFormat.h"
#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace GeoWorld::Tiles;

static int GPassed = 0, GFailed = 0, GSkipped = 0;

static void Check(bool bCondition, const std::string& What, const std::string& Detail = {})
{
	(bCondition ? GPassed : GFailed)++;
	std::printf("  [%s] %s%s%s\n", bCondition ? " ok " : "FAIL", What.c_str(),
		Detail.empty() ? "" : "  -> ", Detail.c_str());
}

static void Section(const char* Title) { std::printf("\n== %s ==\n", Title); }

static std::vector<uint8_t> ReadFile(const std::string& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	if (!Stream) { return {}; }
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(Stream)),
	                            std::istreambuf_iterator<char>());
}

// ===========================================================================
//  1. Lo schema C++ concorda con quello Python
// ===========================================================================
static void TestAgainstPythonVectors(const std::string& VectorsPath)
{
	Section("1. Schema di tiling: C++ contro i vettori generati da Python");

	std::ifstream Stream(VectorsPath);
	if (!Stream)
	{
		std::printf("  (saltato: %s non trovato)\n", VectorsPath.c_str());
		++GSkipped;
		return;
	}

	int Levels = 0, Bounds = 0, Lookups = 0;
	double WorstBoundsError = 0.0, WorstSpacingError = 0.0;
	bool bSchemaOk = false, bAllLookupsOk = true;

	std::string Kind;
	while (Stream >> Kind)
	{
		if (Kind.empty() || Kind[0] == '#')
		{
			Stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			continue;
		}

		if (Kind == "SCHEMA")
		{
			int Posts, Cells, Level0X, Level0Y;
			Stream >> Posts >> Cells >> Level0X >> Level0Y;
			bSchemaOk = (Posts == TilePosts && Cells == TileCells
			          && Level0X == static_cast<int>(TilesX(0))
			          && Level0Y == static_cast<int>(TilesY(0)));
		}
		else if (Kind == "LEVEL")
		{
			int Level, ExpectedX, ExpectedY;
			double ExpectedSpan, ExpectedSpacing;
			Stream >> Level >> ExpectedX >> ExpectedY >> ExpectedSpan >> ExpectedSpacing;
			if (TilesX(Level) != static_cast<uint32_t>(ExpectedX)
			 || TilesY(Level) != static_cast<uint32_t>(ExpectedY)) { bAllLookupsOk = false; }
			WorstSpacingError = std::max(WorstSpacingError,
				std::abs(PostSpacingDeg(Level) - ExpectedSpacing));
			WorstSpacingError = std::max(WorstSpacingError,
				std::abs(TileSpanDeg(Level) - ExpectedSpan));
			++Levels;
		}
		else if (Kind == "BOUNDS")
		{
			int Level; uint32_t X, Y;
			double West, South, East, North;
			Stream >> Level >> X >> Y >> West >> South >> East >> North;
			const FTileBounds Actual = GetTileBounds(Level, X, Y);
			WorstBoundsError = std::max({ WorstBoundsError,
				std::abs(Actual.West - West), std::abs(Actual.South - South),
				std::abs(Actual.East - East), std::abs(Actual.North - North) });
			++Bounds;
		}
		else if (Kind == "LOOKUP")
		{
			int Level; double Lon, Lat; uint32_t ExpectedX, ExpectedY;
			Stream >> Level >> Lon >> Lat >> ExpectedX >> ExpectedY;
			uint32_t X = 0, Y = 0;
			TileForLonLat(Level, Lon, Lat, X, Y);
			if (X != ExpectedX || Y != ExpectedY) { bAllLookupsOk = false; }
			++Lookups;
		}
		else
		{
			Stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		}
	}

	Check(bSchemaOk, "costanti dello schema identiche");
	Check(Levels >= 15, "livelli confrontati", std::to_string(Levels));
	Check(WorstSpacingError < 1e-15, "span e passo dei post identici",
		"errore max " + std::to_string(WorstSpacingError));
	Check(Bounds >= 6 && WorstBoundsError < 1e-12, "estremi delle tile identici",
		std::to_string(Bounds) + " tile, errore max " + std::to_string(WorstBoundsError));
	Check(Lookups >= 6 && bAllLookupsOk, "ricerca tile da lon/lat identica",
		std::to_string(Lookups) + " punti");
}

// ===========================================================================
//  2. Proprieta' dello schema che devono valere sempre
// ===========================================================================
static void TestSchemeInvariants()
{
	Section("2. Invarianti dello schema");

	bool bEdgesShared = true, bParentChildOk = true;
	double WorstPostError = 0.0;

	for (uint32_t Level = 0; Level < 15; ++Level)
	{
		for (uint32_t X : { 0u, 1u, 7u })
		{
			if (X + 1 >= TilesX(Level)) { continue; }
			if (GetTileBounds(Level, X, 0).East != GetTileBounds(Level, X + 1, 0).West)
			{
				bEdgesShared = false;
			}
		}

		// Il post 128 di una tile e' il post 0 della successiva, alla stessa
		// identica longitudine. E' l'overlap di 1 pixel visto dal runtime.
		if (TilesX(Level) >= 2)
		{
			double LonA, LatA, LonB, LatB;
			GetPostLonLat(Level, 0, 0, TileCells, 0, LonA, LatA);
			GetPostLonLat(Level, 1, 0, 0, 0, LonB, LatB);
			WorstPostError = std::max(WorstPostError, std::abs(LonA - LonB));
		}

		// Il padre copre esattamente i suoi quattro figli.
		if (Level + 1 < 15)
		{
			const FTileKey Parent{ Level, 3, 1 };
			if (Parent.Level > 0 && Parent.GetChild(0).GetParent() != Parent)
			{
				bParentChildOk = false;
			}
			const FTileBounds P = GetTileBounds(Parent.Level, Parent.X, Parent.Y);
			const FTileKey Child = Parent.GetChild(0);
			const FTileBounds C = GetTileBounds(Child.Level, Child.X, Child.Y);
			if (std::abs(P.West - C.West) > 1e-12 || std::abs(P.North - C.North) > 1e-12)
			{
				bParentChildOk = false;
			}
		}
	}

	Check(bEdgesShared, "il bordo est di una tile e' il bordo ovest della successiva");
	Check(WorstPostError < 1e-12, "il post 128 coincide con il post 0 della tile a est",
		"errore max " + std::to_string(WorstPostError));
	Check(bParentChildOk, "padre e figlio nord-ovest condividono l'angolo");

	// Le chiavi devono distribuirsi bene: chiavi adiacenti non devono collidere.
	std::set<uint64_t> Hashes;
	for (uint32_t Y = 0; Y < 40; ++Y)
	{
		for (uint32_t X = 0; X < 40; ++X) { Hashes.insert(FTileKey{ 14, X, Y }.ToHash()); }
	}
	Check(Hashes.size() == 1600, "nessuna collisione su 1600 chiavi adiacenti",
		std::to_string(Hashes.size()) + " hash distinti");
}

// ===========================================================================
//  3. Cache LRU
// ===========================================================================
static FTileCache::FTilePtr MakeTile(const FTileKey& Key)
{
	auto Tile = std::make_shared<FHeightTile>();
	Tile->Key = Key;
	Tile->Heights.assign(static_cast<size_t>(TilePosts) * TilePosts, 100.0f);
	return Tile;
}

static void TestCache()
{
	Section("3. Cache LRU");

	const size_t TileSize = MakeTile(FTileKey{ 0, 0, 0 })->GetByteSize();

	// Budget per tre tile esatte.
	FTileCache Cache(TileSize * 3);
	for (uint32_t X = 0; X < 3; ++X) { Cache.Insert(FTileKey{ 10, X, 0 }, MakeTile(FTileKey{ 10, X, 0 })); }
	Check(Cache.GetStats().ResidentTiles == 3, "tre tile entrano nel budget");

	// La quarta deve sfrattare la piu' vecchia, che e' la 0.
	Cache.Insert(FTileKey{ 10, 3, 0 }, MakeTile(FTileKey{ 10, 3, 0 }));
	Check(Cache.Peek(FTileKey{ 10, 0, 0 }) == nullptr, "la meno usata di recente e' stata sfrattata");
	Check(Cache.Peek(FTileKey{ 10, 3, 0 }) != nullptr, "la nuova e' presente");
	Check(Cache.GetStats().UsedBytes <= Cache.GetStats().BudgetBytes, "il budget e' rispettato",
		std::to_string(Cache.GetStats().UsedBytes) + " / " + std::to_string(Cache.GetStats().BudgetBytes));

	// Find aggiorna l'ordine: dopo averla toccata, la 1 non deve piu' essere la vittima.
	Cache.Find(FTileKey{ 10, 1, 0 });
	Cache.Insert(FTileKey{ 10, 4, 0 }, MakeTile(FTileKey{ 10, 4, 0 }));
	Check(Cache.Peek(FTileKey{ 10, 1, 0 }) != nullptr, "Find protegge dalla prossima evizione");
	Check(Cache.Peek(FTileKey{ 10, 2, 0 }) == nullptr, "la vittima e' quella non toccata");

	// Pin: una tile pinnata non si sfratta nemmeno sotto pressione.
	FTileCache Pinned(TileSize * 2);
	Pinned.Insert(FTileKey{ 0, 0, 0 }, MakeTile(FTileKey{ 0, 0, 0 }));
	Pinned.SetPinned(FTileKey{ 0, 0, 0 }, true);
	for (uint32_t X = 1; X < 8; ++X) { Pinned.Insert(FTileKey{ 10, X, 0 }, MakeTile(FTileKey{ 10, X, 0 })); }
	Check(Pinned.Peek(FTileKey{ 0, 0, 0 }) != nullptr,
		"una tile pinnata sopravvive a sette inserimenti con budget per due");

	// Un riferimento gia' preso resta valido dopo lo sfratto.
	FTileCache Small(TileSize);
	Small.Insert(FTileKey{ 5, 5, 5 }, MakeTile(FTileKey{ 5, 5, 5 }));
	auto Held = Small.Find(FTileKey{ 5, 5, 5 });
	Small.Insert(FTileKey{ 5, 6, 5 }, MakeTile(FTileKey{ 5, 6, 5 }));
	Check(Small.Peek(FTileKey{ 5, 5, 5 }) == nullptr && Held && Held->IsValid(),
		"un riferimento gia' preso resta valido dopo lo sfratto");

	// Statistiche.
	FTileCache Counting(TileSize * 4);
	Counting.Insert(FTileKey{ 1, 1, 1 }, MakeTile(FTileKey{ 1, 1, 1 }));
	Counting.Find(FTileKey{ 1, 1, 1 });
	Counting.Find(FTileKey{ 9, 9, 9 });
	const FCacheStats Stats = Counting.GetStats();
	Check(Stats.Hits == 1 && Stats.Misses == 1 && std::abs(Stats.GetHitRate() - 0.5) < 1e-9,
		"hit e miss contati correttamente");
}

// ===========================================================================
//  4. Lettura di un dataset VERO prodotto dalla pipeline
// ===========================================================================
static void TestRealDataset(const std::string& Root)
{
	Section("4. Lettura di un dataset reale");

	if (Root.empty())
	{
		std::printf("  (saltato: nessun dataset indicato sulla riga di comando)\n");
		++GSkipped;
		return;
	}

	// Si scandagliano i livelli finche' se ne trova uno con un indice.
	std::map<uint32_t, std::vector<FTileIndexEntry>> Indices;
	for (uint32_t Level = 0; Level < 22; ++Level)
	{
		const std::vector<uint8_t> Blob = ReadFile(Root + "/" + std::to_string(Level) + "/index.bin");
		if (Blob.empty()) { continue; }

		std::vector<FTileIndexEntry> Entries;
		std::string Error;
		if (!DecodeLevelIndex(Blob.data(), Blob.size(), Level, Entries, Error))
		{
			Check(false, "indice del livello " + std::to_string(Level), Error);
			continue;
		}
		Indices[Level] = std::move(Entries);
	}

	if (Indices.empty())
	{
		Check(false, "nessun indice trovato", "il percorso e' un dataset GeoWorld?");
		return;
	}

	size_t TotalTiles = 0;
	for (const auto& [Level, Entries] : Indices) { TotalTiles += Entries.size(); }
	Check(true, "indici letti", std::to_string(Indices.size()) + " livelli, "
		+ std::to_string(TotalTiles) + " tile");

	// L'indice deve essere ordinato per (y, x): il runtime ci fa ricerca binaria.
	bool bSorted = true;
	for (const auto& [Level, Entries] : Indices)
	{
		for (size_t Index = 1; Index < Entries.size(); ++Index)
		{
			const auto& A = Entries[Index - 1];
			const auto& B = Entries[Index];
			if (A.Y > B.Y || (A.Y == B.Y && A.X >= B.X)) { bSorted = false; }
		}
	}
	Check(bSorted, "tutti gli indici sono ordinati per (y, x)");

	// Si carica il livello piu' fine e si controllano tile e giunzioni.
	const uint32_t FinestLevel = Indices.rbegin()->first;
	const std::vector<FTileIndexEntry>& Finest = Indices.rbegin()->second;

	std::set<std::pair<uint32_t, uint32_t>> Present;
	for (const FTileIndexEntry& Entry : Finest) { Present.insert({ Entry.X, Entry.Y }); }

	FTileCache Cache(64ull * 1024 * 1024);
	int Loaded = 0, Seams = 0, HeaderMismatches = 0, MinMaxMismatches = 0;
	std::string FirstError;

	const size_t Limit = std::min<size_t>(Finest.size(), 400);
	for (size_t Index = 0; Index < Limit; ++Index)
	{
		const FTileIndexEntry& Entry = Finest[Index];
		const FTileKey Key{ FinestLevel, Entry.X, Entry.Y };

		const std::vector<uint8_t> Blob = ReadFile(Root + "/" + GetTileRelativePath(Key));
		if (Blob.empty())
		{
			if (FirstError.empty()) { FirstError = "tile citata dall'indice ma assente: " + GetTileRelativePath(Key); }
			++HeaderMismatches;
			continue;
		}

		auto Tile = std::make_shared<FHeightTile>();
		std::string Error;
		if (!DecodeTile(Blob.data(), Blob.size(), Key, *Tile, Error))
		{
			if (FirstError.empty()) { FirstError = GetTileRelativePath(Key) + ": " + Error; }
			++HeaderMismatches;
			continue;
		}

		++Loaded;
		if (std::abs(Tile->MinHeight - Entry.MinHeight) > 1e-3
		 || std::abs(Tile->MaxHeight - Entry.MaxHeight) > 1e-3) { ++MinMaxMismatches; }

		Cache.Insert(Key, Tile);

		// La giunzione con la tile a est: e' la proprieta' da cui dipende
		// l'assenza di crepe in Fase 5, verificata qui dal codice che legge
		// davvero i file, non dallo script che li ha scritti.
		if (Present.count({ Entry.X + 1, Entry.Y }))
		{
			const FTileKey RightKey{ FinestLevel, Entry.X + 1, Entry.Y };
			const std::vector<uint8_t> RightBlob = ReadFile(Root + "/" + GetTileRelativePath(RightKey));
			FHeightTile Right;
			std::string RightError;
			if (DecodeTile(RightBlob.data(), RightBlob.size(), RightKey, Right, RightError))
			{
				bool bMatch = true;
				for (int32_t J = 0; J < TilePosts; ++J)
				{
					if (Tile->GetHeight(TileCells, J) != Right.GetHeight(0, J)) { bMatch = false; break; }
				}
				if (!bMatch && FirstError.empty())
				{
					FirstError = "giunzione rotta fra " + GetTileRelativePath(Key)
					           + " e " + GetTileRelativePath(RightKey);
				}
				if (bMatch) { ++Seams; }
			}
		}
	}

	Check(HeaderMismatches == 0, "tutte le tile campionate si decodificano",
		std::to_string(Loaded) + " lette" + (FirstError.empty() ? "" : ", primo errore: " + FirstError));
	Check(MinMaxMismatches == 0, "min/max dell'header coincidono con l'indice");
	Check(Seams > 0, "giunzioni verificate leggendo i file", std::to_string(Seams) + " coppie");

	const FCacheStats Stats = Cache.GetStats();
	std::printf("  cache: %zu tile residenti, %.1f MB su %.0f MB di budget\n",
		Stats.ResidentTiles, Stats.GetUsedMegabytes(),
		static_cast<double>(Stats.BudgetBytes) / (1024.0 * 1024.0));
}

// ===========================================================================
int main(int ArgCount, char** Arguments)
{
	std::printf("=====================================================\n");
	std::printf(" GeoTiles -- test dello strato puro (senza Unreal)\n");
	std::printf("=====================================================\n");

	const std::string VectorsPath = (ArgCount > 2) ? Arguments[2]
		: "Plugins/GeoWorld/Source/GeoTiles/TestData/tiling_vectors.txt";
	const std::string DatasetRoot = (ArgCount > 1) ? Arguments[1] : "";

	TestAgainstPythonVectors(VectorsPath);
	TestSchemeInvariants();
	TestCache();
	TestRealDataset(DatasetRoot);

	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti, %d saltati\n", GPassed, GFailed, GSkipped);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}
