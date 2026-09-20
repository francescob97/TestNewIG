// =============================================================================
//  Test del drappeggio delle ortofoto (Fase 6), senza Unreal.
//
//  Gli esempi numerici sono gli stessi dei test Python (tests/test_imageformat.py,
//  classe TestDrape) e gli stessi del documento di design. Se le tre cose non
//  concordano, una delle tre e' sbagliata e si vede subito quale.
// =============================================================================
#include "Imagery/ImageryMapping.h"
#include "Tiles/ImageTileFormat.h"
#include "Tiles/TileCache.h"
#include "Tiles/TilingScheme.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace GeoWorld;
using namespace GeoWorld::Imagery;
using Tiles::FImageTile;
using Tiles::FTileKey;

static int GPassed = 0, GFailed = 0;

static void Check(bool bCondition, const std::string& What, const std::string& Detail = {})
{
	(bCondition ? GPassed : GFailed)++;
	std::printf("  [%s] %s%s%s\n", bCondition ? " ok " : "FAIL", What.c_str(),
		Detail.empty() ? "" : "  -> ", Detail.c_str());
}

static void Section(const char* Title) { std::printf("\n== %s ==\n", Title); }

static std::string Fmt(const char* Format, double Value)
{
	char Buffer[160];
	std::snprintf(Buffer, sizeof(Buffer), Format, Value);
	return Buffer;
}

// --- Disponibilita' finta: i test decidono cosa esiste e cosa e' in memoria ---
class FFakeAvailability : public IImageAvailability
{
public:
	std::set<uint64_t> Existing;
	std::set<uint64_t> Resident;

	static uint64_t Pack(const FTileKey& Key)
	{
		return (static_cast<uint64_t>(Key.Level) << 58)
		     ^ (static_cast<uint64_t>(Key.Y) << 29)
		     ^ static_cast<uint64_t>(Key.X);
	}

	void Add(const FTileKey& Key, bool bResident)
	{
		Existing.insert(Pack(Key));
		if (bResident) { Resident.insert(Pack(Key)); }
	}

	bool ImageExists(const FTileKey& Key) const override
	{
		return Existing.count(Pack(Key)) > 0;
	}
	bool ImageIsResident(const FTileKey& Key) const override
	{
		return Resident.count(Pack(Key)) > 0;
	}
};

// --- Costruttore di un file .gim finto, per provare il lettore ---------------
static std::vector<uint8_t> MakeImageTileBytes(uint32_t Level, uint32_t X, uint32_t Y,
                                               uint8_t Coverage, size_t PayloadSize,
                                               uint32_t Magic = Tiles::ImageTileMagic,
                                               uint16_t Version = Tiles::ImageTileVersion,
                                               uint16_t Side = static_cast<uint16_t>(Tiles::TilePixels))
{
	std::vector<uint8_t> Bytes(Tiles::ImageTileHeaderBytes + PayloadSize, 0);

	auto PutU16 = [&Bytes](size_t At, uint16_t Value)
	{
		Bytes[At + 0] = static_cast<uint8_t>(Value & 0xFF);
		Bytes[At + 1] = static_cast<uint8_t>((Value >> 8) & 0xFF);
	};
	auto PutU32 = [&Bytes](size_t At, uint32_t Value)
	{
		for (int i = 0; i < 4; ++i) { Bytes[At + i] = static_cast<uint8_t>((Value >> (8 * i)) & 0xFF); }
	};

	PutU32(0, Magic);
	PutU16(4, Version);
	PutU16(6, Coverage < 100 ? Tiles::ImageFlagHasFilledPixels : 0);
	PutU32(8, Level);
	PutU32(12, X);
	PutU32(16, Y);
	PutU16(20, Side);
	PutU16(22, Side);
	Bytes[24] = static_cast<uint8_t>(Tiles::EImagePayload::Jpeg);
	Bytes[25] = Coverage;
	PutU32(28, static_cast<uint32_t>(PayloadSize));

	for (size_t i = 0; i < PayloadSize; ++i)
	{
		Bytes[Tiles::ImageTileHeaderBytes + i] = static_cast<uint8_t>(i & 0xFF);
	}
	return Bytes;
}

int main()
{
	std::printf("=====================================================\n");
	std::printf(" Ortofoto -- test dello strato puro (senza Unreal)\n");
	std::printf("=====================================================\n");

	// ------------------------------------------------------------------
	Section("1. Geometria: immagini contro quote");

	Check(Tiles::TilePixels == 256, "la tile di immagine e' 256x256");
	Check(Tiles::TilePixels == 2 * Tiles::TileCells,
		"i pixel sono il doppio delle celle di terreno",
		std::to_string(Tiles::TilePixels) + " contro " + std::to_string(Tiles::TileCells));

	{
		// Conseguenza diretta: l'immagine di livello L ha la risoluzione al
		// suolo del terreno di livello L+1.
		bool bAllMatch = true;
		for (uint32_t Level = 4; Level <= 17; ++Level)
		{
			const double ImagePixel = Tiles::TileSpanDeg(Level) / Tiles::TilePixels;
			const double TerrainStep = Tiles::PostSpacingDeg(Level + 1);
			if (std::fabs(ImagePixel - TerrainStep) > 1e-15) { bAllMatch = false; }
		}
		Check(bAllMatch, "immagine di livello L = risoluzione del terreno di livello L+1");
	}

	{
		const double PixelDeg = Tiles::TileSpanDeg(13) / Tiles::TilePixels;
		const double Metres = PixelDeg * 111132.0;
		Check(std::fabs(Metres - 9.54) < 0.05,
			"il livello 13 e' il nativo di Sentinel-2 (10 m)", Fmt("%.2f m", Metres));
	}

	// ------------------------------------------------------------------
	Section("2. Il ritaglio, con gli esempi del design");

	{
		const FTileKey Terrain{ 12, 4333, 1014 };
		const FDrapeTransform Same = MakeDrapeTransform(Terrain, 12);
		Check(Same.OffsetU == 0.0f && Same.OffsetV == 0.0f && Same.Scale == 1.0f,
			"stesso livello: offset zero, scala uno (nessun calcolo)");
		Check(Same.IsIdentity(), "e si riconosce come identita'");
		Check(Same.ImageKey == Terrain, "e usa la tile con la stessa chiave");

		const FDrapeTransform Two = MakeDrapeTransform(Terrain, 10);
		Check(Two.ImageKey.Level == 10 && Two.ImageKey.X == 1083 && Two.ImageKey.Y == 253,
			"antenato di due livelli", "(10, 1083, 253)");
		Check(Two.Scale == 0.25f, "scala 1/4");
		Check(Two.OffsetU == 0.25f, "offset U = 0.25", Fmt("%.4f", Two.OffsetU));
		Check(Two.OffsetV == 0.5f, "offset V = 0.50", Fmt("%.4f", Two.OffsetV));
	}

	{
		// I quattro figli devono coprire il padre senza buchi ne' sovrapposizioni.
		std::set<std::pair<float, float>> Corners;
		bool bScaleOk = true;
		for (uint32_t Dx = 0; Dx < 2; ++Dx)
		{
			for (uint32_t Dy = 0; Dy < 2; ++Dy)
			{
				const FDrapeTransform D = MakeDrapeTransform(FTileKey{ 9, 100 + Dx, 50 + Dy }, 8);
				if (D.Scale != 0.5f) { bScaleOk = false; }
				Corners.insert({ D.OffsetU, D.OffsetV });
			}
		}
		Check(bScaleOk && Corners.size() == 4,
			"i quattro figli tassellano il padre esattamente",
			std::to_string(Corners.size()) + " quadranti distinti");
	}

	{
		// Gli offset devono essere rappresentabili esattamente in float: sono
		// multipli di potenze di due, quindi lo sono.
		bool bExact = true;
		for (uint32_t Depth = 0; Depth <= 10; ++Depth)
		{
			const FDrapeTransform D = MakeDrapeTransform(FTileKey{ 10 + Depth, 12345, 6789 }, 10);
			const float Steps = D.OffsetU / D.Scale;
			if (Steps != std::floor(Steps)) { bExact = false; }
			if (D.OffsetU + D.Scale > 1.0f + 1e-7f) { bExact = false; }
			if (D.OffsetV + D.Scale > 1.0f + 1e-7f) { bExact = false; }
		}
		Check(bExact, "offset esatti in float e ritaglio dentro il quadrato unitario");
	}

	{
		// La prova che lega l'aritmetica alla geografia: il ritaglio calcolato
		// con gli interi deve coincidere con quello misurato sui rettangoli.
		const FTileKey Terrain{ 13, 4333, 1014 };
		const FDrapeTransform D = MakeDrapeTransform(Terrain, 9);
		const Tiles::FTileBounds TileBox = Tiles::GetTileBounds(13, 4333, 1014);
		const Tiles::FTileBounds ImageBox =
			Tiles::GetTileBounds(D.ImageKey.Level, D.ImageKey.X, D.ImageKey.Y);

		const double Width = ImageBox.East - ImageBox.West;
		const double Height = ImageBox.North - ImageBox.South;
		const double GeoU = (TileBox.West - ImageBox.West) / Width;
		const double GeoV = (ImageBox.North - TileBox.North) / Height;
		const double GeoScale = (TileBox.East - TileBox.West) / Width;

		Check(std::fabs(GeoU - D.OffsetU) < 1e-9, "offset U coincide con la geografia",
			Fmt("scarto %.2e", std::fabs(GeoU - D.OffsetU)));
		Check(std::fabs(GeoV - D.OffsetV) < 1e-9, "offset V coincide con la geografia",
			Fmt("scarto %.2e", std::fabs(GeoV - D.OffsetV)));
		Check(std::fabs(GeoScale - D.Scale) < 1e-9, "scala coincide con la geografia",
			Fmt("scarto %.2e", std::fabs(GeoScale - D.Scale)));
	}

	{
		// Prova geometrica generale: l'antenato contiene sempre la tile.
		bool bContained = true;
		const FTileKey Terrain{ 14, 8901, 3456 };
		for (uint32_t Level = 0; Level <= 14; ++Level)
		{
			const FTileKey A = AncestorOf(Terrain, Level);
			const Tiles::FTileBounds Small = Tiles::GetTileBounds(14, 8901, 3456);
			const Tiles::FTileBounds Big = Tiles::GetTileBounds(A.Level, A.X, A.Y);
			if (Big.West > Small.West + 1e-9 || Small.East > Big.East + 1e-9
			 || Big.South > Small.South + 1e-9 || Small.North > Big.North + 1e-9)
			{
				bContained = false;
			}
		}
		Check(bContained, "ogni antenato contiene geograficamente la tile, a ogni livello");
	}

	// ------------------------------------------------------------------
	Section("3. Scelta dell'immagine: non si aspetta mai");

	{
		FFakeAvailability Available;
		const FTileKey Terrain{ 12, 4333, 1014 };

		// Solo l'antenato di livello 9 e' in memoria; quello giusto esiste ma
		// non e' ancora arrivato.
		Available.Add(AncestorOf(Terrain, 12), /*bResident=*/false);
		Available.Add(AncestorOf(Terrain, 9), /*bResident=*/true);

		FDrapeTransform Drape;
		FTileKey Request;
		bool bHasRequest = false;
		const bool bFound = ChooseDrape(Terrain, 0, 13, Available, Drape, Request, bHasRequest);

		Check(bFound, "trova comunque qualcosa da disegnare");
		Check(Drape.ImageKey.Level == 9, "ripiega sull'antenato residente",
			"livello " + std::to_string(Drape.ImageKey.Level));
		Check(bHasRequest && Request.Level == 12,
			"e intanto CHIEDE quella giusta, altrimenti resterebbe grossolana per sempre");
	}

	{
		FFakeAvailability Available;
		const FTileKey Terrain{ 12, 4333, 1014 };
		Available.Add(AncestorOf(Terrain, 12), /*bResident=*/true);

		FDrapeTransform Drape;
		FTileKey Request;
		bool bHasRequest = true;
		ChooseDrape(Terrain, 0, 13, Available, Drape, Request, bHasRequest);

		Check(Drape.IsIdentity(), "con la tile giusta in memoria il ritaglio e' l'identita'");
		Check(!bHasRequest, "e non chiede niente");
	}

	{
		// Piramide di immagini piu' bassa del terreno: e' il caso normale, non
		// un'eccezione. Il livello massimo deve limitare la richiesta.
		FFakeAvailability Available;
		const FTileKey Terrain{ 14, 8901, 3456 };
		Available.Add(AncestorOf(Terrain, 11), /*bResident=*/false);

		FDrapeTransform Drape;
		FTileKey Request;
		bool bHasRequest = false;
		ChooseDrape(Terrain, 0, 11, Available, Drape, Request, bHasRequest);

		Check(bHasRequest && Request.Level == 11,
			"con piramide piu' bassa chiede il suo livello massimo, non quello del terreno");
	}

	{
		FFakeAvailability Empty;
		FDrapeTransform Drape;
		FTileKey Request;
		bool bHasRequest = false;
		const bool bFound = ChooseDrape(FTileKey{ 12, 10, 10 }, 0, 13, Empty,
			Drape, Request, bHasRequest);
		Check(!bFound, "senza nessuna immagine dice di no invece di inventare");
	}

	{
		// Regressione: il ciclo scende fino a ImageryMinLevel compreso. Con
		// livelli senza segno un "Level >= Min" con Min = 0 non termina mai.
		FFakeAvailability Available;
		const FTileKey Terrain{ 6, 40, 20 };
		Available.Add(FTileKey{ 0, 0, 0 }, /*bResident=*/true);

		FDrapeTransform Drape;
		FTileKey Request;
		bool bHasRequest = false;
		const bool bFound = ChooseDrape(Terrain, 0, 13, Available, Drape, Request, bHasRequest);

		Check(bFound && Drape.ImageKey.Level == 0,
			"arriva fino al livello 0 senza andare in overflow");
		Check(Drape.Scale == 1.0f / 64.0f, "e il ritaglio di sei livelli e' 1/64",
			Fmt("%.6f", Drape.Scale));
	}

	// ------------------------------------------------------------------
	Section("4. Lettura del file .gim");

	{
		const std::vector<uint8_t> Bytes = MakeImageTileBytes(13, 4332, 1012, 100, 1234);
		FImageTile Tile;
		const auto Result = Tiles::ParseImageTile(Bytes.data(), Bytes.size(), Tile);

		Check(Result == Tiles::EImageTileParseResult::Ok, "legge una tile valida",
			Tiles::DescribeParseResult(Result));
		Check(Tile.Key.Level == 13 && Tile.Key.X == 4332 && Tile.Key.Y == 1012, "chiave corretta");
		Check(Tile.CoveragePercent == 100 && !Tile.HasFilledPixels(),
			"copertura piena, nessun pixel di riempimento");
		Check(Tile.CompressedBytes.size() == 1234, "payload della lunghezza dichiarata");
		Check(Tile.IsValid(), "la tile si dichiara valida");
	}

	{
		const std::vector<uint8_t> Bytes = MakeImageTileBytes(10, 1, 1, 62, 100);
		FImageTile Tile;
		Tiles::ParseImageTile(Bytes.data(), Bytes.size(), Tile);
		Check(Tile.CoveragePercent == 62 && Tile.HasFilledPixels(),
			"copertura parziale alza il flag dei pixel di riempimento");
	}

	{
		// Un file di QUOTE dato al lettore di immagini: deve fallire subito.
		std::vector<uint8_t> Bytes = MakeImageTileBytes(5, 1, 1, 100, 64);
		Bytes[0] = 'G'; Bytes[1] = 'W'; Bytes[2] = 'H'; Bytes[3] = 'T';
		FImageTile Tile;
		const auto Result = Tiles::ParseImageTile(Bytes.data(), Bytes.size(), Tile);
		Check(Result == Tiles::EImageTileParseResult::WrongMagic,
			"rifiuta una tile di quote", Tiles::DescribeParseResult(Result));
	}

	{
		std::vector<uint8_t> Bytes = MakeImageTileBytes(5, 1, 1, 100, 64);
		Bytes.resize(Tiles::ImageTileHeaderBytes + 10);   // payload troncato
		FImageTile Tile;
		const auto Result = Tiles::ParseImageTile(Bytes.data(), Bytes.size(), Tile);
		Check(Result == Tiles::EImageTileParseResult::TruncatedPayload,
			"si accorge di un payload troncato", Tiles::DescribeParseResult(Result));
	}

	{
		const std::vector<uint8_t> Bytes = MakeImageTileBytes(
			5, 1, 1, 100, 64, Tiles::ImageTileMagic, Tiles::ImageTileVersion, 128);
		FImageTile Tile;
		const auto Result = Tiles::ParseImageTile(Bytes.data(), Bytes.size(), Tile);
		Check(Result == Tiles::EImageTileParseResult::WrongSize,
			"rifiuta una tile di lato sbagliato", Tiles::DescribeParseResult(Result));
	}

	{
		FImageTile Tile;
		const std::vector<uint8_t> Short(10, 0);
		const auto Result = Tiles::ParseImageTile(Short.data(), Short.size(), Tile);
		Check(Result == Tiles::EImageTileParseResult::TooShort,
			"rifiuta un file piu' corto dell'header");
	}

	// ------------------------------------------------------------------
	Section("5. Indice delle immagini");

	{
		std::vector<uint8_t> Bytes(Tiles::ImageIndexHeaderBytes + 2 * Tiles::ImageIndexRecordBytes, 0);
		auto PutU32 = [&Bytes](size_t At, uint32_t Value)
		{
			for (int i = 0; i < 4; ++i) { Bytes[At + i] = static_cast<uint8_t>((Value >> (8 * i)) & 0xFF); }
		};
		PutU32(0, Tiles::ImageIndexMagic);
		Bytes[4] = 1;
		PutU32(8, 13);
		PutU32(12, 2);
		PutU32(16, 4332); PutU32(20, 1012); Bytes[24] = 100;
		PutU32(28, 4333); PutU32(32, 1012); Bytes[36] = 62;

		std::vector<Tiles::FImageIndexEntry> Entries;
		const char* Error = nullptr;
		const bool bOk = Tiles::ParseImageIndex(Bytes.data(), Bytes.size(), 13, Entries, Error);

		Check(bOk && Entries.size() == 2, "legge un indice valido", Error ? Error : "");
		Check(bOk && Entries[0].X == 4332 && Entries[0].CoveragePercent == 100,
			"prima voce corretta");
		Check(bOk && Entries[1].CoveragePercent == 62, "copertura parziale conservata");
	}

	{
		std::vector<uint8_t> Bytes(Tiles::ImageIndexHeaderBytes, 0);
		Bytes[0] = 'G'; Bytes[1] = 'W'; Bytes[2] = 'I'; Bytes[3] = 'X';   // indice di QUOTE
		Bytes[4] = 1;
		std::vector<Tiles::FImageIndexEntry> Entries;
		const char* Error = nullptr;
		const bool bOk = Tiles::ParseImageIndex(Bytes.data(), Bytes.size(), 0, Entries, Error);
		Check(!bOk && Error != nullptr, "rifiuta l'indice di un dataset di quote",
			Error ? Error : "nessun messaggio");
	}

	// ------------------------------------------------------------------
	Section("6. La cache e' diventata un template");

	{
		// Budget scelto perche' due tile ci stiano e la terza no: 3 x 20 KB
		// sforano 50 KB, 2 x 20 KB no.
		Tiles::TTileCache<FImageTile> Cache(50 * 1024);

		auto MakeTile = [](uint32_t X, size_t PayloadBytes)
		{
			auto Tile = std::make_shared<FImageTile>();
			Tile->Key = FTileKey{ 13, X, 1000 };
			Tile->Width = Tiles::TilePixels;
			Tile->Height = Tiles::TilePixels;
			Tile->CompressedBytes.resize(PayloadBytes);
			return std::shared_ptr<const FImageTile>(Tile);
		};

		Cache.Insert(FTileKey{ 13, 1, 1000 }, MakeTile(1, 20 * 1024));
		Cache.Insert(FTileKey{ 13, 2, 1000 }, MakeTile(2, 20 * 1024));

		Check(Cache.Find(FTileKey{ 13, 1, 1000 }) != nullptr,
			"la cache di immagini tiene le tile inserite");
		Check(Cache.GetStats().UsedBytes > 40 * 1024,
			"e conta i byte del payload compresso",
			Fmt("%.1f KB", Cache.GetStats().UsedBytes / 1024.0));

		// Sfratto: la terza tile non ci sta nel budget.
		Cache.Insert(FTileKey{ 13, 3, 1000 }, MakeTile(3, 20 * 1024));
		Check(Cache.GetStats().Evictions > 0, "e sfratta quando sfora il budget",
			std::to_string(Cache.GetStats().Evictions) + " sfratti");
	}

	{
		// La cache delle quote deve continuare a funzionare identica: e' la
		// prova che il template non ha rotto le Fasi 3-5.
		Tiles::FTileCache Heights(1024 * 1024);
		auto Tile = std::make_shared<Tiles::FHeightTile>();
		Tile->Key = FTileKey{ 12, 5, 5 };
		Tile->Heights.resize(Tiles::TilePosts * Tiles::TilePosts, 100.0f);
		Heights.Insert(FTileKey{ 12, 5, 5 }, Tile);

		Check(Heights.Find(FTileKey{ 12, 5, 5 }) != nullptr,
			"FTileCache resta l'alias della cache di quote e funziona come prima");
	}

	// ------------------------------------------------------------------
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}
