// =============================================================================
//  WaterTileFormat.h -- Lettura delle tile d'acqua (.gwa), del loro indice e
//  del geoide del dataset. STRATO: C++ PURO.
//
//  Il formato e' descritto per esteso in Pipeline/geoworld/waterformat.py, che
//  e' anche chi lo scrive. In breve:
//
//      header   32 byte: "GWWA", versione, flag, livello, x, y, posts (129),
//               raster (256), byte del payload, riservato
//      payload  zlib di: superficie int16[129²] (decimetri), fondale
//               int16[129²], SDF uint8[256²], onde uint8[256²], e se c'e' la
//               corrente u, v int8[256²] ciascuno
//
//  LA DECOMPRESSIONE NON E' QUI. Lo strato puro non conosce zlib: in Unreal la
//  fa FCompression (NAME_Zlib), nei test standalone la zlib di sistema. Qui si
//  legge l'header e si interpreta il payload GIA' decompresso. E' la stessa
//  divisione delle immagini: il JPEG lo decodifica il motore.
//
//  Le quote sono in decimetri sull'ellissoide, con lo STESSO zero per tutte le
//  tile: due tile vicine scrivono lo stesso numero sul post che condividono.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"

namespace GeoWorld::Tiles
{
	inline constexpr uint32_t WaterTileMagic = 0x41575747u;     // "GWWA" little-endian
	inline constexpr uint16_t WaterTileVersion = 1;
	inline constexpr int32_t WaterTileHeaderBytes = 32;

	/** Posti della griglia (quelli del terreno) e lato del raster. */
	inline constexpr int32_t WaterPosts = 129;
	inline constexpr int32_t WaterRaster = 256;

	/** Quota assente (niente acqua) o "non scavare", in decimetri. */
	inline constexpr int32_t WaterNoHeight = -32768;

	/** SDF: byte = WaterSdfZero + WaterSdfScale x distanza in pixel (positiva in acqua). */
	inline constexpr int32_t WaterSdfZero = 128;
	inline constexpr float WaterSdfScale = 8.0f;

	/** Corrente: int8 in centesimi di m/s (1 m/s = 100). */
	inline constexpr float WaterFlowScale = 100.0f;

	/**
	 * Le REGOLE con cui la pipeline fa l'acqua ("rulesVersion" nel manifest,
	 * waterclasses.WATER_RULES_VERSION). Quando cambiano le regole il formato
	 * resta quello e i numeri no: un dataset piu' vecchio si apre lo stesso,
	 * ma lo si dice in rosso. Con le regole 1 i fiumi diventavano fosse
	 * profonde fino a 300 m (docs/fase9-design.md, sezione 12).
	 */
	inline constexpr int32_t WaterRulesVersion = 2;

	// Flag della tile (header e indice). Stessi valori di waterclasses.py.
	inline constexpr uint16_t WaterTileSea = 1 << 0;
	inline constexpr uint16_t WaterTileLake = 1 << 1;
	inline constexpr uint16_t WaterTileRiver = 1 << 2;
	inline constexpr uint16_t WaterTileFlow = 1 << 3;
	inline constexpr uint16_t WaterTileAllWater = 1 << 4;

	struct FWaterTileHeader
	{
		uint16_t Version = 0;
		uint16_t Flags = 0;
		FTileKey Key;
		uint16_t Posts = 0;
		uint16_t Raster = 0;
		uint32_t PayloadBytes = 0;

		bool HasFlow() const { return (Flags & WaterTileFlow) != 0; }

		/** Byte del payload DECOMPRESSO che ci si aspetta. */
		size_t ExpectedPlainBytes() const
		{
			const size_t Grid = static_cast<size_t>(Posts) * Posts;
			const size_t Pixels = static_cast<size_t>(Raster) * Raster;
			return 2 * Grid * 2 + 2 * Pixels + (HasFlow() ? 2 * Pixels : 0);
		}
	};

	/**
	 * Una tile d'acqua in memoria. Le quote sono gia' in metri (NaN dove
	 * mancano), il raster resta in byte: va alla scheda video cosi' com'e'.
	 */
	struct FWaterTile
	{
		FTileKey Key;
		uint16_t Flags = 0;
		std::vector<float> Surface;     // WaterPosts², metri ellissoidici, NaN = niente acqua
		std::vector<float> Bed;         // WaterPosts², metri, NaN = non scavare
		std::vector<uint8_t> Sdf;       // WaterRaster²
		std::vector<uint8_t> Wave;      // WaterRaster²
		std::vector<int8_t> FlowU;      // WaterRaster², vuoto senza corrente (verso est)
		std::vector<int8_t> FlowV;      // WaterRaster², verso SUD

		bool IsValid() const
		{
			return Surface.size() == static_cast<size_t>(WaterPosts) * WaterPosts
			    && Bed.size() == Surface.size()
			    && Sdf.size() == static_cast<size_t>(WaterRaster) * WaterRaster
			    && Wave.size() == Sdf.size();
		}
		bool HasFlow() const { return FlowU.size() == Sdf.size() && FlowV.size() == Sdf.size(); }

		float SurfaceAt(int32_t I, int32_t J) const { return Surface[static_cast<size_t>(J) * WaterPosts + I]; }
		float BedAt(int32_t I, int32_t J) const { return Bed[static_cast<size_t>(J) * WaterPosts + I]; }

		/** Distanza dalla riva in pixel del raster (positiva in acqua). */
		float SdfPixels(int32_t Column, int32_t Row) const
		{
			const uint8_t Encoded = Sdf[static_cast<size_t>(Row) * WaterRaster + Column];
			return (static_cast<float>(Encoded) - static_cast<float>(WaterSdfZero)) / WaterSdfScale;
		}

		size_t GetByteSize() const
		{
			return sizeof(FWaterTile) + (Surface.size() + Bed.size()) * sizeof(float)
			     + Sdf.size() + Wave.size() + FlowU.size() + FlowV.size();
		}
	};

	namespace WaterDetail
	{
		inline uint16_t ReadU16(const uint8_t* Data) { return static_cast<uint16_t>(Data[0] | (Data[1] << 8)); }
		inline uint32_t ReadU32(const uint8_t* Data)
		{
			return static_cast<uint32_t>(Data[0]) | (static_cast<uint32_t>(Data[1]) << 8)
			     | (static_cast<uint32_t>(Data[2]) << 16) | (static_cast<uint32_t>(Data[3]) << 24);
		}
		inline int16_t ReadI16(const uint8_t* Data) { return static_cast<int16_t>(ReadU16(Data)); }
		inline double ReadF64(const uint8_t* Data)
		{
			double Value = 0.0;
			std::memcpy(&Value, Data, sizeof(double));   // little-endian come tutte le macchine bersaglio
			return Value;
		}
		inline float ReadF32(const uint8_t* Data)
		{
			float Value = 0.0f;
			std::memcpy(&Value, Data, sizeof(float));
			return Value;
		}

		inline float DecimetresToMetres(int16_t Value)
		{
			return (Value == WaterNoHeight) ? std::numeric_limits<float>::quiet_NaN()
			                                : static_cast<float>(Value) * 0.1f;
		}
	}

	/**
	 * Legge e VALIDA l'header. ExpectedKey intercetta il caso che capita davvero:
	 * una piramide rigenerata con file vecchi rimasti sul disco.
	 */
	inline bool ParseWaterTileHeader(const uint8_t* Data, size_t Size, const FTileKey& ExpectedKey,
	                                 FWaterTileHeader& Out, std::string& OutError)
	{
		using namespace WaterDetail;
		if (!Data || Size < static_cast<size_t>(WaterTileHeaderBytes))
		{
			OutError = "file troppo corto per l'header";
			return false;
		}
		if (ReadU32(Data) != WaterTileMagic)
		{
			OutError = "non e' una tile d'acqua (magic)";
			return false;
		}
		Out.Version = ReadU16(Data + 4);
		Out.Flags = ReadU16(Data + 6);
		Out.Key = FTileKey{ ReadU32(Data + 8), ReadU32(Data + 12), ReadU32(Data + 16) };
		Out.Posts = ReadU16(Data + 20);
		Out.Raster = ReadU16(Data + 22);
		Out.PayloadBytes = ReadU32(Data + 24);

		if (Out.Version != WaterTileVersion)
		{
			OutError = "versione " + std::to_string(Out.Version) + ", attesa " + std::to_string(WaterTileVersion);
			return false;
		}
		if (Out.Posts != WaterPosts || Out.Raster != WaterRaster)
		{
			OutError = "dimensioni inattese";
			return false;
		}
		if (Out.Key != ExpectedKey)
		{
			OutError = "la tile dichiara una chiave diversa da quella del percorso";
			return false;
		}
		if (Size != static_cast<size_t>(WaterTileHeaderBytes) + Out.PayloadBytes)
		{
			OutError = "dimensione del file incoerente con l'header (troncato?)";
			return false;
		}
		return true;
	}

	/** Interpreta il payload GIA' DECOMPRESSO. */
	inline bool DecodeWaterTilePayload(const FWaterTileHeader& Header, const uint8_t* Plain, size_t Size,
	                                   FWaterTile& Out, std::string& OutError)
	{
		using namespace WaterDetail;
		if (!Plain || Size != Header.ExpectedPlainBytes())
		{
			OutError = "payload di " + std::to_string(Size) + " byte, attesi "
			         + std::to_string(Header.ExpectedPlainBytes());
			return false;
		}
		const size_t Grid = static_cast<size_t>(WaterPosts) * WaterPosts;
		const size_t Pixels = static_cast<size_t>(WaterRaster) * WaterRaster;

		Out = FWaterTile();
		Out.Key = Header.Key;
		Out.Flags = Header.Flags;
		Out.Surface.resize(Grid);
		Out.Bed.resize(Grid);
		const uint8_t* Cursor = Plain;
		for (size_t Index = 0; Index < Grid; ++Index) { Out.Surface[Index] = DecimetresToMetres(ReadI16(Cursor + Index * 2)); }
		Cursor += Grid * 2;
		for (size_t Index = 0; Index < Grid; ++Index) { Out.Bed[Index] = DecimetresToMetres(ReadI16(Cursor + Index * 2)); }
		Cursor += Grid * 2;
		Out.Sdf.assign(Cursor, Cursor + Pixels);
		Cursor += Pixels;
		Out.Wave.assign(Cursor, Cursor + Pixels);
		Cursor += Pixels;
		if (Header.HasFlow())
		{
			Out.FlowU.resize(Pixels);
			Out.FlowV.resize(Pixels);
			std::memcpy(Out.FlowU.data(), Cursor, Pixels);
			Cursor += Pixels;
			std::memcpy(Out.FlowV.data(), Cursor, Pixels);
		}
		return true;
	}

	/**
	 * Lato di un pixel del raster in metri, al centro della tile: media
	 * geometrica fra est e nord. E' l'unita' dell'SDF (vedi waterformat.py).
	 */
	inline double WaterMetresPerPixel(uint32_t Level, uint32_t Y)
	{
		const FTileBounds Bounds = GetTileBounds(Level, 0, Y);
		const double Centre = 0.5 * (Bounds.North + Bounds.South) * 3.14159265358979323846 / 180.0;
		// Le stesse costanti di tiling.metres_per_degree della pipeline.
		const double North = 111132.0;
		const double East = 111320.0 * std::cos(Centre);
		const double Span = TileSpanDeg(Level) / WaterRaster;
		return std::sqrt(Span * East * Span * North);
	}

	// =========================================================================
	//  L'indice di livello
	// =========================================================================

	inline constexpr uint32_t WaterIndexMagic = 0x49575747u;    // "GWWI"

	struct FWaterIndexEntry
	{
		uint32_t X = 0, Y = 0;
		uint16_t Flags = 0;
		uint32_t Bytes = 0;
	};

	inline bool ParseWaterIndex(const uint8_t* Data, size_t Size, uint32_t ExpectedLevel,
	                            std::vector<FWaterIndexEntry>& Out, std::string& OutError)
	{
		using namespace WaterDetail;
		Out.clear();
		constexpr size_t HeaderBytes = 16, RecordBytes = 16;
		if (!Data || Size < HeaderBytes || ReadU32(Data) != WaterIndexMagic)
		{
			OutError = "non e' l'indice di un dataset d'acqua";
			return false;
		}
		const uint32_t Level = ReadU32(Data + 8);
		const uint32_t Count = ReadU32(Data + 12);
		if (Level != ExpectedLevel || Size != HeaderBytes + static_cast<size_t>(Count) * RecordBytes)
		{
			OutError = "indice incoerente (livello o dimensione)";
			return false;
		}
		Out.reserve(Count);
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			const uint8_t* Record = Data + HeaderBytes + static_cast<size_t>(Index) * RecordBytes;
			Out.push_back(FWaterIndexEntry{ ReadU32(Record), ReadU32(Record + 4), ReadU16(Record + 8), ReadU32(Record + 12) });
		}
		return true;
	}

	// =========================================================================
	//  Il geoide del dataset (geoid.bin)
	// =========================================================================

	/**
	 * L'ondulazione del geoide N su una griglia regolare: la quota ellissoidica
	 * del livello medio del mare. Serve alla quota "s.l.m." nelle stampe.
	 */
	struct FGeoidGrid
	{
		double OriginLon = 0.0, OriginLat = 0.0, Spacing = 1.0;
		int32_t Columns = 0, Rows = 0;
		std::vector<float> Undulation;     // riga 0 = NORD

		bool IsValid() const
		{
			return Columns >= 2 && Rows >= 2 && Undulation.size() == static_cast<size_t>(Columns) * Rows;
		}

		/** Bilineare. Falso fuori dalla griglia. */
		bool Sample(double Lon, double Lat, double& OutN) const
		{
			if (!IsValid()) { return false; }
			const double Fx = (Lon - OriginLon) / Spacing;
			const double Fy = (OriginLat - Lat) / Spacing;
			if (Fx < 0.0 || Fy < 0.0 || Fx > Columns - 1 || Fy > Rows - 1) { return false; }
			const int32_t X0 = std::min(Columns - 2, static_cast<int32_t>(Fx));
			const int32_t Y0 = std::min(Rows - 2, static_cast<int32_t>(Fy));
			const double Ax = Fx - X0, Ay = Fy - Y0;
			auto At = [this](int32_t X, int32_t Y) { return static_cast<double>(Undulation[static_cast<size_t>(Y) * Columns + X]); };
			OutN = At(X0, Y0) * (1 - Ax) * (1 - Ay) + At(X0 + 1, Y0) * Ax * (1 - Ay)
			     + At(X0, Y0 + 1) * (1 - Ax) * Ay + At(X0 + 1, Y0 + 1) * Ax * Ay;
			return true;
		}
	};

	inline constexpr uint32_t GeoidMagic = 0x45475747u;         // "GWGE"
	inline constexpr int32_t GeoidHeaderBytes = 40;

	inline bool ParseGeoid(const uint8_t* Data, size_t Size, FGeoidGrid& Out, std::string& OutError)
	{
		using namespace WaterDetail;
		if (!Data || Size < static_cast<size_t>(GeoidHeaderBytes) || ReadU32(Data) != GeoidMagic || ReadU16(Data + 4) != 1)
		{
			OutError = "geoid.bin non valido";
			return false;
		}
		Out.Columns = static_cast<int32_t>(ReadU32(Data + 8));
		Out.Rows = static_cast<int32_t>(ReadU32(Data + 12));
		Out.OriginLon = ReadF64(Data + 16);
		Out.OriginLat = ReadF64(Data + 24);
		Out.Spacing = ReadF64(Data + 32);
		const size_t Count = static_cast<size_t>(Out.Columns) * Out.Rows;
		if (Size != static_cast<size_t>(GeoidHeaderBytes) + Count * 4 || Out.Spacing <= 0.0)
		{
			OutError = "geoid.bin: dimensione incoerente";
			return false;
		}
		Out.Undulation.resize(Count);
		for (size_t Index = 0; Index < Count; ++Index) { Out.Undulation[Index] = ReadF32(Data + GeoidHeaderBytes + Index * 4); }
		return true;
	}

	// =========================================================================
	//  Il mare aperto del dataset (opensea.bin)
	// =========================================================================
	//
	//  Al largo il dataset del terreno non ha tile (non hanno nemmeno un post di
	//  dato) e il quadtree lascerebbe un buco. La pipeline elenca le tile SENZA
	//  terreno tutte in mare, al livello piu' grossolano possibile, e una
	//  griglia della profondita' del fondale: il runtime ci costruisce tile di
	//  terreno e d'acqua VIRTUALI (OpenSea.h). Formato in waterformat.py.

	inline constexpr uint32_t OpenSeaMagic = 0x534F5747u;       // "GWOS"
	inline constexpr uint16_t OpenSeaVersion = 1;
	inline constexpr int32_t OpenSeaHeaderBytes = 48;
	inline constexpr int32_t OpenSeaRecordBytes = 12;
	/** Profondita' assente (terra, o fuori dalla griglia), in decimetri uint16. */
	inline constexpr uint32_t OpenSeaNoDepth = 65535;          // 0xFFFF

	struct FOpenSeaHeader
	{
		uint32_t TileCount = 0;
		uint32_t Columns = 0, Rows = 0;
		uint32_t PayloadBytes = 0;
		double West = 0.0, North = 0.0, Spacing = 1.0;

		size_t ExpectedPlainBytes() const
		{
			return static_cast<size_t>(TileCount) * OpenSeaRecordBytes + static_cast<size_t>(Columns) * Rows * 2;
		}
	};

	struct FOpenSea
	{
		/** Le tile elencate (FTileKey::Pack): loro e tutte le loro discendenti sono mare. */
		std::unordered_set<uint64_t> Tiles;
		uint32_t MinLevel = 0xFFFFFFFFu;
		/** Griglia per CELLE: la cella (c, r) copre [West + c * Spacing, West + (c + 1) * Spacing]. */
		int32_t Columns = 0, Rows = 0;
		double West = 0.0, North = 0.0, Spacing = 1.0;
		std::vector<uint16_t> DepthDm;     // righe x colonne, riga 0 = NORD

		bool IsEmpty() const { return Tiles.empty(); }

		/** La tile, o un suo antenato, e' elencata: niente terreno, tutta mare. */
		bool Covers(const FTileKey& Key) const
		{
			if (Tiles.empty()) { return false; }
			for (uint32_t Up = 0; Up <= Key.Level; ++Up)
			{
				const uint32_t Level = Key.Level - Up;
				if (Level < MinLevel) { break; }
				if (Tiles.count(FTileKey{ Level, Key.X >> Up, Key.Y >> Up }.Pack()) != 0) { return true; }
			}
			return false;
		}

		/**
		 * Profondita' del fondale in metri: bilineare sulle sole celle di mare
		 * attorno al punto; `Fallback` se attorno e' tutto terra (succede: la
		 * griglia e' piu' grossolana della maschera che ha deciso la tile).
		 */
		double DepthAt(double Lon, double Lat, double Fallback) const
		{
			if (Columns < 1 || Rows < 1 || DepthDm.size() != static_cast<size_t>(Columns) * Rows) { return Fallback; }
			const double Fx = std::min(static_cast<double>(Columns) - 1.0, std::max(0.0, (Lon - West) / Spacing - 0.5));
			const double Fy = std::min(static_cast<double>(Rows) - 1.0, std::max(0.0, (North - Lat) / Spacing - 0.5));
			const int32_t X0 = static_cast<int32_t>(Fx), Y0 = static_cast<int32_t>(Fy);
			const int32_t X1 = std::min(Columns - 1, X0 + 1), Y1 = std::min(Rows - 1, Y0 + 1);
			const double Ax = Fx - X0, Ay = Fy - Y0;
			const int32_t Xs[4] = { X0, X1, X0, X1 };
			const int32_t Ys[4] = { Y0, Y0, Y1, Y1 };
			const double Weights[4] = { (1 - Ax) * (1 - Ay), Ax * (1 - Ay), (1 - Ax) * Ay, Ax * Ay };
			double Sum = 0.0, Weight = 0.0;
			for (int Corner = 0; Corner < 4; ++Corner)
			{
				const uint16_t Value = DepthDm[static_cast<size_t>(Ys[Corner]) * Columns + Xs[Corner]];
				if (Value == OpenSeaNoDepth) { continue; }
				Sum += 0.1 * Value * Weights[Corner];
				Weight += Weights[Corner];
			}
			return (Weight > 1.0e-9) ? Sum / Weight : Fallback;
		}
	};

	inline bool ParseOpenSeaHeader(const uint8_t* Data, size_t Size, FOpenSeaHeader& Out, std::string& OutError)
	{
		using namespace WaterDetail;
		if (!Data || Size < static_cast<size_t>(OpenSeaHeaderBytes) || ReadU32(Data) != OpenSeaMagic)
		{
			OutError = "opensea.bin non valido (magic)";
			return false;
		}
		if (ReadU16(Data + 4) != OpenSeaVersion)
		{
			OutError = "opensea.bin: versione " + std::to_string(ReadU16(Data + 4)) + ", attesa "
			         + std::to_string(OpenSeaVersion);
			return false;
		}
		Out.TileCount = ReadU32(Data + 8);
		Out.Columns = ReadU32(Data + 12);
		Out.Rows = ReadU32(Data + 16);
		Out.PayloadBytes = ReadU32(Data + 20);
		Out.West = ReadF64(Data + 24);
		Out.North = ReadF64(Data + 32);
		Out.Spacing = ReadF64(Data + 40);
		if (Size != static_cast<size_t>(OpenSeaHeaderBytes) + Out.PayloadBytes || !(Out.Spacing > 0.0))
		{
			OutError = "opensea.bin: dimensione incoerente con l'header (troncato?)";
			return false;
		}
		return true;
	}

	/** Interpreta il payload GIA' DECOMPRESSO (zlib, come le tile). */
	inline bool DecodeOpenSeaPayload(const FOpenSeaHeader& Header, const uint8_t* Plain, size_t Size,
	                                 FOpenSea& Out, std::string& OutError)
	{
		using namespace WaterDetail;
		if (!Plain || Size != Header.ExpectedPlainBytes())
		{
			OutError = "opensea.bin: payload di " + std::to_string(Size) + " byte, attesi "
			         + std::to_string(Header.ExpectedPlainBytes());
			return false;
		}
		Out = FOpenSea();
		Out.Tiles.reserve(Header.TileCount);
		for (uint32_t Index = 0; Index < Header.TileCount; ++Index)
		{
			const uint8_t* Record = Plain + static_cast<size_t>(Index) * OpenSeaRecordBytes;
			const FTileKey Key{ ReadU32(Record), ReadU32(Record + 4), ReadU32(Record + 8) };
			Out.Tiles.insert(Key.Pack());
			Out.MinLevel = std::min(Out.MinLevel, Key.Level);
		}
		Out.Columns = static_cast<int32_t>(Header.Columns);
		Out.Rows = static_cast<int32_t>(Header.Rows);
		Out.West = Header.West;
		Out.North = Header.North;
		Out.Spacing = Header.Spacing;
		const uint8_t* Grid = Plain + static_cast<size_t>(Header.TileCount) * OpenSeaRecordBytes;
		Out.DepthDm.resize(static_cast<size_t>(Header.Columns) * Header.Rows);
		for (size_t Index = 0; Index < Out.DepthDm.size(); ++Index) { Out.DepthDm[Index] = ReadU16(Grid + Index * 2); }
		return true;
	}
}
