// =============================================================================
//  BuildingTileFormat.h -- Lettura delle tile di edifici (.gbd) e del loro
//  indice. STRATO: C++ PURO.
//
//  Il formato e' descritto per esteso in Pipeline/geoworld/bldformat.py, che e'
//  anche chi lo scrive. In breve: un header di 32 byte (con la dimensione del
//  payload compresso e di quello decompresso) e un payload zlib con
//  gli edifici uno dopo l'altro; ogni edificio un record di 30 byte (classe,
//  tetto, flag, piani, altezze in dm, colori, id OSM, Wikidata, quanti anelli)
//  e i suoi anelli: il contorno (antiorario) e i cortili (orari), con i
//  vertici in int16, frazioni della tile x 16384 da sud-ovest (x est, y nord).
//
//  LA DECOMPRESSIONE NON E' QUI, come per l'acqua e la vegetazione: in Unreal
//  la fa FCompression, nei test la zlib di sistema.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Tiles/BuildingClasses.h"
#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"

namespace GeoWorld::Tiles
{
	inline constexpr uint32_t BuildingTileMagic = 0x44425747u;     // "GWBD" little-endian
	inline constexpr uint16_t BuildingTileVersion = 1;
	inline constexpr int32_t BuildingTileHeaderBytes = 32;
	inline constexpr int32_t BuildingRecordBytes = 30;
	inline constexpr int32_t BuildingQuantum = 16384;
	inline constexpr uint32_t BuildingIndexMagic = 0x49425747u;    // "GWBI"

	struct FBuildingTileHeader
	{
		uint16_t Version = 0;
		FTileKey Key;
		uint32_t Count = 0;
		uint32_t PayloadBytes = 0;
		/** La dimensione del payload decompresso: FCompression la vuole esatta. */
		uint32_t PlainBytes = 0;
	};

	/** Un anello: i vertici in gradi (lon, lat), senza ripetere il primo in fondo. */
	struct FBuildingRing
	{
		std::vector<double> Lon, Lat;
		size_t Size() const { return Lon.size(); }
	};

	struct FBuilding
	{
		uint8_t Class = 0;
		uint8_t Roof = 0;
		uint8_t Flags = 0;
		uint8_t Levels = 0;
		float HeightM = 0.0f;
		float MinHeightM = 0.0f;
		/** 0: lo decide il runtime dalla sagoma. */
		float RoofHeightM = 0.0f;
		uint8_t WallColour[3] = { 0, 0, 0 };
		uint8_t RoofColour[3] = { 0, 0, 0 };
		/** Il tipo nei 2 bit alti (1 way, 2 relazione), il numero sotto. */
		uint64_t OsmId = 0;
		uint32_t Wikidata = 0;
		/** Il primo e' il contorno, gli altri i cortili. */
		std::vector<FBuildingRing> Rings;

		uint64_t OsmNumber() const { return OsmId & ((1ull << 62) - 1); }
		uint32_t OsmKind() const { return static_cast<uint32_t>(OsmId >> 62); }
	};

	struct FBuildingTile
	{
		FTileKey Key;
		std::vector<FBuilding> Buildings;

		size_t GetByteSize() const
		{
			size_t Bytes = sizeof(FBuildingTile) + Buildings.size() * sizeof(FBuilding);
			for (const FBuilding& Building : Buildings)
			{
				for (const FBuildingRing& Ring : Building.Rings) { Bytes += sizeof(FBuildingRing) + Ring.Size() * 16; }
			}
			return Bytes;
		}
	};

	namespace BuildingDetail
	{
		inline uint16_t ReadU16(const uint8_t* Data) { return static_cast<uint16_t>(Data[0] | (Data[1] << 8)); }
		inline int16_t ReadI16(const uint8_t* Data) { return static_cast<int16_t>(ReadU16(Data)); }
		inline uint32_t ReadU32(const uint8_t* Data)
		{
			return static_cast<uint32_t>(Data[0]) | (static_cast<uint32_t>(Data[1]) << 8)
			     | (static_cast<uint32_t>(Data[2]) << 16) | (static_cast<uint32_t>(Data[3]) << 24);
		}
		inline uint64_t ReadU64(const uint8_t* Data)
		{
			return static_cast<uint64_t>(ReadU32(Data)) | (static_cast<uint64_t>(ReadU32(Data + 4)) << 32);
		}
	}

	/** L'header; controlla che la chiave sia quella attesa (il file e' nel posto giusto). */
	inline bool ParseBuildingTileHeader(const uint8_t* Data, size_t Size, const FTileKey& ExpectedKey,
	                                    FBuildingTileHeader& Out, std::string& OutError)
	{
		using namespace BuildingDetail;
		if (!Data || Size < static_cast<size_t>(BuildingTileHeaderBytes))
		{
			OutError = "file troppo corto per l'header";
			return false;
		}
		if (ReadU32(Data) != BuildingTileMagic)
		{
			OutError = "non e' una tile di edifici (magic)";
			return false;
		}
		Out.Version = ReadU16(Data + 4);
		Out.Key = FTileKey{ ReadU32(Data + 8), ReadU32(Data + 12), ReadU32(Data + 16) };
		Out.Count = ReadU32(Data + 20);
		Out.PayloadBytes = ReadU32(Data + 24);
		Out.PlainBytes = ReadU32(Data + 28);
		if (Out.Version != BuildingTileVersion)
		{
			OutError = "versione del formato sconosciuta";
			return false;
		}
		if (!(Out.Key == ExpectedKey))
		{
			OutError = "la tile dichiara una chiave diversa da quella del percorso";
			return false;
		}
		if (Size != static_cast<size_t>(BuildingTileHeaderBytes) + Out.PayloadBytes)
		{
			OutError = "dimensione del file incoerente con l'header (troncato?)";
			return false;
		}
		return true;
	}

	/** Il payload GIA' DECOMPRESSO: gli edifici, con i vertici in gradi. */
	inline bool DecodeBuildingTilePayload(const FBuildingTileHeader& Header, const uint8_t* Plain, size_t Size,
	                                      FBuildingTile& Out, std::string& OutError)
	{
		using namespace BuildingDetail;
		if (Size != Header.PlainBytes)
		{
			OutError = "payload di dimensione diversa da quella dell'header";
			return false;
		}
		Out = FBuildingTile();
		Out.Key = Header.Key;
		const FTileBounds Bounds = GetTileBounds(Header.Key.Level, Header.Key.X, Header.Key.Y);
		const double Step = TileSpanDeg(Header.Key.Level) / BuildingQuantum;
		size_t Offset = 0;
		Out.Buildings.reserve(Header.Count);
		for (uint32_t Index = 0; Index < Header.Count; ++Index)
		{
			if (!Plain || Offset + BuildingRecordBytes > Size)
			{
				OutError = "payload troncato (record)";
				return false;
			}
			const uint8_t* Record = Plain + Offset;
			FBuilding Building;
			Building.Class = Record[0] < BuildingClassCount ? Record[0] : 0;
			Building.Roof = Record[1];
			Building.Flags = Record[2];
			Building.Levels = Record[3];
			Building.HeightM = ReadU16(Record + 4) * 0.1f;
			Building.MinHeightM = ReadU16(Record + 6) * 0.1f;
			Building.RoofHeightM = ReadU16(Record + 8) * 0.1f;
			for (int32_t Channel = 0; Channel < 3; ++Channel)
			{
				Building.WallColour[Channel] = Record[10 + Channel];
				Building.RoofColour[Channel] = Record[13 + Channel];
			}
			Building.OsmId = ReadU64(Record + 16);
			Building.Wikidata = ReadU32(Record + 24);
			const uint16_t RingCount = ReadU16(Record + 28);
			Offset += BuildingRecordBytes;
			Building.Rings.resize(RingCount);
			for (FBuildingRing& Ring : Building.Rings)
			{
				if (Offset + 2 > Size)
				{
					OutError = "payload troncato (anello)";
					return false;
				}
				const uint16_t Vertices = ReadU16(Plain + Offset);
				Offset += 2;
				if (Offset + static_cast<size_t>(Vertices) * 4 > Size)
				{
					OutError = "payload troncato (vertici)";
					return false;
				}
				Ring.Lon.resize(Vertices);
				Ring.Lat.resize(Vertices);
				for (uint16_t Vertex = 0; Vertex < Vertices; ++Vertex)
				{
					Ring.Lon[Vertex] = Bounds.West + ReadI16(Plain + Offset) * Step;
					Ring.Lat[Vertex] = Bounds.South + ReadI16(Plain + Offset + 2) * Step;
					Offset += 4;
				}
			}
			if (!Building.Rings.empty() && Building.Rings[0].Size() >= 3) { Out.Buildings.push_back(std::move(Building)); }
		}
		if (Offset != Size)
		{
			OutError = "byte in avanzo nel payload";
			return false;
		}
		return true;
	}

	// =========================================================================
	//  L'indice: le tile che esistono, con quanti edifici
	// =========================================================================

	struct FBuildingIndexEntry
	{
		uint32_t X = 0, Y = 0;
		uint32_t Count = 0;
		uint32_t Bytes = 0;
	};

	inline bool ParseBuildingIndex(const uint8_t* Data, size_t Size, uint32_t ExpectedLevel,
	                               std::vector<FBuildingIndexEntry>& Out, std::string& OutError)
	{
		using namespace BuildingDetail;
		Out.clear();
		constexpr size_t HeaderBytes = 16, RecordBytes = 16;
		if (!Data || Size < HeaderBytes || ReadU32(Data) != BuildingIndexMagic)
		{
			OutError = "non e' l'indice di un dataset di edifici";
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
			Out.push_back(FBuildingIndexEntry{ ReadU32(Record), ReadU32(Record + 4), ReadU32(Record + 8), ReadU32(Record + 12) });
		}
		return true;
	}
}
