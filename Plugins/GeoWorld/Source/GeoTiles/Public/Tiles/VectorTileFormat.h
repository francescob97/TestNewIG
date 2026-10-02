// =============================================================================
//  VectorTileFormat.h -- Lettura delle tile vettoriali (.gvt). STRATO: C++ PURO.
//
//  Il formato e' descritto per esteso in Pipeline/geoworld/vectorformat.py, che
//  e' anche chi lo scrive. In breve:
//
//      header   32 byte: "GWVT", versione, flag, livello, x, y, extent, buffer,
//               numero di linee, numero di punti
//      linee    12 byte ciascuna: classe, flag, layer, riservato, larghezza in
//               decimetri, riservato, numero di punti
//      punti    (x, y) int16, di tutte le linee una dopo l'altra
//
//  Coordinate LOCALI: x = 0 sul bordo ovest e x = Extent sul bordo est; y = 0
//  sul bordo NORD e y = Extent sul bordo sud. Le linee escono dalla tile fino a
//  Buffer unita' (un ottavo del lato), perche' una strada larga che corre lungo
//  il bordo deve comparire anche nella tile accanto.
//
//  A differenza delle immagini qui non c'e' niente da decomprimere: la tile e'
//  pronta all'uso appena letta, e la si puo' leggere tutta in questo strato.
//  E' per questo che i test standalone la provano su un file scritto dalla
//  pipeline vera (TestData/vector_fixture.gvt), e non su byte scritti a mano.
// =============================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Tiles/TileKey.h"
#include "Tiles/VectorClasses.h"

namespace GeoWorld::Tiles
{
	inline constexpr uint32_t VectorTileMagic = 0x54565747u;    // "GWVT" little-endian
	inline constexpr uint16_t VectorTileVersion = 1;
	inline constexpr int32_t VectorTileHeaderBytes = 32;
	inline constexpr int32_t VectorFeatureRecordBytes = 12;

	/** Lato della tile in unita' locali, quello che scrive la pipeline. */
	inline constexpr int32_t VectorTileExtent = 16384;

	struct FVectorPoint
	{
		int16_t X = 0;
		int16_t Y = 0;
	};

	struct FVectorFeature
	{
		ERoadClass Class = ERoadClass::Unknown;
		uint8_t Flags = 0;
		int8_t Layer = 0;
		uint16_t WidthDecimetres = 0;

		/** Indice del primo punto in FVectorTile::Points. */
		uint32_t FirstPoint = 0;
		uint32_t PointCount = 0;

		/** Rettangolo dei punti, calcolato alla lettura: serve a scartare in
		 *  fretta le linee lontane dal pezzo di tile che si sta disegnando. */
		int16_t MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;

		float GetWidthMetres() const { return static_cast<float>(WidthDecimetres) * 0.1f; }
		bool IsBridge() const { return (Flags & RoadFlagBridge) != 0; }
		bool IsUnpaved() const { return (Flags & RoadFlagUnpaved) != 0; }
	};

	struct FVectorTile
	{
		FTileKey Key;
		int32_t Extent = VectorTileExtent;
		int32_t Buffer = 0;
		std::vector<FVectorFeature> Features;
		std::vector<FVectorPoint> Points;

		/** Per il budget della cache: e' l'unica cosa che TTileCache chiede. */
		size_t GetByteSize() const
		{
			return sizeof(FVectorTile)
			     + Features.size() * sizeof(FVectorFeature)
			     + Points.size() * sizeof(FVectorPoint);
		}
	};

	namespace Detail
	{
		// Nomi propri e non quelli di ImageTileFormat.h: i due header finiscono
		// nella stessa unita' di traduzione, e due funzioni omonime con lo
		// stesso corpo sarebbero una ridefinizione.
		inline uint16_t ReadVectorU16(const uint8_t* Bytes)
		{
			return static_cast<uint16_t>(Bytes[0] | (Bytes[1] << 8));
		}

		inline uint32_t ReadVectorU32(const uint8_t* Bytes)
		{
			return static_cast<uint32_t>(Bytes[0])
			     | (static_cast<uint32_t>(Bytes[1]) << 8)
			     | (static_cast<uint32_t>(Bytes[2]) << 16)
			     | (static_cast<uint32_t>(Bytes[3]) << 24);
		}

		inline int16_t ReadVectorI16(const uint8_t* Bytes)
		{
			return static_cast<int16_t>(ReadVectorU16(Bytes));
		}
	}

	enum class EVectorTileParseResult
	{
		Ok,
		TooShort,
		WrongMagic,
		WrongVersion,
		BadExtent,
		WrongSize,
		InconsistentPoints,
	};

	inline const char* DescribeVectorParseResult(EVectorTileParseResult Result)
	{
		switch (Result)
		{
		case EVectorTileParseResult::Ok:                 return "ok";
		case EVectorTileParseResult::TooShort:           return "file piu' corto dell'header";
		case EVectorTileParseResult::WrongMagic:         return "non e' una tile vettoriale (magic diverso da GWVT)";
		case EVectorTileParseResult::WrongVersion:       return "versione del formato non gestita";
		case EVectorTileParseResult::BadExtent:          return "extent nullo: header rovinato";
		case EVectorTileParseResult::WrongSize:          return "dimensione del file incoerente con linee e punti dichiarati";
		case EVectorTileParseResult::InconsistentPoints: return "le linee usano piu' punti di quelli presenti";
		}
		return "sconosciuto";
	}

	/** Interpreta un file .gvt. In caso di errore Out resta in uno stato qualunque. */
	inline EVectorTileParseResult ParseVectorTile(const uint8_t* Bytes, size_t ByteCount,
	                                              FVectorTile& Out)
	{
		if (ByteCount < static_cast<size_t>(VectorTileHeaderBytes))
		{
			return EVectorTileParseResult::TooShort;
		}
		if (Detail::ReadVectorU32(Bytes + 0) != VectorTileMagic)
		{
			return EVectorTileParseResult::WrongMagic;
		}
		if (Detail::ReadVectorU16(Bytes + 4) != VectorTileVersion)
		{
			return EVectorTileParseResult::WrongVersion;
		}

		// Bytes + 6: due byte di flag, oggi zero.
		Out.Key.Level = Detail::ReadVectorU32(Bytes + 8);
		Out.Key.X = Detail::ReadVectorU32(Bytes + 12);
		Out.Key.Y = Detail::ReadVectorU32(Bytes + 16);
		Out.Extent = static_cast<int32_t>(Detail::ReadVectorU16(Bytes + 20));
		Out.Buffer = static_cast<int32_t>(Detail::ReadVectorU16(Bytes + 22));
		const uint32_t FeatureCount = Detail::ReadVectorU32(Bytes + 24);
		const uint32_t PointCount = Detail::ReadVectorU32(Bytes + 28);

		if (Out.Extent <= 0)
		{
			return EVectorTileParseResult::BadExtent;
		}

		// Il controllo in 64 bit: con conteggi enormi (un file rovinato) la
		// moltiplicazione in 32 bit potrebbe fare il giro e sembrare giusta.
		const uint64_t Expected = static_cast<uint64_t>(VectorTileHeaderBytes)
		                        + static_cast<uint64_t>(FeatureCount) * VectorFeatureRecordBytes
		                        + static_cast<uint64_t>(PointCount) * 4u;
		if (static_cast<uint64_t>(ByteCount) != Expected)
		{
			return EVectorTileParseResult::WrongSize;
		}

		Out.Features.clear();
		Out.Points.clear();
		Out.Features.reserve(FeatureCount);
		Out.Points.resize(PointCount);

		const uint8_t* PointBytes = Bytes + VectorTileHeaderBytes
		                          + static_cast<size_t>(FeatureCount) * VectorFeatureRecordBytes;
		for (uint32_t Index = 0; Index < PointCount; ++Index)
		{
			Out.Points[Index].X = Detail::ReadVectorI16(PointBytes + Index * 4u);
			Out.Points[Index].Y = Detail::ReadVectorI16(PointBytes + Index * 4u + 2u);
		}

		uint32_t Cursor = 0;
		for (uint32_t Index = 0; Index < FeatureCount; ++Index)
		{
			const uint8_t* Record = Bytes + VectorTileHeaderBytes
			                      + static_cast<size_t>(Index) * VectorFeatureRecordBytes;
			FVectorFeature Feature;
			Feature.Class = static_cast<ERoadClass>(Record[0]);
			Feature.Flags = Record[1];
			Feature.Layer = static_cast<int8_t>(Record[2]);
			// Record[3]: riservato.
			Feature.WidthDecimetres = Detail::ReadVectorU16(Record + 4);
			// Record + 6: due byte riservati.
			Feature.PointCount = Detail::ReadVectorU32(Record + 8);
			Feature.FirstPoint = Cursor;

			if (Feature.PointCount > PointCount - Cursor)
			{
				return EVectorTileParseResult::InconsistentPoints;
			}
			Cursor += Feature.PointCount;

			if (Feature.PointCount > 0)
			{
				const FVectorPoint& First = Out.Points[Feature.FirstPoint];
				Feature.MinX = Feature.MaxX = First.X;
				Feature.MinY = Feature.MaxY = First.Y;
				for (uint32_t P = 1; P < Feature.PointCount; ++P)
				{
					const FVectorPoint& Point = Out.Points[Feature.FirstPoint + P];
					Feature.MinX = std::min(Feature.MinX, Point.X);
					Feature.MaxX = std::max(Feature.MaxX, Point.X);
					Feature.MinY = std::min(Feature.MinY, Point.Y);
					Feature.MaxY = std::max(Feature.MaxY, Point.Y);
				}
			}
			Out.Features.push_back(Feature);
		}

		if (Cursor != PointCount)
		{
			return EVectorTileParseResult::InconsistentPoints;
		}
		return EVectorTileParseResult::Ok;
	}

	// -------------------------------------------------------------------------
	//  Indice di livello: quali tile esistono, con quante linee.
	//  Magic diverso da quelli delle quote (GWIX) e delle immagini (GWIA).
	// -------------------------------------------------------------------------
	inline constexpr uint32_t VectorIndexMagic = 0x49565747u;   // "GWVI"
	inline constexpr int32_t VectorIndexHeaderBytes = 16;
	inline constexpr int32_t VectorIndexRecordBytes = 12;

	struct FVectorIndexEntry
	{
		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t FeatureCount = 0;
	};

	inline bool ParseVectorIndex(const uint8_t* Bytes, size_t ByteCount, uint32_t ExpectedLevel,
	                             std::vector<FVectorIndexEntry>& Out, const char*& OutError)
	{
		OutError = nullptr;
		if (ByteCount < static_cast<size_t>(VectorIndexHeaderBytes))
		{
			OutError = "indice piu' corto del suo header";
			return false;
		}
		if (Detail::ReadVectorU32(Bytes + 0) != VectorIndexMagic)
		{
			OutError = "non e' l'indice di un dataset vettoriale (quote o immagini?)";
			return false;
		}
		if (Detail::ReadVectorU16(Bytes + 4) != 1)
		{
			OutError = "versione dell'indice non gestita";
			return false;
		}

		const uint32_t Level = Detail::ReadVectorU32(Bytes + 8);
		const uint32_t Count = Detail::ReadVectorU32(Bytes + 12);
		if (Level != ExpectedLevel)
		{
			OutError = "l'indice dichiara un livello diverso da quello della cartella";
			return false;
		}
		const uint64_t Expected = static_cast<uint64_t>(VectorIndexHeaderBytes)
		                        + static_cast<uint64_t>(Count) * VectorIndexRecordBytes;
		if (static_cast<uint64_t>(ByteCount) != Expected)
		{
			OutError = "dimensione dell'indice incoerente con il numero di voci";
			return false;
		}

		Out.clear();
		Out.reserve(Count);
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			const uint8_t* Record = Bytes + VectorIndexHeaderBytes
			                      + static_cast<size_t>(Index) * VectorIndexRecordBytes;
			FVectorIndexEntry Entry;
			Entry.X = Detail::ReadVectorU32(Record + 0);
			Entry.Y = Detail::ReadVectorU32(Record + 4);
			Entry.FeatureCount = Detail::ReadVectorU32(Record + 8);
			Out.push_back(Entry);
		}
		return true;
	}
}
