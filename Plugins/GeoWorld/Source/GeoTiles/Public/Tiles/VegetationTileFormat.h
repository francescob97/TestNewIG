// =============================================================================
//  VegetationTileFormat.h -- Lettura delle tile di vegetazione (.gvg) e del
//  loro indice. STRATO: C++ PURO.
//
//  Il formato e' descritto per esteso in Pipeline/geoworld/vegformat.py, che
//  e' anche chi lo scrive. In breve:
//
//      header   32 byte: "GWVG", versione, flag, livello, x, y, raster (256),
//               riservato, byte del payload, riservato
//      payload  zlib di raster x raster byte: la classe della copertura del
//               suolo (VegetationClasses.h), righe dal NORD, colonne
//               dall'OVEST, come le quote e le immagini
//
//  Tutte le tile di un dataset stanno a UN livello (13 di default: 2,4 km di
//  lato, pixel da ~10 m come WorldCover). Le celle degli alberi, piu' piccole,
//  leggono la tile che le contiene.
//
//  LA DECOMPRESSIONE NON E' QUI, come per l'acqua: in Unreal la fa
//  FCompression (NAME_Zlib), nei test la zlib di sistema.
// =============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"
#include "Tiles/VegetationClasses.h"

namespace GeoWorld::Tiles
{
	inline constexpr uint32_t VegetationTileMagic = 0x47565747u;    // "GWVG" little-endian
	inline constexpr uint16_t VegetationTileVersion = 1;
	inline constexpr int32_t VegetationTileHeaderBytes = 32;
	inline constexpr int32_t VegetationRaster = 256;

	// Flag della tile (header e indice): cosa c'e' dentro, per saltare subito le tile vuote.
	inline constexpr uint16_t VegetationTileTrees = 1 << 0;
	inline constexpr uint16_t VegetationTileShrubs = 1 << 1;

	struct FVegetationTileHeader
	{
		uint16_t Version = 0;
		uint16_t Flags = 0;
		FTileKey Key;
		uint16_t Raster = 0;
		uint32_t PayloadBytes = 0;

		size_t ExpectedPlainBytes() const { return static_cast<size_t>(Raster) * Raster; }
	};

	/** Una tile in memoria: le classi, un byte per pixel. */
	struct FVegetationTile
	{
		FTileKey Key;
		uint16_t Flags = 0;
		std::vector<uint8_t> Classes;   // VegetationRaster^2, righe dal nord

		bool IsValid() const { return Classes.size() == static_cast<size_t>(VegetationRaster) * VegetationRaster; }

		/** La classe nel punto (gradi); NoData fuori dalla tile. */
		uint8_t ClassAt(double LatDeg, double LonDeg) const
		{
			if (!IsValid()) { return static_cast<uint8_t>(ELandCover::NoData); }
			const FTileBounds Bounds = GetTileBounds(Key.Level, Key.X, Key.Y);
			const double Span = TileSpanDeg(Key.Level);
			const double Column = std::floor((LonDeg - Bounds.West) / Span * VegetationRaster);
			const double Row = std::floor((Bounds.North - LatDeg) / Span * VegetationRaster);
			if (Column < 0.0 || Row < 0.0 || Column >= VegetationRaster || Row >= VegetationRaster)
			{
				return static_cast<uint8_t>(ELandCover::NoData);
			}
			return Classes[static_cast<size_t>(Row) * VegetationRaster + static_cast<size_t>(Column)];
		}

		size_t GetByteSize() const { return sizeof(FVegetationTile) + Classes.size(); }
	};

	namespace VegetationDetail
	{
		inline uint16_t ReadU16(const uint8_t* Data) { return static_cast<uint16_t>(Data[0] | (Data[1] << 8)); }
		inline uint32_t ReadU32(const uint8_t* Data)
		{
			return static_cast<uint32_t>(Data[0]) | (static_cast<uint32_t>(Data[1]) << 8)
			     | (static_cast<uint32_t>(Data[2]) << 16) | (static_cast<uint32_t>(Data[3]) << 24);
		}
	}

	/**
	 * Legge e VALIDA l'header. ExpectedKey intercetta il caso che capita davvero:
	 * un dataset rigenerato con file vecchi rimasti sul disco.
	 */
	inline bool ParseVegetationTileHeader(const uint8_t* Data, size_t Size, const FTileKey& ExpectedKey,
	                                      FVegetationTileHeader& Out, std::string& OutError)
	{
		using namespace VegetationDetail;
		if (!Data || Size < static_cast<size_t>(VegetationTileHeaderBytes))
		{
			OutError = "file troppo corto per l'header";
			return false;
		}
		if (ReadU32(Data) != VegetationTileMagic)
		{
			OutError = "non e' una tile di vegetazione (magic)";
			return false;
		}
		Out.Version = ReadU16(Data + 4);
		Out.Flags = ReadU16(Data + 6);
		Out.Key = FTileKey{ ReadU32(Data + 8), ReadU32(Data + 12), ReadU32(Data + 16) };
		Out.Raster = ReadU16(Data + 20);
		Out.PayloadBytes = ReadU32(Data + 24);
		if (Out.Version != VegetationTileVersion)
		{
			OutError = "versione " + std::to_string(Out.Version) + ", attesa " + std::to_string(VegetationTileVersion);
			return false;
		}
		if (Out.Raster != VegetationRaster)
		{
			OutError = "raster di " + std::to_string(Out.Raster) + " pixel, atteso " + std::to_string(VegetationRaster);
			return false;
		}
		if (Out.Key != ExpectedKey)
		{
			OutError = "la tile dichiara una chiave diversa da quella del percorso";
			return false;
		}
		if (Size != static_cast<size_t>(VegetationTileHeaderBytes) + Out.PayloadBytes)
		{
			OutError = "dimensione del file incoerente con l'header (troncato?)";
			return false;
		}
		return true;
	}

	/** Interpreta il payload GIA' DECOMPRESSO. */
	inline bool DecodeVegetationTilePayload(const FVegetationTileHeader& Header, const uint8_t* Plain, size_t Size,
	                                        FVegetationTile& Out, std::string& OutError)
	{
		if (!Plain || Size != Header.ExpectedPlainBytes())
		{
			OutError = "payload di " + std::to_string(Size) + " byte, attesi " + std::to_string(Header.ExpectedPlainBytes());
			return false;
		}
		Out = FVegetationTile();
		Out.Key = Header.Key;
		Out.Flags = Header.Flags;
		Out.Classes.assign(Plain, Plain + Size);
		for (uint8_t& Class : Out.Classes)
		{
			// Un numero che non conosciamo (un dataset di una versione futura) non fa crescere niente.
			if (Class >= LandCoverCount) { Class = static_cast<uint8_t>(ELandCover::NoData); }
		}
		return true;
	}

	// =========================================================================
	//  L'indice: le tile che esistono, con i loro flag
	// =========================================================================

	inline constexpr uint32_t VegetationIndexMagic = 0x49565747u;   // "GWVI"

	struct FVegetationIndexEntry
	{
		uint32_t X = 0, Y = 0;
		uint16_t Flags = 0;
		uint32_t Bytes = 0;
	};

	inline bool ParseVegetationIndex(const uint8_t* Data, size_t Size, uint32_t ExpectedLevel,
	                                 std::vector<FVegetationIndexEntry>& Out, std::string& OutError)
	{
		using namespace VegetationDetail;
		Out.clear();
		constexpr size_t HeaderBytes = 16, RecordBytes = 16;
		if (!Data || Size < HeaderBytes || ReadU32(Data) != VegetationIndexMagic)
		{
			OutError = "non e' l'indice di un dataset di vegetazione";
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
			Out.push_back(FVegetationIndexEntry{ ReadU32(Record), ReadU32(Record + 4), ReadU16(Record + 8), ReadU32(Record + 12) });
		}
		return true;
	}
}
