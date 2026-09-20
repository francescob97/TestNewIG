// =============================================================================
//  ImageTileFormat.h -- Lettura delle tile di immagine (.gim). STRATO: C++ PURO.
//
//  Header-only come tutto lo strato puro: in Unreal ogni modulo e' una DLL e un
//  simbolo definito in un .cpp non sarebbe visibile agli altri.
//
//  QUESTO FILE NON DECOMPRIME NIENTE
//  Legge l'header e consegna il payload compresso cosi' com'e'. La decodifica
//  del JPEG spetta allo strato Unreal, che ha IImageWrapper, cioe' un decoder
//  gia' scritto, veloce e provato da anni. Tirare dentro una libreria JPEG per
//  restare "puri" sarebbe purismo vero, del tipo che costa e non rende.
//
//  La divisione e' comunque utile: il parsing dell'header, i controlli di
//  coerenza e la geometria sono verificabili senza il motore, ed e' li' che
//  stanno gli errori che fanno perdere tempo.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "Tiles/TileKey.h"

namespace GeoWorld::Tiles
{
	/**
	 * Lato della tile in pixel.
	 *
	 * 256, non 129 come i post delle quote, e la differenza e' sostanziale:
	 *   - i post sono registrati SUI NODI e il bordo e' condiviso con la tile
	 *     vicina, perche' le mesh adiacenti devono combaciare esattamente;
	 *   - i pixel sono registrati SULLE AREE e coprono il rettangolo senza
	 *     sovrapposizione, perche' una colonna condivisa verrebbe disegnata due
	 *     volte e sfarfallerebbe.
	 *
	 * 256 = 2 x 128 da' anche esattamente due pixel per cella di terreno, e fa
	 * si' che l'immagine di livello L abbia la risoluzione al suolo del terreno
	 * di livello L+1.
	 */
	inline constexpr int32_t TilePixels = 256;

	inline constexpr uint32_t ImageTileMagic = 0x4D495747u;  // "GWIM" little-endian
	inline constexpr uint16_t ImageTileVersion = 1;
	inline constexpr int32_t ImageTileHeaderBytes = 32;

	enum class EImagePayload : uint8_t
	{
		Jpeg = 0,
		Png = 1,
		Bc1 = 2,        // non ancora prodotto dalla pipeline: vedi fase6-design.md
	};

	/** bit0: la tile contiene pixel di riempimento (copertura parziale). */
	inline constexpr uint16_t ImageFlagHasFilledPixels = 1 << 0;

	struct FImageTile
	{
		FTileKey Key;
		int32_t Width = 0;
		int32_t Height = 0;
		EImagePayload Payload = EImagePayload::Jpeg;

		/** Percentuale del rettangolo effettivamente coperta dall'ortofoto. */
		uint8_t CoveragePercent = 0;
		uint16_t Flags = 0;

		/** Byte compressi, cosi' come stanno nel file. */
		std::vector<uint8_t> CompressedBytes;

		/**
		 * Pixel decodificati, in formato BGRA a 8 bit per canale.
		 *
		 * QUESTO STRATO NON LO RIEMPIE MAI. Lo riempie il worker dello strato
		 * Unreal, che ha IImageWrapper. Il campo sta qui e non in una struttura
		 * a parte per una ragione pratica: e' quello che la cache deve contare
		 * nel proprio budget, e una cache che misura meta' dell'oggetto e'
		 * peggio di una senza budget.
		 *
		 * BGRA e non RGB perche' e' l'ordine che PF_B8G8R8A8 si aspetta: la
		 * conversione avviene una volta sola, nel worker, invece che a ogni
		 * riga durante il caricamento della texture.
		 */
		std::vector<uint8_t> Pixels;

		bool HasFilledPixels() const { return (Flags & ImageFlagHasFilledPixels) != 0; }

		bool IsValid() const
		{
			return Width == TilePixels && Height == TilePixels && !CompressedBytes.empty();
		}

		/** Ha i pixel pronti da caricare in una texture? */
		bool IsDecoded() const
		{
			return Pixels.size() == static_cast<size_t>(Width) * Height * 4;
		}

		size_t GetByteSize() const
		{
			return sizeof(FImageTile) + CompressedBytes.size() + Pixels.size();
		}
	};

	namespace Detail
	{
		inline uint16_t ReadImageU16(const uint8_t* Bytes) { return static_cast<uint16_t>(Bytes[0] | (Bytes[1] << 8)); }

		inline uint32_t ReadImageU32(const uint8_t* Bytes)
		{
			return static_cast<uint32_t>(Bytes[0])
			     | (static_cast<uint32_t>(Bytes[1]) << 8)
			     | (static_cast<uint32_t>(Bytes[2]) << 16)
			     | (static_cast<uint32_t>(Bytes[3]) << 24);
		}
	}

	enum class EImageTileParseResult
	{
		Ok,
		TooShort,
		WrongMagic,
		WrongVersion,
		WrongSize,
		TruncatedPayload,
	};

	inline const char* DescribeParseResult(EImageTileParseResult Result)
	{
		switch (Result)
		{
		case EImageTileParseResult::Ok:               return "ok";
		case EImageTileParseResult::TooShort:         return "file piu' corto dell'header";
		case EImageTileParseResult::WrongMagic:       return "non e' una tile di immagine (magic diverso da GWIM)";
		case EImageTileParseResult::WrongVersion:     return "versione del formato non gestita";
		case EImageTileParseResult::WrongSize:        return "dimensioni diverse da 256x256";
		case EImageTileParseResult::TruncatedPayload: return "payload troncato rispetto a quanto dichiarato";
		}
		return "sconosciuto";
	}

	/**
	 * Interpreta un file .gim.
	 *
	 * Non copia il payload piu' del necessario: una sola copia dal blob letto
	 * dal disco al vector della tile. Il chiamante puo' buttare il blob.
	 */
	inline EImageTileParseResult ParseImageTile(const uint8_t* Bytes, size_t ByteCount,
	                                            FImageTile& Out)
	{
		if (ByteCount < static_cast<size_t>(ImageTileHeaderBytes))
		{
			return EImageTileParseResult::TooShort;
		}

		if (Detail::ReadImageU32(Bytes + 0) != ImageTileMagic)
		{
			return EImageTileParseResult::WrongMagic;
		}
		if (Detail::ReadImageU16(Bytes + 4) != ImageTileVersion)
		{
			return EImageTileParseResult::WrongVersion;
		}

		Out.Flags = Detail::ReadImageU16(Bytes + 6);
		Out.Key.Level = Detail::ReadImageU32(Bytes + 8);
		Out.Key.X = Detail::ReadImageU32(Bytes + 12);
		Out.Key.Y = Detail::ReadImageU32(Bytes + 16);
		Out.Width = static_cast<int32_t>(Detail::ReadImageU16(Bytes + 20));
		Out.Height = static_cast<int32_t>(Detail::ReadImageU16(Bytes + 22));
		Out.Payload = static_cast<EImagePayload>(Bytes[24]);
		Out.CoveragePercent = Bytes[25];
		// Bytes + 26: due byte riservati.
		const uint32_t PayloadSize = Detail::ReadImageU32(Bytes + 28);

		if (Out.Width != TilePixels || Out.Height != TilePixels)
		{
			return EImageTileParseResult::WrongSize;
		}

		if (ByteCount < static_cast<size_t>(ImageTileHeaderBytes) + PayloadSize)
		{
			return EImageTileParseResult::TruncatedPayload;
		}

		Out.CompressedBytes.assign(Bytes + ImageTileHeaderBytes,
		                           Bytes + ImageTileHeaderBytes + PayloadSize);
		return EImageTileParseResult::Ok;
	}

	// -------------------------------------------------------------------------
	//  Indice di livello delle immagini
	//
	//  Magic diverso da quello delle quote di proposito: aprire un indice di
	//  immagini credendolo di quote deve fallire subito e con un messaggio
	//  chiaro, non leggere numeri a caso e produrre un dataset che sembra
	//  funzionare finche' non si guarda.
	// -------------------------------------------------------------------------
	inline constexpr uint32_t ImageIndexMagic = 0x41495747u;   // "GWIA"
	inline constexpr int32_t ImageIndexHeaderBytes = 16;
	inline constexpr int32_t ImageIndexRecordBytes = 12;

	struct FImageIndexEntry
	{
		uint32_t X = 0;
		uint32_t Y = 0;
		uint8_t CoveragePercent = 0;
		EImagePayload Payload = EImagePayload::Jpeg;
	};

	inline bool ParseImageIndex(const uint8_t* Bytes, size_t ByteCount, uint32_t ExpectedLevel,
	                            std::vector<FImageIndexEntry>& Out, const char*& OutError)
	{
		OutError = nullptr;

		if (ByteCount < static_cast<size_t>(ImageIndexHeaderBytes))
		{
			OutError = "indice piu' corto del suo header";
			return false;
		}
		if (Detail::ReadImageU32(Bytes + 0) != ImageIndexMagic)
		{
			OutError = "non e' un indice di immagini: sembra quello di un dataset di quote";
			return false;
		}
		if (Detail::ReadImageU16(Bytes + 4) != 1)
		{
			OutError = "versione dell'indice non gestita";
			return false;
		}

		const uint32_t Level = Detail::ReadImageU32(Bytes + 8);
		const uint32_t Count = Detail::ReadImageU32(Bytes + 12);

		if (Level != ExpectedLevel)
		{
			OutError = "l'indice dichiara un livello diverso da quello della cartella";
			return false;
		}

		const size_t Expected = static_cast<size_t>(ImageIndexHeaderBytes)
		                      + static_cast<size_t>(Count) * ImageIndexRecordBytes;
		if (ByteCount != Expected)
		{
			OutError = "dimensione dell'indice incoerente con il numero di voci dichiarate";
			return false;
		}

		Out.clear();
		Out.reserve(Count);
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			const uint8_t* Record = Bytes + ImageIndexHeaderBytes
			                      + static_cast<size_t>(Index) * ImageIndexRecordBytes;
			FImageIndexEntry Entry;
			Entry.X = Detail::ReadImageU32(Record + 0);
			Entry.Y = Detail::ReadImageU32(Record + 4);
			Entry.CoveragePercent = Record[8];
			Entry.Payload = static_cast<EImagePayload>(Record[9]);
			Out.push_back(Entry);
		}
		return true;
	}
}
